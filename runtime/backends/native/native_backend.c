/*
 * native_backend.c - PC 用 DLL の方式（exec = native）。段階 2: 変数と関数
 *
 * やること:
 *   1. CS+ のプロジェクトファイル（mtpj）から、ビルド対象の .c、インクルード・パス、定義・定義解除マクロを取る。
 *      ドライバやスタブなど、製品とは別に管理するソースは native_src で足す。
 *   2. 各ソースの先頭に rt/sim_native_pre.h を差し込み（-include）、clang で並列にコンパイルする。
 *   3. オブジェクトを 1 つの DLL にまとめる（C ライブラリなし。LLVM の配布物の clang と lld-link だけで作れる）。
 *   4. DLL を読み込み、デバッグ情報から変数と関数を取る。
 *   5. 0xF0000〜0xFFFFF に、同じ番地のメモリを確保する（番地を直接書いたレジスタや RAM のアクセス用）。
 *
 * キャッシュのフォルダは、設定ファイルの隣の .simnative/<mtpj の名前>-<鍵>/（native_dir で変えられる）。
 * 鍵は「コンパイラの版、コンパイルの引数、差し込むヘッダー、DLL に入れる部品」から作る。フォルダの中:
 *   <名前>-<パスの鍵>.obj   ソース 1 つのオブジェクト
 *   <名前>-<パスの鍵>.d     clang が書いた、そのソースが読んだファイルの一覧
 *   <名前>-<パスの鍵>.sig   ソースと、読んだファイルすべての中身から作った値。同じならコンパイルし直さない
 *   native-N.dll            できあがり（読み込み中のファイルは上書きできないので、作るたびに番号を進める）
 *   current.txt             いま使う DLL の名前
 *   link.sig                DLL に入れたオブジェクトと、その中身から作った値
 *   cc.log / cc_times.txt   コンパイラの出力 / コンパイルの所要秒数
 *
 * 今の方式（exec = interp / xlate）と違い、失敗したときに戻る先がない。コンパイラがない、コンパイルに
 * 失敗した、番地を確保できない、のどれもエラーにする。
 *
 * まだできないこと（docs/DESIGN.md の 4.3 節にも書く）:
 *   - スタブの切り替え、GUI
 *   - #asm 〜 #endasm、`short int` などの書き直し、標準ヘッダー
 *   - Windows 以外での実行
 */
#include "native_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rl78/rl78_be.h"
#include "sim_cc.h"
#include "sim_cfg.h"
#include "sim_mtpj.h"
#include "sim_platform.h"
#include "sim_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define WIN_LO 0xF0000u          /* 番地を再現する範囲 */
#define WIN_HI 0x100000u
#define HEAP_SIZE 0x100000u      /* $ptr 用の領域 */
#define MAX_ARGS 8
#define MAX_RANGES 64

typedef struct {
    uint64_t lo, hi;             /* [lo, hi) */
    int writable;
    uint8_t *init;               /* リセットで戻す中身（writable のとき。NULL なら 0 で埋める） */
} range_t;

typedef struct {
    char *src;                   /* ソースのパス */
    char *base;                  /* キャッシュの中の名前（拡張子なし） */
    char *obj, *dep, *sig;
    int pre;                     /* sim_native_pre.h を差し込む */
    int compile;                 /* 今回コンパイルする */
    const char **argv;
} unit_t;

typedef struct {
    sim_backend b;
    sim_cfg cfg;
    int quiet;
    char *cc, *rt_dir, *dir, *dll, *mtpj_path, *mode_name;
    int from_mtpj, from_extra, files_total, files_off;
    char **srcs; int nsrcs;
    char **asms; int nasms;
    char **incs; int nincs;
    char **macros; int nmacros;
    rl78_symtab st;
    rl78_warns w;
    char *dll_data;             /* DLL のファイルを閉じるまで保持する */
    int opt;
    int ncompiled, nreused, linked;
    double build_sec;
    void *lib;
    range_t ranges[MAX_RANGES]; int nranges;
    uint8_t *heap; size_t heap_used;
    uint64_t calls;
} nat;

/* ---- 小物 ---- */

