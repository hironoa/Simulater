/*
 * sim_main.c - コマンドライン
 *
 *   sim run [--abs X.abs] [--cfg sim.cfg] <テスト.csv>... [-o 出力フォルダ] [--expect-actual] [--fixed-time] [--quiet]
 *   sim gui [--abs X.abs] [--cfg sim.cfg] [--port 8765] [--period-ms 5] [--func 関数名] [--speed 1] [--no-browser] [--record-max N]
 *   sim list [--abs X.abs] [--cfg sim.cfg] [文字列]
 *   sim info [--abs X.abs] [--cfg sim.cfg]
 *   sim --version
 *   共通: --verbose（-v）で読み込み中の警告と詳細を出す
 *
 * 終了コード: 0 = すべて OK、1 = NG あり、2 = 実行できないエラー
 */
#define _CRT_SECURE_NO_WARNINGS
#include "sim_backend.h"
#include "sim_csv.h"
#include "sim_expr.h"
#include "sim_gui.h"
#include "sim_platform.h"

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
            "  sim list --abs <X.abs> [--cfg sim.cfg] [文字列]\n"
            "  sim info --abs <X.abs> [--cfg sim.cfg]\n"
            "  sim --version\n"
            "  共通: --verbose（-v）で読み込みの警告と詳細を出す。--abs を省くと設定ファイルの abs= を使う\n");
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
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_open.verbose = 1;
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
    return sim_ctx_new(b);
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
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) o.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) o.period_ms = atof(argv[++i]);
        else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) o.func = argv[++i];
        else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) o.speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--record-max") == 0 && i + 1 < argc) o.record_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--html-dir") == 0 && i + 1 < argc) o.html_dir = argv[++i];
        else if (strcmp(argv[i], "--no-browser") == 0) o.open_browser = 0;
        else { fprintf(stderr, "エラー: 不明なオプション %s\n", argv[i]); usage(); return 2; }
    }
    if (o.port <= 0 || o.port > 65535) { fprintf(stderr, "エラー: --port が不正です\n"); return 2; }
    if (o.period_ms <= 0) { fprintf(stderr, "エラー: --period-ms は正の数にしてください\n"); return 2; }
    c = open_ctx();
    if (!c) return 2;
    return sim_gui_run(c, &o);
}

static int cmd_list(int argc, char **argv)
{
    sim_ctx *c = open_ctx();
    const char *q = argc > 0 ? argv[0] : "";
    size_t i, n;
    sim_backend *b;
    if (!c) return 2;
    b = sim_ctx_backend(c);
    n = sim_ctx_sym_count(c);
    for (i = 0; i < n; i++) {
        char tn[256];
        const sim_var *v = b->var_at(b, i);
        if (!sim_strcasestr_found(sim_ctx_sym_name(c, i), q)) continue;
        sim_type_display(sim_ctx_sym_type(c, i), tn, sizeof tn);
        printf("var  0x%05llX %-40s %s%s  [%s]\n", (unsigned long long)v->addr, sim_ctx_sym_name(c, i), tn,
               sim_ctx_sym_const(c, i) ? " (const)" : "", v->file ? v->file : "");
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
    printf("変数: %zu 個、関数: %zu 個\n", sim_ctx_sym_count(c), b->func_count(b));
    sim_ctx_free(c);
    b->close(b);
    return 0;
}

int main(int argc, char **argv)
{
    sim_console_utf8();
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
