/*
 * sim_main.c - コマンドライン
 *
 *   sim run [--abs X.abs] [--cfg sim.cfg] <テスト.csv>... [-o 出力フォルダ] [--expect-actual] [--fixed-time] [--quiet]
 *   sim gui [--abs X.abs] [--cfg sim.cfg] [--port 8765] [--period-ms 5] [--func 関数名] [--speed 1] [--no-browser] [--record-max N]
 *   sim list [--abs X.abs] [--cfg sim.cfg] [--where] [文字列]
 *   sim info [--abs X.abs] [--cfg sim.cfg]
 *   sim --version
 *   共通: --verbose（-v）で読み込み中の警告と詳細を出す
 *         --defines パス、--cdef 名前[=値]、--mtpj X.mtpj（ヘッダの直アドレス定義とコンパイルスイッチ）
 *
 * 終了コード: 0 = すべて OK、1 = NG あり、2 = 実行できないエラー
 */
#define _CRT_SECURE_NO_WARNINGS
#include "sim_backend.h"
#include "sim_csv.h"
#include "sim_defines.h"
#include "sim_expr.h"
#include "sim_gui.h"
#include "sim_platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_VERSION "0.1.0"

static void usage(void)
{
    fprintf(stderr,
            "使い方:\n"
            "  sim run --abs <X.abs> [--cfg sim.cfg] <テスト.csv>... [-o 出力フォルダ] [--expect-actual] [--fixed-time] [--quiet]\n"
            "  sim gui --abs <X.abs> [--cfg sim.cfg] [--port 8765] [--period-ms 5] [--func 関数名] [--speed 1] [--no-browser] [--record-max N]\n"
            "  sim list --abs <X.abs> [--cfg sim.cfg] [--where] [文字列]\n"
            "  sim info --abs <X.abs> [--cfg sim.cfg] [--trace-startup] [--trace-head N] [--trace-tail N]\n"
            "  sim --version\n"
            "  共通: --verbose（-v）で読み込みの警告と詳細を出す。--abs を省くと設定ファイルの abs= を使う\n"
            "        --defines <パス> でヘッダの直アドレス定義を読む。--cdef <名前[=値]>（複数可）と --mtpj <X.mtpj> は\n"
            "        その #if / #ifdef の評価に使う定義マクロ。sim list --where は定義元のファイル名と行番号を表示する\n");
}

static sim_open_opts g_open;