static void say(nat *z, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void say(nat *z, const char *fmt, ...)
{
    va_list ap;
    if (z->quiet) return;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void say_line(void *ctx, const char *line) { say(ctx, "%s", line); }

static void push(char ***list, int *n, char *s)
{
    *list = sim_xrealloc(*list, sizeof **list * (size_t)(*n + 1));
    (*list)[(*n)++] = s;
}

static void push_macro(nat *z, const char *flag, const char *value)
{
    sim_buf b;
    sim_buf_init(&b);
    sim_buf_printf(&b, "%s%s", flag, value);
    push(&z->macros, &z->nmacros, sim_xstrdup(sim_buf_cstr(&b)));
    sim_buf_free(&b);
}

static int is_abs_path(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

/* path のフォルダの部分（呼び出し側で free）。区切りがなければ "." */
static char *dir_of(const char *path)
{
    const char *base = sim_basename(path);
    size_t n = (size_t)(base - path);
    char *d;
    if (n == 0) return sim_xstrdup(".");
    d = sim_xstrndup(path, n);
    while (n > 1 && (d[n - 1] == '/' || d[n - 1] == '\\') && d[n - 2] != ':') d[--n] = 0;
    return d;
}

static char *resolve(const char *dir, const char *rel)
{
    return is_abs_path(rel) ? sim_xstrdup(rel) : sim_path_join(dir, rel);
}

static int ends_with_ci(const char *s, const char *ext)
{
    size_t n = strlen(s), m = strlen(ext), i;
    if (n < m) return 0;
    for (i = 0; i < m; i++) {
        int a = (unsigned char)s[n - m + i], b = (unsigned char)ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

static uint64_t hash_str(uint64_t h, const char *s)
{
    h = sim_cc_fnv(h, s, strlen(s));
    return sim_cc_fnv(h, "", 1);
}

/* ファイルの中身の値。読めなければ 0 を返して *ok = 0 */
static uint64_t hash_file(const char *path, int *ok)
{
    size_t n = 0;
    char *t = sim_read_file(path, &n);
    uint64_t h;
    if (!t) { *ok = 0; return 0; }
    h = sim_cc_fnv(SIM_CC_FNV_INIT, t, n);
    free(t);
    return h;
}

static void free_list(char **list, int n)
{
    int i;
    for (i = 0; i < n; i++) free(list[i]);
    free(list);
}

static void nat_free(nat *z)
{
    int i;
    if (!z) return;
    sim_cfg_free(&z->cfg);
    free(z->cc); free(z->rt_dir); free(z->dir); free(z->dll); free(z->mtpj_path); free(z->mode_name);
    free_list(z->srcs, z->nsrcs);
    free_list(z->asms, z->nasms);
    free_list(z->incs, z->nincs);
    free_list(z->macros, z->nmacros);
    rl78_symtab_free(&z->st);
    rl78_warns_free(&z->w);
    free(z->dll_data);
    for (i = 0; i < z->nranges; i++) free(z->ranges[i].init);
    free(z);
}

/* ---- ソースの一覧 ---- */

typedef struct { nat *z; const char *dir; } scan_t;

static void scan_visit(void *ctx, const char *name, int kind)
{
    scan_t *s = ctx;
    if (kind == 1 && ends_with_ci(name, ".c")) {
        push(&s->z->srcs, &s->z->nsrcs, sim_path_join(s->dir, name));
        s->z->from_extra++;
    }
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int collect_sources(nat *z, const sim_open_opts *o, char *err, size_t errlen)
{
    const char *s;
    int i;

    if (o->mtpj_path) z->mtpj_path = sim_xstrdup(o->mtpj_path);
    else if ((s = sim_cfg_get(&z->cfg, "mtpj"))) z->mtpj_path = sim_cfg_path(&z->cfg, s);
    if (z->mtpj_path) {
        sim_mtpj m;
        char merr[400], *mdir = dir_of(z->mtpj_path);
        if (sim_mtpj_load(&m, z->mtpj_path, sim_cfg_get(&z->cfg, "mtpj_mode"), merr, sizeof merr)) {
            snprintf(err, errlen, "native: %s", merr);
            free(mdir);
            return -1;
        }
        z->files_total = m.files_total;
        z->files_off = m.files_off;
        if (m.nmodes > 0 && m.mode >= 0 && m.mode < m.nmodes) z->mode_name = sim_xstrdup(m.modes[m.mode]);
        for (i = 0; i < m.nfiles; i++) {
            if (ends_with_ci(m.files[i], ".c")) { push(&z->srcs, &z->nsrcs, resolve(mdir, m.files[i])); z->from_mtpj++; }
            else if (ends_with_ci(m.files[i], ".asm") || ends_with_ci(m.files[i], ".s"))
                push(&z->asms, &z->nasms, sim_xstrdup(m.files[i]));
        }
        for (i = 0; i < m.nincs; i++) push(&z->incs, &z->nincs, resolve(mdir, m.incs[i]));
        for (i = 0; i < m.ndefs; i++) push_macro(z, "-D", m.defs[i]);
        for (i = 0; i < m.nundefs; i++) push_macro(z, "-U", m.undefs[i]);
        sim_mtpj_free(&m);
        free(mdir);
    }
    /* 製品とは別に管理するソース（ドライバ、スタブ）。フォルダならその直下の .c を名前の順に */
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        if (!strcmp(e->key, "native_src")) {
            char *p = sim_cfg_path(&z->cfg, e->value);
            int kind = sim_path_kind(p), first = z->nsrcs;
            e->used = 1;
            if (kind == 2) {
                scan_t sc;
                sc.z = z;
                sc.dir = p;
                sim_dir_each(p, scan_visit, &sc);
                qsort(z->srcs + first, (size_t)(z->nsrcs - first), sizeof *z->srcs, name_cmp);
            } else if (kind == 1) {
                push(&z->srcs, &z->nsrcs, sim_xstrdup(p));
                z->from_extra++;
            } else {
                snprintf(err, errlen, "sim.cfg:%d: native_src = %s が見つかりません", e->line, p);
                free(p);
                return -1;
            }
            free(p);
        } else if (!strcmp(e->key, "native_include")) {
            e->used = 1;
            push(&z->incs, &z->nincs, sim_cfg_path(&z->cfg, e->value));
        } else if (!strcmp(e->key, "cdef")) {
            e->used = 1;
            push_macro(z, "-D", e->value);
        }
    }
    for (i = 0; i < o->ncdefs; i++) push_macro(z, "-D", o->cdefs[i]);
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        if (!strcmp(e->key, "cundef")) {
            e->used = 1;
            push_macro(z, "-U", e->value);
        }
    }
    if (z->nsrcs == 0) {
        snprintf(err, errlen, "native: コンパイルする .c がありません。sim.cfg に mtpj = <製品の .mtpj>（または --mtpj）か、"
                              "native_src = <ドライバやスタブの .c、またはそのフォルダ> を書いてください");
        return -1;
    }
    for (i = 0; i < z->nsrcs; i++) {
        if (sim_path_kind(z->srcs[i]) != 1) {
            snprintf(err, errlen, "native: ソース %s が見つかりません（mtpj に登録されたファイルは、mtpj のフォルダからの相対パスで探します）", z->srcs[i]);
            return -1;
        }
    }
    return 0;
}

/* ---- コンパイル ---- */

/* clang が -MD で書いたファイル（make の書式）から、読んだファイルの一覧を取る。最初の 1 つがソース */
static void parse_dep(const char *text, char ***list, int *n)
{
    const char *p = text;
    sim_buf cur;
    /* 「ターゲット:」を飛ばす。ターゲットにドライブ名の : があるので、空白か行末が続く : を探す */
    for (; *p; p++)
        if (*p == ':' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\r' || p[1] == '\n' || !p[1])) { p++; break; }
    sim_buf_init(&cur);
    for (;; p++) {
        if (*p == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) { p += p[1] == '\r' ? 2 : 1; goto flush; }
        if (*p == '\\' && (p[1] == ' ' || p[1] == '#')) { sim_buf_addc(&cur, p[1]); p++; continue; }
        if (*p == '$' && p[1] == '$') { sim_buf_addc(&cur, '$'); p++; continue; }
        if (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { sim_buf_addc(&cur, *p); continue; }
flush:
        if (cur.len) { push(list, n, sim_xstrdup(sim_buf_cstr(&cur))); cur.len = 0; }
        if (!*p) break;
    }
    sim_buf_free(&cur);
}

/* 存在確認は build の間だけ共有する。パスのつづりは変えない。 */
typedef struct path_entry {
    char *path;
    int exists;
    struct path_entry *next;
} path_entry;

#define PATH_BUCKETS 4096
typedef struct { path_entry *buckets[PATH_BUCKETS]; } path_cache;

static int path_exists(path_cache *cache, const char *path)
{
    size_t bucket = (size_t)(hash_str(SIM_CC_FNV_INIT, path) % PATH_BUCKETS);
    path_entry *e;
    for (e = cache->buckets[bucket]; e; e = e->next)
        if (!strcmp(e->path, path)) return e->exists;
    e = sim_xmalloc(sizeof *e);
    e->path = sim_xstrdup(path);
    e->exists = sim_path_kind(path) == 1;
    e->next = cache->buckets[bucket];
    cache->buckets[bucket] = e;
    return e->exists;
}

static void path_cache_free(path_cache *cache)
{
    size_t i;
    for (i = 0; i < PATH_BUCKETS; i++) {
        path_entry *e = cache->buckets[i];
        while (e) {
            path_entry *next = e->next;
            free(e->path);
            free(e);
            e = next;
        }
    }
}

static void push_unique(char ***list, int *n, char *s)
{
    int i;
    for (i = 0; i < *n; i++)
        if (!strcmp((*list)[i], s)) { free(s); return; }
    push(list, n, s);
}

/* a と b の先頭 n 文字が同じなら 1。区切りの / と \ は同じとみなす（clang は依存パスの区切りを \ にそろえて
 * 書くが、こちらが持つインクルード・パスには / が混ざる） */
static int same_prefix(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i] == '\\' ? '/' : a[i], y = b[i] == '\\' ? '/' : b[i];
        if (x != y) return 0;
    }
    return 1;
}

/* 探索順で先に当たる場所に同名のファイルが置かれると、clang が読むファイルは変わるのに、前回読んだファイルの
 * 中身は変わらない。そこで、依存ファイルごとに「探索フォルダ + つづり」の形に分け、ほかの探索フォルダ
 * （インクルード・パスと、ソースや依存ファイルのあるフォルダ）に同じつづりのファイルがあるかを調べて、
 * あったものの一覧も値に混ぜる。置かれても消えても値が変わる */
static uint64_t hash_candidates(nat *z, path_cache *cache, uint64_t h, char **files, int n)
{
    char **dirs = NULL, **found = NULL;
    int ndirs = 0, nfound = 0, i, j, k;
    for (i = 0; i < z->nincs; i++)
        push_unique(&dirs, &ndirs, sim_xstrdup(z->incs[i]));
    for (i = 0; i < n; i++)
        push_unique(&dirs, &ndirs, dir_of(files[i]));
    for (i = 0; i < n; i++) {
        size_t len = strlen(files[i]);
        for (j = 0; j < ndirs; j++) {
            size_t m = strlen(dirs[j]);
            const char *rel;
            if (m >= len || !same_prefix(files[i], dirs[j], m)) continue;
            if (files[i][m] != '/' && files[i][m] != '\\') continue;
            rel = files[i] + m + 1;
            for (k = 0; k < ndirs; k++) {
                char *candidate;
                if (k == j) continue;
                candidate = sim_path_join(dirs[k], rel);
                if (path_exists(cache, candidate)) push(&found, &nfound, candidate);
                else free(candidate);
            }
        }
    }
    if (nfound > 1) qsort(found, (size_t)nfound, sizeof *found, name_cmp);
    for (i = 0; i < nfound; i++)
        if (!i || strcmp(found[i - 1], found[i])) h = hash_str(h, found[i]);
    free_list(found, nfound);
    free_list(dirs, ndirs);
    return h;
}

/* ソースと、読んだファイルすべての中身から作った値を文字列にする。読めないファイルがあれば NULL */
static char *make_sig(nat *z, path_cache *cache, char **files, int n)
{
    uint64_t h = SIM_CC_FNV_INIT;
    sim_buf out;
    int i, ok = 1;
    for (i = 0; i < n && ok; i++) {
        uint64_t fh = hash_file(files[i], &ok);
        h = sim_cc_fnv(h, &fh, sizeof fh);
    }
    if (!ok) return NULL;
    h = hash_candidates(z, cache, h, files, n);
    sim_buf_init(&out);
    sim_buf_printf(&out, "%016llx\n", (unsigned long long)h);
    for (i = 0; i < n; i++) { sim_buf_adds(&out, files[i]); sim_buf_addc(&out, '\n'); }
    (void)sim_buf_cstr(&out);   /* NUL で終える */
    return out.p;
}

/* 前回の .sig と、いまのファイルの中身が合えば 1 */
static int unit_fresh(nat *z, path_cache *cache, const unit_t *u)
{
    size_t n = 0;
    char *old = sim_read_file(u->sig, &n), *p, *q, *now, **files = NULL;
    int nfiles = 0, same;
    if (!old) return 0;
    if (sim_path_kind(u->obj) != 1) { free(old); return 0; }
    p = strchr(old, '\n');
    for (p = p ? p + 1 : NULL; p && *p; p = q + 1) {
        q = strchr(p, '\n');
        if (!q) break;
        push(&files, &nfiles, sim_xstrndup(p, (size_t)(q - p)));
    }
    now = nfiles ? make_sig(z, cache, files, nfiles) : NULL;
    same = now && strlen(now) == n && !memcmp(now, old, n);
    free(now);
    free_list(files, nfiles);
    free(old);
    return same;
}

/* コンパイルのあと、.d から .sig を作る。0 なら成功 */
static int unit_write_sig(nat *z, path_cache *cache, const unit_t *u)
{
    size_t n = 0;
    char *d = sim_read_file(u->dep, &n), *sig, **files = NULL;
    int nfiles = 0, rc = -1;
    if (!d) return -1;
    parse_dep(d, &files, &nfiles);
    free(d);
    sig = nfiles ? make_sig(z, cache, files, nfiles) : NULL;
    if (sig) rc = sim_write_file(u->sig, sig, strlen(sig));
    free(sig);
    free_list(files, nfiles);
    return rc;
}

/* コンパイルの引数のうち、どのソースにも共通の部分。flags に積む（文字列の持ち主は flags） */
static void common_flags(nat *z, char ***flags, int *n)
{
    static const char *const fixed[] = {
        "-std=gnu89",               /* 古い書き方（宣言なしの関数の呼び出しなど）を受け付ける */
        "-gdwarf-4", "-w", "-ffreestanding", "-fno-strict-aliasing", "-fwrapv",
        "-mno-stack-arg-probe",     /* C ランタイムがないので、スタックの確保の確認（__chkstk）を呼ばせない */
        NULL
    };
    int i;
    for (i = 0; fixed[i]; i++) push(flags, n, sim_xstrdup(fixed[i]));
    push(flags, n, sim_xstrdup(z->opt == 2 ? "-O2" : "-O0"));
    /* sim_defines_load と同じ順（mtpj の定義・定義解除 → cdef → --cdef → cundef）を保つ。 */
    for (i = 0; i < z->nmacros; i++) push(flags, n, sim_xstrdup(z->macros[i]));
    for (i = 0; i < z->nincs; i++) {
        push(flags, n, sim_xstrdup("-I"));
        push(flags, n, sim_xstrdup(z->incs[i]));
    }
}

/* コンパイラの版の表示（鍵に入れる）。取れなければ空文字列 */
static char *cc_version(nat *z, const char *base)
{
    const char *argv[3];
    char *tmp = sim_path_join(base, "ccver.tmp"), *text;
    size_t n = 0;
    argv[0] = z->cc;
    argv[1] = "--version";
    argv[2] = NULL;
    sim_remove_file(tmp);
    sim_cc_run_wait(argv, tmp);
    text = sim_read_file(tmp, &n);
    sim_remove_file(tmp);
    free(tmp);
    return text ? text : sim_xstrdup("");
}

static int build(nat *z, char *err, size_t errlen)
{
    char **flags = NULL, *base, *pre, *rt, *ver, *log, *p, *cur, *linksig_path, *want;
    int nflags = 0, i, k, nunits, njobs = 0, rc = -1, ok = 1, gen = 0;
    uint64_t key = SIM_CC_FNV_INIT, t0 = sim_now_ns();
    unit_t *units;
    sim_cc_job *jobs;
    sim_buf ls;
    const char *s, *stem;
    char keytext[32];
    path_cache cache = {{0}};

    /* フォルダ: 指定がなければ設定ファイルの隣の .simnative */
    if ((s = sim_cfg_get(&z->cfg, "native_dir")) && *s) base = sim_cfg_path(&z->cfg, s);
    else base = sim_path_join(z->cfg.dir ? z->cfg.dir : ".", ".simnative");
    if (sim_mkdir_p(base)) {
        snprintf(err, errlen, "native: フォルダ %s を作れません", base);
        free(base);
        return -1;
    }
    pre = sim_path_join(z->rt_dir, "sim_native_pre.h");
    rt = sim_path_join(z->rt_dir, "sim_native_rt.c");
    common_flags(z, &flags, &nflags);

    /* 鍵: コンパイラの版、共通の引数、差し込むヘッダー、DLL に入れる部品 */
    ver = cc_version(z, base);
    key = hash_str(key, ver);
    free(ver);
    for (i = 0; i < nflags; i++) key = hash_str(key, flags[i]);
    {
        uint64_t h;
        h = hash_file(pre, &ok); key = sim_cc_fnv(key, &h, sizeof h);
        h = hash_file(rt, &ok);  key = sim_cc_fnv(key, &h, sizeof h);
    }
    if (!ok) {
        snprintf(err, errlen, "native: %s と sim_native_rt.c がありません（sim の実行ファイルの隣の native フォルダごと置いてください）", pre);
        goto out_early;
    }
    snprintf(keytext, sizeof keytext, "%016llx", (unsigned long long)key);
    stem = z->mtpj_path ? sim_basename(z->mtpj_path) : "native";
    {
        sim_buf nm;
        char *prefix;
        sim_buf_init(&nm);
        sim_buf_adds(&nm, stem);
        sim_buf_addc(&nm, '-');
        prefix = sim_xstrdup(sim_buf_cstr(&nm));
        sim_buf_adds(&nm, keytext);
        z->dir = sim_path_join(base, sim_buf_cstr(&nm));
        sim_cc_sweep_old(base, prefix, sim_buf_cstr(&nm));
        sim_buf_free(&nm);
        free(prefix);
    }
    if (sim_mkdir_p(z->dir)) {
        snprintf(err, errlen, "native: フォルダ %s を作れません", z->dir);
        goto out_early;
    }
    log = sim_path_join(z->dir, "cc.log");
    sim_remove_file(log);

    /* ソースごとの単位。最後の 1 つは DLL に入れる部品（ヘッダーを差し込まない） */
    nunits = z->nsrcs + 1;
    units = sim_xcalloc((size_t)nunits, sizeof *units);
    jobs = sim_xcalloc((size_t)nunits, sizeof *jobs);
    for (i = 0; i < nunits; i++) {
        unit_t *u = &units[i];
        const char *src = i < z->nsrcs ? z->srcs[i] : rt, *bn = sim_basename(src);
        sim_buf nm;
        size_t stemlen = strrchr(bn, '.') ? (size_t)(strrchr(bn, '.') - bn) : strlen(bn);
        u->src = sim_xstrdup(src);
        u->pre = i < z->nsrcs;
        sim_buf_init(&nm);
        sim_buf_add(&nm, bn, stemlen);
        sim_buf_printf(&nm, "-%08x", (unsigned)(hash_str(SIM_CC_FNV_INIT, src) & 0xffffffffu));
        u->base = sim_xstrdup(sim_buf_cstr(&nm));
        sim_buf_free(&nm);
        sim_buf_init(&nm); sim_buf_printf(&nm, "%s.obj", u->base); u->obj = sim_path_join(z->dir, sim_buf_cstr(&nm)); sim_buf_free(&nm);
        sim_buf_init(&nm); sim_buf_printf(&nm, "%s.d", u->base);   u->dep = sim_path_join(z->dir, sim_buf_cstr(&nm)); sim_buf_free(&nm);
        sim_buf_init(&nm); sim_buf_printf(&nm, "%s.sig", u->base); u->sig = sim_path_join(z->dir, sim_buf_cstr(&nm)); sim_buf_free(&nm);
        u->compile = !unit_fresh(z, &cache, u);
        if (!u->compile) { z->nreused++; continue; }
        sim_remove_file(u->obj);
        sim_remove_file(u->sig);
        sim_remove_file(u->dep);
        u->argv = sim_xcalloc((size_t)nflags + 16, sizeof *u->argv);
        k = 0;
        u->argv[k++] = z->cc;
        if (u->pre) {
            int j;
            for (j = 0; j < nflags; j++) u->argv[k++] = flags[j];
            u->argv[k++] = "-include";
            u->argv[k++] = pre;
        } else {
            u->argv[k++] = "-O2";
            u->argv[k++] = "-w";
            u->argv[k++] = "-ffreestanding";
            u->argv[k++] = "-fno-builtin";      /* memcpy の中身を memcpy の呼び出しに置き換えさせない */
            u->argv[k++] = "-mno-stack-arg-probe";
        }
        u->argv[k++] = "-MD";
        u->argv[k++] = "-MF";
        u->argv[k++] = u->dep;
        u->argv[k++] = "-c";
        u->argv[k++] = u->src;
        u->argv[k++] = "-o";
        u->argv[k++] = u->obj;
        u->argv[k] = NULL;
        jobs[njobs].argv = u->argv;
        jobs[njobs].name = sim_basename(u->src);
        njobs++;
    }
    z->ncompiled = njobs;

    if (njobs) {
        int par = 0;
        uint64_t v;
        if ((s = sim_cfg_get(&z->cfg, "native_jobs"))) {
            if (sim_parse_uint(s, 64, &v) || v == 0) {
                snprintf(err, errlen, "sim.cfg: native_jobs = %s を読めません（1〜64）", s);
                goto out;
            }
            par = (int)v;
        }
        if (par == 0) par = sim_cpu_count();
        if (par > 64) par = 64;
        say(z, "native: %d 個のソースのうち %d 個をコンパイルします（%d 並列。フォルダ %s）\n", nunits, njobs, par, z->dir);
        if (sim_cc_run_jobs(jobs, (size_t)njobs, par, log, "native", say_line, z)) {
            sim_buf names;
            sim_buf_init(&names);
            for (i = 0, k = 0; i < njobs; i++) {
                if (jobs[i].exit_code == 0) continue;
                if (k < 5) sim_buf_printf(&names, "%s%s", k ? ", " : "", jobs[i].name);
                k++;
            }
            if (k > 5) sim_buf_printf(&names, " ほか %d 個", k - 5);
            snprintf(err, errlen, "native: コンパイルに失敗しました（%d 個: %s）。コンパイラの出力は %s にあります",
                     k, sim_buf_cstr(&names), log);
            sim_buf_free(&names);
            goto out;
        }
        sim_cc_write_times(jobs, (size_t)njobs, z->dir, "native", say_line, z);
        for (i = 0; i < nunits; i++) {
            if (units[i].compile && unit_write_sig(z, &cache, &units[i])) {
                snprintf(err, errlen, "native: %s を書けません", units[i].sig);
                goto out;
            }
        }
    }

    /* リンク: オブジェクトの組、その中身の値が前回と同じで、DLL も残っていれば作り直さない */
    sim_buf_init(&ls);
    for (i = 0; i < nunits; i++) {
        size_t n = 0;
        char *sg = sim_read_file(units[i].sig, &n);
        sim_buf_printf(&ls, "%s %.16s\n", units[i].base, sg ? sg : "");
        free(sg);
    }
    want = sim_xstrdup(sim_buf_cstr(&ls));
    sim_buf_free(&ls);
    linksig_path = sim_path_join(z->dir, "link.sig");
    cur = sim_path_join(z->dir, "current.txt");
    {
        size_t n = 0, m = 0;
        char *have = sim_read_file(linksig_path, &n), *name = sim_read_file(cur, &m);
        while (name && m && (name[m - 1] == '\n' || name[m - 1] == '\r' || name[m - 1] == ' ')) name[--m] = 0;
        if (name && m && !strchr(name, '/') && !strchr(name, '\\')) {
            sscanf(name, "native-%d", &gen);
            if (njobs == 0 && have && !strcmp(have, want)) {
                p = sim_path_join(z->dir, name);
                if (sim_path_kind(p) == 1) z->dll = p; else free(p);
            }
        }
        free(have);
        free(name);
    }
    if (!z->dll) {
        const char **argv = sim_xcalloc((size_t)nunits + 16, sizeof *argv);
        char name[64], *old = NULL;
        k = 0;
        snprintf(name, sizeof name, "native-%d.dll", gen + 1);
        z->dll = sim_path_join(z->dir, name);
        argv[k++] = z->cc;
        argv[k++] = "-shared";
        argv[k++] = "-nostdlib";
        argv[k++] = "-fuse-ld=lld";       /* LLVM の配布物に入っている lld-link。Windows SDK は要らない */
        argv[k++] = "-Wl,/noentry";
        argv[k++] = "-Wl,/debug:dwarf";
        argv[k++] = "-Wl,/opt:noref";
        for (i = 0; i < nunits; i++) argv[k++] = units[i].obj;
        argv[k++] = "-o";
        argv[k++] = z->dll;
        argv[k] = NULL;
        i = sim_cc_run_wait(argv, log);
        free(argv);
        if (i != 0) {
            snprintf(err, errlen, "native: DLL を作れませんでした。コンパイラの出力は %s にあります"
                                  "（定義のない関数や変数を使っている、など）", log);
            goto out_link;
        }
        if (sim_write_file(linksig_path, want, strlen(want)) || sim_write_file(cur, name, strlen(name))) {
            snprintf(err, errlen, "native: %s に書けません", z->dir);
            goto out_link;
        }
        if (gen > 0) {       /* 前の DLL を消す（ほかの sim が読み込んでいる間は消せない。そのときは残る） */
            char oldname[64];
            snprintf(oldname, sizeof oldname, "native-%d.dll", gen);
            old = sim_path_join(z->dir, oldname);
            sim_remove_file(old);
            free(old);
        }
        z->linked = 1;
    }
    z->build_sec = (double)(sim_now_ns() - t0) / 1e9;
    if (z->ncompiled || z->linked) say(z, "native: DLL ができました（%.1f 秒）\n", z->build_sec);
    rc = 0;
out_link:
    free(want);
    free(linksig_path);
    free(cur);
out:
    for (i = 0; i < nunits; i++) {
        free(units[i].src); free(units[i].base); free(units[i].obj); free(units[i].dep); free(units[i].sig);
        free(units[i].argv);
    }
    free(units);
    free(jobs);
    free(log);
out_early:
    path_cache_free(&cache);
    free_list(flags, nflags);
    free(pre);
    free(rt);
    free(base);
    return rc;
}

/* ---- メモリ ---- */

static range_t *find_range(nat *z, uint64_t addr, size_t n)
{
    int i;
    for (i = 0; i < z->nranges; i++)
        if (addr >= z->ranges[i].lo && addr + n <= z->ranges[i].hi && addr + n >= addr) return &z->ranges[i];
    return NULL;
}

static int add_range(nat *z, uint64_t lo, uint64_t hi, int writable, int keep_init)
{
    range_t *r;
    if (z->nranges == MAX_RANGES || hi <= lo) return -1;
    r = &z->ranges[z->nranges++];
    r->lo = lo;
    r->hi = hi;
    r->writable = writable;
    r->init = NULL;
    if (writable && keep_init) {
        r->init = sim_xmalloc((size_t)(hi - lo));
        memcpy(r->init, (void *)(uintptr_t)lo, (size_t)(hi - lo));
    }
    return 0;
}

static int b_read(sim_backend *b, uint64_t addr, void *buf, size_t n, char *err, size_t errlen)
{
    nat *z = b->impl;
    if (!find_range(z, addr, n)) {
        snprintf(err, errlen, "native: 番地 0x%llX から %u バイトは読めません（読めるのは 0x%X〜0x%X、DLL の変数、$ で確保した領域）",
                 (unsigned long long)addr, (unsigned)n, WIN_LO, WIN_HI - 1);
        return -1;
    }
    memcpy(buf, (void *)(uintptr_t)addr, n);
    return 0;
}

static int b_write(sim_backend *b, uint64_t addr, const void *buf, size_t n, char *err, size_t errlen)
{
    nat *z = b->impl;
    range_t *r = find_range(z, addr, n);
    if (!r || !r->writable) {
        snprintf(err, errlen, "native: 番地 0x%llX から %u バイトには書けません（書けるのは 0x%X〜0x%X、DLL の変数、$ で確保した領域）",
                 (unsigned long long)addr, (unsigned)n, WIN_LO, WIN_HI - 1);
        return -1;
    }
    memcpy((void *)(uintptr_t)addr, buf, n);
    return 0;
}

static int b_alloc(sim_backend *b, size_t size, uint64_t *addr, char *err, size_t errlen)
{
    nat *z = b->impl;
    size_t need = (size + 15u) & ~(size_t)15u;
    if (need < size || z->heap_used + need > HEAP_SIZE) {
        snprintf(err, errlen, "native: $ で確保する領域（%u バイト）が足りません", (unsigned)HEAP_SIZE);
        return -1;
    }
    *addr = (uint64_t)(uintptr_t)(z->heap + z->heap_used);
    z->heap_used += need;
    return 0;
}

static int b_reset(sim_backend *b, char *err, size_t errlen)
{
    nat *z = b->impl;
    int i;
    (void)err; (void)errlen;
    for (i = 0; i < z->nranges; i++) {
        range_t *r = &z->ranges[i];
        if (!r->writable) continue;
        if (r->init) memcpy((void *)(uintptr_t)r->lo, r->init, (size_t)(r->hi - r->lo));
        else memset((void *)(uintptr_t)r->lo, 0, (size_t)(r->hi - r->lo));
    }
    z->heap_used = 0;
    return 0;
}

/* ---- 関数の呼び出し ---- */

#ifdef _WIN32
static volatile LONG g_in_call;
static const char *volatile g_call_name;

/* 対象関数の実行中に起きた例外（確保していない番地の読み書きなど）。戻る先がないので、理由を出して終わる */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *x)
{
    const EXCEPTION_RECORD *r = x->ExceptionRecord;
    if (!g_in_call || !(r->ExceptionCode & 0x80000000u)) return EXCEPTION_CONTINUE_SEARCH;
    fflush(stdout);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        fprintf(stderr, "エラー: native: 関数 %s の実行中に、番地 0x%llX を%sうとして止まりました。"
                        "番地を再現しているのは 0x%X〜0x%X だけです（0x10000 より下は Windows が使わせません）\n",
                g_call_name ? g_call_name : "?", (unsigned long long)r->ExceptionInformation[1],
                r->ExceptionInformation[0] == 1 ? "書こ" : r->ExceptionInformation[0] == 8 ? "実行しよ" : "読も",
                WIN_LO, WIN_HI - 1);
    } else {
        fprintf(stderr, "エラー: native: 関数 %s の実行中に例外 0x%08lX が起きました（0 での割り算、スタックの使い切りなど）\n",
                g_call_name ? g_call_name : "?", (unsigned long)r->ExceptionCode);
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

typedef int64_t (*entry_fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

static int b_call(sim_backend *b, const sim_func *f, const int64_t *args, int nargs,
                  uint64_t *ret_raw, char *err, size_t errlen)
{
    nat *z = b->impl;
    int64_t a[MAX_ARGS] = {0};
    entry_fn fn = NULL;
    void *entry = NULL;
    size_t j;
    int i;
    for (j = 0; j < z->st.nfuncs; j++)
        if (&z->st.funcs[j].f == f) entry = (void *)(uintptr_t)f->addr;
    memcpy(&fn, &entry, sizeof fn);
    if (!fn) { snprintf(err, errlen, "native: シンボル表にない関数は呼べません"); return -1; }
    if (nargs < 0 || nargs > MAX_ARGS) { snprintf(err, errlen, "native: 引数は 8 個までです"); return -1; }
    for (i = 0; i < nargs && i < MAX_ARGS; i++) a[i] = args[i];
    /* x64 の呼び出しの決まりでは、整数とポインタの引数は 64 ビットの枠で渡り、関数は自分の型の幅だけを読む。
     * 戻り値も 64 ビットのレジスタの下位に入るので、引数が 8 個までの関数は同じ形で呼べる */
#ifdef _WIN32
    g_call_name = f->name;
    InterlockedExchange(&g_in_call, 1);
#endif
    *ret_raw = 0;
    if (f->ret && f->ret->kind == SIM_T_FLOAT && f->ret->size == 4) {
        float (*ff)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
        float v;
        uint32_t bits;
        memcpy(&ff, &entry, sizeof ff);
        v = ff(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        memcpy(&bits, &v, sizeof bits);
        *ret_raw = bits;
    } else if (f->ret && f->ret->kind == SIM_T_FLOAT && f->ret->size == 8) {
        double (*df)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
        double v;
        memcpy(&df, &entry, sizeof df);
        v = df(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        memcpy(ret_raw, &v, sizeof v);
    } else {
        *ret_raw = (uint64_t)fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
#ifdef _WIN32
    InterlockedExchange(&g_in_call, 0);
#endif
    z->calls++;
    return 0;
}

/* ---- 列挙、情報、後始末 ---- */

static size_t b_var_count(sim_backend *b) { return ((nat *)b->impl)->st.nvars; }
static const sim_var *b_var_at(sim_backend *b, size_t i)
{
    nat *z = b->impl;
    return i < z->st.nvars ? &z->st.vars[i] : NULL;
}
static size_t b_func_count(sim_backend *b) { return ((nat *)b->impl)->st.nfuncs; }
static const sim_func *b_func_at(sim_backend *b, size_t i)
{
    nat *z = b->impl;
    return i < z->st.nfuncs ? &z->st.funcs[i].f : NULL;
}

static void b_print_info(sim_backend *b, FILE *out)
{
    nat *z = b->impl;
    int i, ndefs = 0, nundefs = 0;
    for (i = 0; i < z->nmacros; i++) {
        if (!strncmp(z->macros[i], "-D", 2)) ndefs++;
        else if (!strncmp(z->macros[i], "-U", 2)) nundefs++;
    }
    fprintf(out, "実行方式: PC 用にコンパイルした DLL（exec = native）。RL78 の命令は再現しません\n");
    if (z->mtpj_path) {
        fprintf(out, "  プロジェクト: %s", z->mtpj_path);
        if (z->mode_name) fprintf(out, "（ビルド・モード %s）", z->mode_name);
        fprintf(out, "\n  登録ファイル: %d 個（ビルド対象外 %d 個）\n", z->files_total, z->files_off);
    }
    fprintf(out, "  コンパイルする .c: %d 個（mtpj から %d 個、native_src から %d 個）\n", z->nsrcs, z->from_mtpj, z->from_extra);
    fprintf(out, "  アセンブラのソース: %d 個（コンパイルしません。中の関数は DLL に入りません）\n", z->nasms);
    for (i = 0; i < z->nasms; i++) fprintf(out, "    %s\n", z->asms[i]);
    fprintf(out, "  インクルード・パス: %d 個、定義マクロ: %d 個、定義解除マクロ: %d 個\n", z->nincs, ndefs, nundefs);
    fprintf(out, "  コンパイラ: %s\n", z->cc);
    fprintf(out, "  今回コンパイルしたもの: %d 個、使い回したもの: %d 個、DLL: %s\n",
            z->ncompiled, z->nreused, z->linked ? "作り直した" : "前回のまま");
    fprintf(out, "  DLL: %s\n", z->dll ? z->dll : "");
    fprintf(out, "  番地を再現する範囲: 0x%X〜0x%X\n", WIN_LO, WIN_HI - 1);
    fprintf(out, "  native sources=%d asm=%d files=%d off=%d compiled=%d reused=%d linked=%d funcs=%d vars=%d opt=%d\n",
            z->nsrcs, z->nasms, z->files_total, z->files_off, z->ncompiled, z->nreused, z->linked,
            (int)z->st.nfuncs, (int)z->st.nvars, z->opt);
    fprintf(out, "  変数: %zu 個、関数: %zu 個（デバッグ情報から）\n", z->st.nvars, z->st.nfuncs);
    fprintf(out, "  最適化: native_opt = %d\n", z->opt);
    if (z->w.n) fprintf(out, "警告: %d 件%s\n", z->w.n, z->w.verbose ? "" : "（--verbose で表示）");
}

static void b_close(sim_backend *b)
{
    /* DLL と確保したメモリは、プロセスが終わるまでそのままにする */
    nat_free(b->impl);
}

/* ---- 開く ---- */

#ifdef _WIN32
/* PE32+ の数値は、構造体へのキャストをせずリトルエンディアンで読む。 */
static uint32_t pe_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned pe_u16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static int pe_range(size_t len, size_t off, size_t n) { return off <= len && n <= len - off; }

/* DISCARDABLE なデバッグ情報は、ロードしたメモリではなくファイルから取る。 */
static int read_dwarf(nat *z, char *err, size_t errlen)
{
    size_t len = 0, pe, opt, sections, strings = 0, strings_len = 0, i, n;
    const uint8_t *d;
    unsigned nsects, optlen;
    uint64_t image_base, loaded = (uint64_t)(uintptr_t)z->lib;
    uint32_t image_size, symptr, nsyms;
    rl78_dwarf_secs secs = {0};
    rl78_dwarf_opts opts = {0};
    z->dll_data = sim_read_file(z->dll, &len);
    d = (const uint8_t *)z->dll_data;
    if (!d) goto bad;
    if (!pe_range(len, 0, 64) || d[0] != 'M' || d[1] != 'Z') goto bad;
    pe = pe_u32(d + 60);
    if (!pe_range(len, pe, 24) || memcmp(d + pe, "PE\0\0", 4)) goto bad;
    nsects = pe_u16(d + pe + 6);
    optlen = pe_u16(d + pe + 20);
    opt = pe + 24;
    if (optlen < 64 || !pe_range(len, opt, optlen) || pe_u16(d + opt) != 0x20b) goto bad;
    image_base = pe_u32(d + opt + 24) | (uint64_t)pe_u32(d + opt + 28) << 32;
    image_size = pe_u32(d + opt + 56);
    if (!image_size || loaded > UINT64_MAX - image_size) goto bad;
    sections = opt + optlen;
    if (!pe_range(len, sections, (size_t)nsects * 40)) goto bad;
    symptr = pe_u32(d + pe + 12);
    nsyms = pe_u32(d + pe + 16);
    if (symptr) {
        if (!pe_range(len, symptr, 0) || nsyms > (len - symptr) / 18) goto bad;
        strings = (size_t)symptr + (size_t)nsyms * 18;
        if (!pe_range(len, strings, 4)) goto bad;
        strings_len = pe_u32(d + strings);
        if (strings_len < 4 || !pe_range(len, strings, strings_len)) goto bad;
    }
    for (i = 0; i < nsects; i++) {
        const uint8_t *h = d + sections + i * 40;
        const char *name;
        char short_name[9];
        size_t raw = pe_u32(h + 20), size = pe_u32(h + 16), vsize = pe_u32(h + 8);
        /* SizeOfRawData はファイルの境界まで 0 で埋めた大きさ。0 の埋め草を CU と読まないよう VirtualSize で切る */
        if (vsize && vsize < size) size = vsize;
        memcpy(short_name, h, 8); short_name[8] = 0;
        name = short_name;
        if (h[0] == '/') {
            size_t j, off = 0;
            for (j = 1; j < 8 && h[j]; j++) {
                if (h[j] < '0' || h[j] > '9') goto bad;
                off = off * 10 + (h[j] - '0');
            }
            if (j == 1 || off < 4 || off >= strings_len) goto bad;
            name = (const char *)d + strings + off;
            if (!memchr(name, 0, strings_len - off)) goto bad;
        }
        if (!pe_range(len, raw, size)) goto bad;
        if (!strcmp(name, ".debug_info")) { secs.info = d + raw; secs.info_len = size; }
        if (!strcmp(name, ".debug_abbrev")) { secs.abbrev = d + raw; secs.abbrev_len = size; }
        if (!strcmp(name, ".debug_str")) { secs.str = d + raw; secs.str_len = size; }
    }
    if (!secs.info_len || !secs.abbrev_len || !secs.str_len) {
        snprintf(err, errlen, "native: DLL にデバッグ情報がありません（.debug_info / .debug_abbrev / .debug_str）");
        return -1;
    }
    opts.default_ptr_size = 8;
    if (rl78_dwarf_read_secs(&z->st, &secs, &opts, &z->w) <= 0) {
        snprintf(err, errlen, "native: DLL のデバッグ情報のコンパイル単位を読めません");
        return -1;
    }
    /* リンカが捨てた番地は除き、残ったものを実際のロード先に移す。 */
    for (i = 0, n = 0; i < z->st.nvars; i++) {
        sim_var v = z->st.vars[i];
        if (v.addr < image_base || v.addr - image_base >= image_size) continue;
        v.addr = loaded + (v.addr - image_base);
        z->st.vars[n++] = v;
    }
    z->st.nvars = n;
    for (i = 0, n = 0; i < z->st.nfuncs; i++) {
        rl78_funcinfo f = z->st.funcs[i];
        if (f.f.addr < image_base || f.f.addr - image_base >= image_size) continue;
        f.f.addr = loaded + (f.f.addr - image_base);
        z->st.funcs[n++] = f;
    }
    z->st.nfuncs = n;
    return 0;
bad:
    snprintf(err, errlen, "native: DLL の PE/COFF ヘッダーを読めません（%s）", z->dll);
    return -1;
}

/* 読み込んだ DLL のセクションを、読み書きできる範囲として登録する。書けるセクションは中身を取っておく */
static int add_dll_sections(nat *z, void *lib)
{
    BYTE *base = lib;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        uint64_t lo = (uint64_t)(uintptr_t)(base + s->VirtualAddress);
        DWORD size = s->Misc.VirtualSize;
        int w = (s->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        if (!size || (s->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) || !(s->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        if (add_range(z, lo, lo + size, w, 1)) return -1;
    }
    return 0;
}

static int map_memory(nat *z, char *err, size_t errlen)
{
    void *p = VirtualAlloc((void *)(uintptr_t)WIN_LO, WIN_HI - WIN_LO, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)WIN_LO) {
        MEMORY_BASIC_INFORMATION mi;
        DWORD e = GetLastError();
        memset(&mi, 0, sizeof mi);
        VirtualQuery((void *)(uintptr_t)WIN_LO, &mi, sizeof mi);
        snprintf(err, errlen, "native: 0x%X〜0x%X のメモリを確保できません（エラー %lu。この範囲はすでに使われています: "
                              "先頭 %p、大きさ 0x%llX、状態 0x%lX、種類 0x%lX）。もう一度実行すると確保できることがあります",
                 WIN_LO, WIN_HI - 1, (unsigned long)e, mi.AllocationBase, (unsigned long long)mi.RegionSize,
                 (unsigned long)mi.State, (unsigned long)mi.Type);
        return -1;
    }
    z->heap = VirtualAlloc(NULL, HEAP_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!z->heap) { snprintf(err, errlen, "native: メモリを確保できません"); return -1; }
    add_range(z, WIN_LO, WIN_HI, 1, 0);
    add_range(z, (uint64_t)(uintptr_t)z->heap, (uint64_t)(uintptr_t)z->heap + HEAP_SIZE, 1, 0);
    return 0;
}
#endif

static const char *exec_of(const sim_open_opts *o, sim_cfg *cfg)
{
    const char *s = o->exec && *o->exec ? o->exec : sim_cfg_get(cfg, "exec");
    return s ? s : "";
}

sim_backend *native_backend_open(const sim_open_opts *o, int *handled, char *err, size_t errlen)
{
    nat *z = sim_xcalloc(1, sizeof *z);
    sim_backend *b;
    const char *s;
    char cerr[1024];
    int i, rc;

    *handled = 0;
    sim_cfg_init(&z->cfg);
    {
        char *cp = sim_cfg_locate(o->cfg_path, o->abs_path);
        rc = sim_cfg_load(&z->cfg, cp, cerr, sizeof cerr);
        free(cp);
    }
    /* 設定ファイルの誤りは、今の方式の入口が同じファイルを読んで報告する */
    if (rc == -2 || strcmp(exec_of(o, &z->cfg), "native")) { nat_free(z); return NULL; }
    *handled = 1;
    z->quiet = o->quiet_xlate;

#ifndef _WIN32
    snprintf(err, errlen, "exec = native は Windows でだけ使えます（0xF0000〜0xFFFFF の番地にメモリを確保できる必要があります）");
    nat_free(z);
    return NULL;
#else
    /* 番地の確保は、ほかのメモリの確保より先に行う（遅くなるほど、ほかのものに取られやすい） */
    if (map_memory(z, err, errlen)) goto fail;

    z->w.verbose = o->verbose;
    rl78_symtab_init(&z->st);
    for (i = 0; i < z->cfg.n; i++) {
        if (strcmp(z->cfg.e[i].key, "native_func")) continue;
        z->cfg.e[i].used = 1;
        fprintf(stderr, "sim.cfg:%d: native_func は不要になりました（関数の型はデバッグ情報から取ります）。この行は無視します\n", z->cfg.e[i].line);
    }
    if ((s = sim_cfg_get(&z->cfg, "native_opt"))) {
        if (strcmp(s, "0") && strcmp(s, "2")) {
            snprintf(err, errlen, "sim.cfg: native_opt = %s は 0 / 2 にしてください", s);
            goto fail;
        }
        z->opt = s[0] - '0';
    }
    if (collect_sources(z, o, err, errlen)) goto fail;

    if ((s = sim_cfg_get(&z->cfg, "native_cc")) && *s) ;
    else if ((s = sim_cfg_get(&z->cfg, "xlate_cc")) && *s) ;
    else s = NULL;
    if (s) z->cc = strchr(s, '/') || strchr(s, '\\') ? sim_cfg_path(&z->cfg, s) : sim_xstrdup(s);
    else if (getenv("SIM_XLATE_CC") && *getenv("SIM_XLATE_CC")) z->cc = sim_xstrdup(getenv("SIM_XLATE_CC"));
    else z->cc = sim_xstrdup("clang");
    if (!sim_cc_works(z->cc)) {
        snprintf(err, errlen, "native: clang を起動できません（%s）。sim.cfg の native_cc = に clang のパスを書いてください"
                              "（LLVM の配布物を展開したフォルダの bin\\clang.exe）", z->cc);
        goto fail;
    }
    if (getenv("SIM_NATIVE_RT") && *getenv("SIM_NATIVE_RT")) z->rt_dir = sim_xstrdup(getenv("SIM_NATIVE_RT"));
    else z->rt_dir = sim_path_join(o->exe_dir ? o->exe_dir : ".", "native");

    if (build(z, err, errlen)) goto fail;

    z->lib = sim_dl_open(z->dll, cerr, sizeof cerr);
    if (!z->lib) { snprintf(err, errlen, "native: DLL %s を読み込めません（%s）", z->dll, cerr); goto fail; }
    if (read_dwarf(z, err, errlen)) goto fail;
    if (z->w.n && !z->w.verbose) fprintf(stderr, "sim: 読み込みの警告が %d 件あります（--verbose で表示）\n", z->w.n);
    if (add_dll_sections(z, z->lib)) { snprintf(err, errlen, "native: DLL のセクションが多すぎます"); goto fail; }
    AddVectoredExceptionHandler(1, on_exception);

    z->b.csv_init = sim_cfg_csv_init(&z->cfg, o->csv_init, err, errlen);
    if (z->b.csv_init < 0) goto fail;
    if ((s = sim_cfg_get(&z->cfg, "func"))) {
        size_t j;
        for (j = 0; j < z->st.nfuncs; j++) if (!strcmp(z->st.funcs[j].f.name, s)) z->b.default_func = z->st.funcs[j].f.name;
    }
    b = &z->b;
    b->name = "native";
    b->ptr_size = 8;
    b->big_endian = 0;
    b->var_count = b_var_count;
    b->var_at = b_var_at;
    b->func_count = b_func_count;
    b->func_at = b_func_at;
    b->read = b_read;
    b->write = b_write;
    b->call = b_call;
    b->alloc = b_alloc;
    b->reset = b_reset;
    b->close = b_close;
    b->print_info = b_print_info;
    b->cpu_hz = 0;
    b->impl = z;
    return b;
fail:
    nat_free(z);
    return NULL;
#endif
}