/* 共通オプション（--abs、--cfg、--verbose）を取り除く。不正なら -1 */
static int take_common(int *argc, char **argv)
{
    int i, k = 0;
    for (i = 0; i < *argc; i++) {
        if ((strcmp(argv[i], "--abs") == 0 || strcmp(argv[i], "--cfg") == 0) && i + 1 >= *argc) {
            fprintf(stderr, "エラー: %s の後にファイル名がありません\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--abs") == 0) g_open.abs_path = argv[++i];
        else if (strcmp(argv[i], "--cfg") == 0) g_open.cfg_path = argv[++i];
        else if (!strcmp(argv[i], "--lmf-names")) g_open.lmf_names = 1;
        else if (!strcmp(argv[i], "--defines")) {
            if (i+1 >= *argc) { fprintf(stderr, "エラー: --defines の後にパスがありません\n"); return -1; }
            g_open.defines = sim_xrealloc(g_open.defines, (size_t)(g_open.ndefines+1)*sizeof *g_open.defines);
            g_open.defines[g_open.ndefines++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--cdef")) {
            if (i+1 >= *argc) { fprintf(stderr, "エラー: --cdef の後に 名前[=値] がありません\n"); return -1; }
            g_open.cdefs = sim_xrealloc(g_open.cdefs, (size_t)(g_open.ncdefs+1)*sizeof *g_open.cdefs);
            g_open.cdefs[g_open.ncdefs++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--mtpj")) {
            if (i+1 >= *argc) { fprintf(stderr, "エラー: --mtpj の後にファイル名がありません\n"); return -1; }
            g_open.mtpj_path = argv[++i];
        }
        else if (!strncmp(argv[i], "--csv-init=", 11)) g_open.csv_init = argv[i] + 11;
        else if (strcmp(argv[i], "--trace-startup") == 0) g_open.trace_startup = 1;
        else if (!strcmp(argv[i], "--trace-head") || !strcmp(argv[i], "--trace-tail")) {
            uint64_t n;
            int head = !strcmp(argv[i], "--trace-head");
            if (i + 1 >= *argc || sim_parse_uint(argv[++i], 10000, &n) || !n) {
                fprintf(stderr, "エラー: 診断の表示件数は 1〜10000 にしてください\n");
                return -1;
            }
            if (head) g_open.trace_head = (unsigned)n; else g_open.trace_tail = (unsigned)n;
        }
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_open.verbose++;
        else argv[k++] = argv[i];
    }
    *argc = k;
    return 0;
}

static sim_ctx *open_ctx(void)
{
    char err[2048];
    sim_backend *b;
    g_open.exe_dir = sim_exe_dir();
    b = sim_backend_open(&g_open, err, sizeof err);
    if (!b) {
        fprintf(stderr, "エラー: ターゲットを開けません: %s\n", err);
        return NULL;
    }
    {
        sim_ctx *c = sim_ctx_new(b);
        if (sim_defines_load(c, &g_open, err, sizeof err)) {
            fprintf(stderr, "エラー: 定義を読めません: %s\n", err);
            sim_ctx_free(c); b->close(b); return NULL;
        }
        return c;
    }
}

static int cmd_run(int argc, char **argv)
{
    sim_csv_opts o;
    sim_csv_stats st;
    const char **files;
    int nfiles = 0, i;
    sim_ctx *c;
    uint64_t t0;
    memset(&o, 0, sizeof o);
    memset(&st, 0, sizeof st);
    o.outdir = "result";
    files = calloc((size_t)argc + 1, sizeof *files);
    if (!files) return 2;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) o.outdir = argv[++i];
        else if (strcmp(argv[i], "--expect-actual") == 0) o.expect_actual = 1;
        else if (strcmp(argv[i], "--fixed-time") == 0) o.fixed_time = 1;
        else if (strcmp(argv[i], "--quiet") == 0) o.quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "エラー: 不明なオプション %s\n", argv[i]); usage(); free(files); return 2; }
        else files[nfiles++] = argv[i];
    }
    if (nfiles == 0) { fprintf(stderr, "エラー: テスト CSV を指定してください\n"); usage(); free(files); return 2; }
    o.inputs = files;
    o.ninputs = nfiles;
    if (sim_csv_check_outputs(&o)) { free(files); return 2; }
    c = open_ctx();
    if (!c) { free(files); return 2; }
    t0 = sim_now_ns();
    for (i = 0; i < nfiles; i++) sim_csv_run_file(c, files[i], &o, &st);
    fprintf(stdout, "合計: 総数 %ld, OK %ld, NG %ld, NO Check %ld", st.total, st.ok, st.ng, st.nocheck);
    if (st.errors) fprintf(stdout, ", エラー %d", st.errors);
    fprintf(stdout, "\n実行時間: %.3f 秒（対象関数 %.3f 秒）\n", (double)(sim_now_ns() - t0) / 1e9, st.seconds);
    free(files);
    {
        sim_backend *b = sim_ctx_backend(c);
        if (b->exec_stats) {
            sim_exec_stats es;
            double sec = st.seconds;
            uint64_t n;
            b->exec_stats(b, &es);
            n = es.insns >= es.startup_insns ? es.insns - es.startup_insns : es.insns;
            fprintf(stdout, "実行命令数: %llu（スタートアップ %llu を除く）、呼び出し %llu 回、所要時間 %.3f 秒、%.1f 百万命令/秒\n",
                    (unsigned long long)n, (unsigned long long)es.startup_insns, (unsigned long long)es.calls, sec,
                    sec > 0 ? (double)n / sec / 1e6 : 0.0);
        }
        sim_ctx_free(c);
        b->close(b);
    }
    if (st.errors) return 2;
    return st.ng ? 1 : 0;
}

static int cmd_gui(int argc, char **argv)
{
    sim_gui_opts o;
    sim_ctx *c;
    int i;
    memset(&o, 0, sizeof o);
    o.port = 8765;
    o.period_ms = 5;
    o.speed = 1;
    o.open_browser = 1;
    o.record_max = 200000;
    /* 数値は文字列全体を読み、範囲外や末尾のごみ（8765x、1e400、nan）を別の値にせずエラーにする */
    for (i = 0; i < argc; i++) {
        uint64_t u;
        double d;
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            i++;
            if (sim_parse_uint(argv[i], 65535, &u) || u == 0) {
                fprintf(stderr, "エラー: --port %s は 1〜65535 の整数にしてください\n", argv[i]);
                return 2;
            }
            o.port = (int)u;
        } else if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) {
            i++;
            if (sim_parse_double(argv[i], &d) || !(d >= 0.01 && d <= 60000)) {
                fprintf(stderr, "エラー: --period-ms %s は 0.01〜60000 の数にしてください\n", argv[i]);
                return 2;
            }
            o.period_ms = d;
        } else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) o.func = argv[++i];
        else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            i++;
            if (sim_parse_double(argv[i], &d) || !(d == 0 || (d >= 0.1 && d <= 100))) {
                fprintf(stderr, "エラー: --speed %s は 0（最速）か 0.1〜100 の数にしてください\n", argv[i]);
                return 2;
            }
            o.speed = d;
        } else if (strcmp(argv[i], "--record-max") == 0 && i + 1 < argc) {
            i++;
            if (sim_parse_uint(argv[i], INT_MAX, &u) || u == 0) {
                fprintf(stderr, "エラー: --record-max %s は 1〜%d の整数にしてください\n", argv[i], INT_MAX);
                return 2;
            }
            o.record_max = (int)u;
        }
        else if (strcmp(argv[i], "--html-dir") == 0 && i + 1 < argc) o.html_dir = argv[++i];
        else if (strcmp(argv[i], "--no-browser") == 0) o.open_browser = 0;
        else { fprintf(stderr, "エラー: 不明なオプション %s\n", argv[i]); usage(); return 2; }
    }
    c = open_ctx();
    if (!c) return 2;
    return sim_gui_run(c, &o);
}

static int cmd_list(int argc, char **argv)
{
    sim_ctx *c;
    const char *q = "";
    size_t i, n;
    int where = 0, a;
    sim_backend *b;
    for (a = 0; a < argc; a++) {
        if (!strcmp(argv[a], "--where")) where = 1;
        else if (argv[a][0] == '-' && argv[a][1] == '-') { fprintf(stderr, "エラー: 不明なオプション %s\n", argv[a]); usage(); return 2; }
        else q = argv[a];
    }
    c = open_ctx();
    if (!c) return 2;
    b = sim_ctx_backend(c);
    n = sim_ctx_sym_count(c);
    for (i = 0; i < n; i++) {
        char tn[256];
        const sim_var *v = sim_ctx_sym_var(c, i);
        if (!sim_strcasestr_found(sim_ctx_sym_name(c, i), q)) continue;
        sim_type_display(sim_ctx_sym_type(c, i), tn, sizeof tn);
        printf("var  0x%05llX %-40s %s%s  [%s", (unsigned long long)v->addr, sim_ctx_sym_name(c, i), tn,
               sim_ctx_sym_const(c, i) ? " (const)" : "", v->file ? v->file : "");
        /* --where: ヘッダ（defines）や var / varfile から取り込んだ定義は、定義元の行番号も表示する */
        if (where && sim_ctx_sym_line(c, i)) printf(":%u", sim_ctx_sym_line(c, i));
        printf("]\n");
    }
    n = b->func_count(b);
    for (i = 0; i < n; i++) {
        const sim_func *f = b->func_at(b, i);
        int k;
        if (!sim_strcasestr_found(f->name, q)) continue;
        printf("func 0x%05llX %s%s(", (unsigned long long)f->addr, f->is_external ? "" : "static ", f->name);
        for (k = 0; k < f->nparams; k++) {
            char tn[256];
            sim_type_display(f->param_types[k], tn, sizeof tn);
            printf("%s%s %s", k ? ", " : "", tn, f->param_names[k] ? f->param_names[k] : "");
        }
        {
            char tn[256];
            sim_type_display(f->ret, tn, sizeof tn);
            printf("%s) -> %s  [%s]\n", f->variadic ? ", ..." : "", tn, f->file ? f->file : "");
        }
    }
    sim_ctx_free(c);
    b->close(b);
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    sim_ctx *c;
    sim_backend *b;
    (void)argv;
    if (argc > 0) { fprintf(stderr, "エラー: 不明な引数 %s\n", argv[0]); usage(); return 2; }
    c = open_ctx();
    if (!c) return 2;
    b = sim_ctx_backend(c);
    printf("バックエンド: %s\n", b->name);
    if (b->print_info) b->print_info(b, stdout);
    sim_ctx_print_definition_info(c, stdout);
    printf("変数: %zu 個、関数: %zu 個\n", sim_ctx_sym_count(c), b->func_count(b));
    sim_ctx_free(c);
    b->close(b);
    return 0;
}

int main(int argc, char **argv)
{
    sim_console_utf8();
    sim_utf8_args(&argc, &argv);
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 4096);
#endif
    if (argc < 2) { usage(); return 2; }
    {
        int n = argc - 2;
        if (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "gui") == 0 || strcmp(argv[1], "list") == 0 ||
            strcmp(argv[1], "info") == 0) {
            if (take_common(&n, argv + 2)) { usage(); return 2; }
        }
        if (strcmp(argv[1], "run") == 0) return cmd_run(n, argv + 2);
        if (strcmp(argv[1], "gui") == 0) return cmd_gui(n, argv + 2);
        if (strcmp(argv[1], "list") == 0) return cmd_list(n, argv + 2);
        if (strcmp(argv[1], "info") == 0) return cmd_info(n, argv + 2);
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        printf("sim %s\n", SIM_VERSION);
        return 0;
    }
    usage();
    return 2;
}
