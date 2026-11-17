/*
 * native_backend.c - PC 用 DLL の方式（exec = native）。段階 3: スタブへの置換
 *
 * やること:
 *   1. CS+ のプロジェクトファイル（mtpj）から、ビルド対象の .c、インクルード・パス、定義・定義解除マクロを取る。
 *      ドライバやスタブなど、製品とは別に管理するソースは native_src で足す。
 *   2. rt/sim_native_pre.h を差し込んでプリプロセスし、型と __asm を書き直してから並列にコンパイルする。
 *      コンパイルは LLVM IR を経由し、整数からポインタへの変換を near の番地として読み替える（native_near.c）。
 *   3. オブジェクトを 1 つの DLL にまとめる（C ライブラリなし。LLVM の配布物の clang と lld-link だけで作れる）。
 *   4. DLL を読み込み、デバッグ情報から変数と関数を取る。
 *   5. 0xF0000〜0xFFFFF に、同じ番地のメモリを確保する（番地を直接書いたレジスタや RAM のアクセス用）。
 *   6. native_layout があれば、ロードモジュールから変数のマイコンでの番地を読み、製品のソースの変数と対応づける
 *      （native_layout.c）。DLL の sim_native_clear_range が、マイコンの番地の範囲にある変数を host_clear でクリアする。
 *
 * キャッシュのフォルダは、設定ファイルの隣の .simnative/<mtpj の名前>-<鍵>/（native_dir で変えられる）。
 * 鍵は「コンパイラの版、コンパイルの引数、差し込むヘッダー、DLL に入れる部品」から作る。フォルダの中:
 *   <名前>-<パスの鍵>.obj   ソース 1 つのオブジェクト
 *   <名前>-<パスの鍵>.d     clang が書いた、そのソースが読んだファイルの一覧
 *   <名前>-<パスの鍵>.sig   ソースと、読んだファイルすべての中身から作った値。同じならコンパイルし直さない
 *   <名前>-<パスの鍵>.i / .pp.c / .rw   プリプロセスの結果 / 書き直した結果 / 書き直しと読み替えの数
 *   <名前>-<パスの鍵>.ll / .near.ll    LLVM IR（最適化の前）/ inttoptr を読み替えた IR（native_near = off なら作らない）
 *   <名前>-<パスの鍵>.decl  .ll から取った宣言（定義のない名前の代わりを作るときに使う）
 *   native-N.dll            できあがり（読み込み中のファイルは上書きできないので、作るたびに番号を進める）
 *   current.txt             いま使う DLL の名前
 *   link.sig                DLL に入れたオブジェクトと、その中身から作った値
 *   cc.log / cc_times.txt   コンパイラの出力 / コンパイルの所要秒数
 *   <名前>-<プロセスの ID>-<通し番号>.tmp   書いてすぐ読んで消す一時ファイル（同時に動くほかの sim と重ならない名前）
 *
 * 関数の例外と時間切れは呼び出し元へ復帰する。コンパイラがない、コンパイルに
 * 失敗した、番地を確保できない、のどれもエラーにする。
 *
 * まだできないこと（docs/DESIGN.md の 4.3 節にも書く）:
 *   - GUI
 *   - #asm 〜 #endasm、__asm { … }、型を書かない宣言
 *   - Windows 以外での実行
 */
#include "native_backend.h"
#include "native_rewrite.h"
#include "native_near.h"
#include "native_coff.h"
#include "native_layout.h"

#include <ctype.h>
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
#else
#include <unistd.h>
#endif

#define WIN_LO 0xF0000u          /* 番地を再現する範囲 */
#define WIN_HI 0x100000u
#define HEAP_SIZE 0x100000u      /* $ptr 用の領域 */
#define MAX_STUBS 256
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
    char *obj, *dep, *sig, *input, *pp, *rw;
    char *ll, *nll, *decl;       /* LLVM IR、読み替えた IR、定義のない名前を調べるための宣言（どれもキャッシュに残す） */
    native_rewrite_stats stats;
    native_near_stats nst;       /* inttoptr の読み替えの数 */
    int pre;                     /* sim_native_pre.h を差し込む */
    int compile;                 /* 今回コンパイルする */
    uint64_t pre_ns;             /* プリプロセスにかかった時間（cc_times.txt にはコンパイルとの合計を出す） */
    const char **argv;
} unit_t;

typedef struct {
    char *name;
    int function;
    int ret;                     /* 関数の戻り値の種類（NATIVE_RET_…） */
    unsigned long size;          /* 変数の大きさ。0 は不明（UNDEF_DATA_SIZE を取る） */
    unsigned long align;         /* 変数の宣言の整列。0 は不明 */
    /* 以下は集めている間だけ使う */
    int defined, referenced;
    int decl_f, decl_d, size_unknown, ret_set;
} undef_t;

/* 止まった場所の 1 段。いちばん内側が先で、そのあとがインライン展開した呼び出し元。分からない名前は NULL */
#define MAX_FRAMES 4
typedef struct {
    char *func, *file;
    unsigned long line;
} frame_t;

#define UNDEF_DATA_SIZE 4096 /* 大きさが分からない変数の代わり。表として読まれても範囲外になりにくいよう大きめに取る */
#define UNDEF_ALIGN_MIN 16   /* 変数の代わりの整列の最小。SSE の命令（movaps など）が前提にする境界 */

/* 設定と CSV を別々に保持し、入口は番地ごとに一度だけ保存する。 */
typedef struct {
    uint64_t orig, target, value;
    uint64_t limit;             /* 書き換えてよい範囲の終わり（次の関数の先頭か、コードのセクションの終わり） */
    const char *oname, *sname;
    const sim_type *ret;        /* 戻り値の型（デバッグ情報になければ NULL） */
    uint32_t size;
    int skip, floating, automatic;
} stub_t;
typedef struct {
    uint64_t addr;
    uint8_t saved[14];
    size_t len;
} patch_t;
typedef struct { const char *name; uint64_t addr; } code_sym_t;

/* マイコンの番地と PC の変数の対応（native_layout）。マイコンの番地の順に並べる */
typedef struct {
    uint64_t lo, hi;             /* マイコンの番地 [lo, hi) */
    uint64_t max_hi;             /* 先頭からこの項目までの hi の最大（二分探索に使う） */
    uint8_t *pc;                 /* PC の実体 */
    uint32_t pc_size;
    int writable;                /* const（DLL の読み取り専用のセクション）なら 0 */
    int warned;                  /* 境界をまたぐ警告を出した */
    const char *name;            /* 表示用（ファイル名/関数名@変数名） */
} layout_t;

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
    stub_t stubs[MAX_STUBS], csv_stubs[MAX_STUBS];
    int nstubs, ncsv;
    patch_t patches[MAX_STUBS * 2];
    int npatches;
    code_sym_t *code_syms;
    size_t ncode_syms;
    rl78_symtab st;
    rl78_warns w;
    char *dll_data;             /* DLL のファイルを閉じるまで保持する */
    int opt;
    int near_on;                 /* native_near: 整数からポインタへの変換を near の番地として読み替える */
    unsigned timeout_ms;
#ifdef _WIN32
    HANDLE watcher, exception_handler;
#endif
    int ncompiled, nreused, linked;
    native_rewrite_stats rewrite;
    native_near_stats near_total;
    int asm_files;
    undef_t *undefs;
    int nundefs, undef_skipped;
    double build_sec;
    void *lib;
    range_t ranges[MAX_RANGES]; int nranges;
    uint8_t *heap; size_t heap_used;
    uint64_t calls;
    /* 止まった場所の表示（llvm-symbolizer） */
    char *symbolizer;            /* 起動する llvm-symbolizer */
    uint64_t image_base;         /* DLL の優先ベース（デバッグ情報の番地はこれが基準） */
    int sym_runs, sym_limit_said, sym_explained;
    uint64_t sym_last_addr;      /* 直前に調べた番地（優先ベースでの値。0 はまだない） */
    int sym_nframes;             /* 直前の結果。-1 は llvm-symbolizer を使えなかった */
    frame_t sym_frames[MAX_FRAMES];
    /* マイコンの番地の範囲のクリア（native_layout、sim_native_clear_range） */
    char *layout_path;           /* native_layout（なければ NULL） */
    rl78_elf lay_elf;            /* ロードモジュール（対応づけを作るまで持つ） */
    rl78_symtab lay_st;
    layout_t *lay; size_t nlay;
    int lay_vars, lay_matched, lay_unmatched, lay_size_diff, lay_dll_only, lay_skipped_src, lay_src_vars;
    const char **lay_lm_only, **lay_dll_names, **lay_src_names;   /* 対応なしと対象外の名前（表示用。それぞれ 50 個まで） */
    int n_lm_only, n_dll_names, n_src_names;
    uint64_t host_slot;          /* DLL の __sim_host_clear の番地（0 は見つからない） */
    volatile uint64_t clear_calls, clear_vars;
    volatile int clear_warned_args, clear_warned_nolayout;
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

#ifdef _WIN32
static void stop_watcher(nat *z);
#endif

static void nat_free(nat *z)
{
    int i;
    if (!z) return;
#ifdef _WIN32
    stop_watcher(z);
#endif
    sim_cfg_free(&z->cfg);
    free(z->cc); free(z->rt_dir); free(z->dir); free(z->dll); free(z->mtpj_path); free(z->mode_name);
    free(z->symbolizer);
    for (i = 0; i < z->sym_nframes; i++) { free(z->sym_frames[i].func); free(z->sym_frames[i].file); }
    free_list(z->srcs, z->nsrcs);
    free_list(z->asms, z->nasms);
    free_list(z->incs, z->nincs);
    free_list(z->macros, z->nmacros);
    rl78_symtab_free(&z->st);
    rl78_warns_free(&z->w);
    free(z->dll_data);
    free(z->code_syms);
    for (i = 0; i < z->nundefs; i++) free(z->undefs[i].name);
    free(z->undefs);
    for (i = 0; i < z->nranges; i++) free(z->ranges[i].init);
    free(z->layout_path);
    rl78_elf_free(&z->lay_elf);
    rl78_symtab_free(&z->lay_st);
    free(z->lay);
    free((void *)z->lay_lm_only);
    free((void *)z->lay_dll_names);
    free((void *)z->lay_src_names);
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

/* 書いてすぐ読む一時ファイルの名前（<stem>-<プロセスの ID>-<このプロセスの中での通し番号><ext>。呼び出し側で free）。
 * 同じキャッシュのフォルダを使う sim が同時に動いても、相手のファイルを読んだり消したりしない */
static char *private_tmp(const char *stem, const char *ext)
{
    static unsigned seq;
    unsigned long pid;
    sim_buf b;
    char *p;
#ifdef _WIN32
    pid = (unsigned long)GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    sim_buf_init(&b);
    sim_buf_printf(&b, "%s-%lu-%u%s", stem, pid, ++seq, ext);
    p = sim_xstrdup(sim_buf_cstr(&b));
    sim_buf_free(&b);
    return p;
}

/* コンパイラの版の表示（鍵に入れる）。取れなければ空文字列 */
static char *cc_version(nat *z, const char *base)
{
    const char *argv[3];
    char *stem = sim_path_join(base, "ccver"), *tmp = private_tmp(stem, ".tmp"), *text;
    size_t n = 0;
    free(stem);
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

/* 自分で作る中間ファイルの名前。DWARF の補正でも同じ規則を使う。 */
static char *unit_path(nat *z, const char *src, const char *ext)
{
    const char *bn = sim_basename(src), *dot = strrchr(bn, '.');
    sim_buf b;
    char *p;
    sim_buf_init(&b);
    sim_buf_add(&b, bn, dot ? (size_t)(dot - bn) : strlen(bn));
    sim_buf_printf(&b, "-%08x%s", (unsigned)(hash_str(SIM_CC_FNV_INIT, src) & 0xffffffffu), ext);
    p = sim_path_join(z->dir, sim_buf_cstr(&b));
    sim_buf_free(&b);
    return p;
}

/* .rw の 1 行: 書き直しの数 4 つと、inttoptr の読み替えの数 3 つ（定数、実行時、書き換えなかったもの） */
static int read_rewrite_stats(unit_t *u)
{
    char *s = sim_read_file(u->rw, NULL), extra;
    int ok = s && sscanf(s, "%d %d %d %d %d %d %d %c", &u->stats.int_replaced,
                        &u->stats.int_dropped, &u->stats.short_added, &u->stats.asm_removed,
                        &u->nst.const_replaced, &u->nst.dyn_inserted, &u->nst.kept, &extra) == 7;
    free(s);
    return ok && u->stats.int_replaced >= 0 && u->stats.int_dropped >= 0 &&
           u->stats.short_added >= 0 && u->stats.asm_removed >= 0 &&
           u->nst.const_replaced >= 0 && u->nst.dyn_inserted >= 0 && u->nst.kept >= 0;
}

/* ---- どの .c にも定義がない名前の代わり ----
 * 名前の集合は、全オブジェクトのシンボル表から求める（参照され、どこにも定義がないもの）。
 * 関数か変数かは、シンボル表では分からない（clang は未定義のシンボルに関数の印を付けない）ので、
 * その名前を参照しているソースの LLVM IR（コンパイルの途中で作る .ll）の宣言（declare / external の global）から決める。
 * 宣言は、ソースをコンパイルするたびに .ll から .decl に書き出しておく（clang をもう一度起動しない）。 */
typedef struct {
    undef_t *items; int n, cap;
    int *slots; size_t nslots;       /* items の添字 + 1。0 は空き */
    int **refs; int *nrefs;          /* ユニットごとに、未定義として参照した items の添字 */
    int unit;
} undef_set;

static int undef_find(undef_set *s, const char *name, int add)
{
    size_t mask, h;
    if (!s->nslots || (add && (size_t)s->n * 2 >= s->nslots)) {
        size_t ns = s->nslots ? s->nslots * 2 : 256, k, j;
        int *sl = sim_xcalloc(ns, sizeof *sl);
        for (k = 0; k < (size_t)s->n; k++) {
            for (j = (size_t)hash_str(SIM_CC_FNV_INIT, s->items[k].name) & (ns - 1); sl[j]; j = (j + 1) & (ns - 1)) {}
            sl[j] = (int)k + 1;
        }
        free(s->slots);
        s->slots = sl;
        s->nslots = ns;
    }
    mask = s->nslots - 1;
    for (h = (size_t)hash_str(SIM_CC_FNV_INIT, name) & mask; s->slots[h]; h = (h + 1) & mask)
        if (!strcmp(s->items[s->slots[h] - 1].name, name)) return s->slots[h] - 1;
    if (!add) return -1;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->items = sim_xrealloc(s->items, (size_t)s->cap * sizeof *s->items);
    }
    memset(&s->items[s->n], 0, sizeof *s->items);
    s->items[s->n].name = sim_xstrdup(name);
    s->slots[h] = s->n + 1;
    return s->n++;
}

static void undef_visit(void *ctx, const char *name, int defined, int function)
{
    undef_set *set = ctx;
    int i, u = set->unit;
    (void)function;                  /* 未定義のシンボルでは当てにならない（native_coff.h） */
    if (!strcmp(name, "__ImageBase")) return;
    i = undef_find(set, name, 1);
    if (defined) { set->items[i].defined = 1; return; }
    set->items[i].referenced = 1;
    set->refs[u] = sim_xrealloc(set->refs[u], ((size_t)set->nrefs[u] + 1) * sizeof **set->refs);
    set->refs[u][set->nrefs[u]++] = i;
}

static int undef_target(const undef_t *u) { return u->referenced && !u->defined; }

/* .decl / undef.txt の 1 行: 「F 戻り値の種類 名前」または「D 大きさ 整列 名前」（大きさ・整列の 0 は不明） */
static const char *const ret_names[] = {"int", "f32", "f64", "other", "sret"};
#define NRET ((int)(sizeof ret_names / sizeof ret_names[0]))

/* line（改行なし）を読む。name は line の中を指す。読めなければ 0（古い形式の「F 名前」「D 大きさ 名前」も読めない扱い） */
static int decl_parse(char *line, char **name, int *function, unsigned long *size, int *ret, unsigned long *align)
{
    *size = 0;
    *align = 0;
    *ret = NATIVE_RET_INT;
    if (line[0] == 'F' && line[1] == ' ') {
        char *kind = line + 2, *sp = strchr(kind, ' ');
        int k;
        if (!sp || !sp[1]) return 0;
        for (k = 0; k < NRET; k++)
            if ((size_t)(sp - kind) == strlen(ret_names[k]) && !memcmp(kind, ret_names[k], (size_t)(sp - kind))) break;
        if (k == NRET) return 0;
        *function = 1;
        *ret = k;
        *name = sp + 1;
        return 1;
    }
    if (line[0] == 'D' && line[1] == ' ') {
        char *sp;
        if (line[2] < '0' || line[2] > '9') return 0;
        *size = strtoul(line + 2, &sp, 10);
        if (*sp != ' ' || sp[1] < '0' || sp[1] > '9') return 0;
        *align = strtoul(sp + 1, &sp, 10);
        if (*sp != ' ' || !sp[1]) return 0;
        *function = 0;
        *name = sp + 1;
        return 1;
    }
    return 0;
}

/* .ll の宣言を .decl の行にする */
static void decl_visit(void *ctx, const char *name, int is_function, unsigned long size, int ret, unsigned long align)
{
    sim_buf *b = ctx;
    if (is_function) sim_buf_printf(b, "F %s %s\n", ret_names[ret >= 0 && ret < NRET ? ret : NATIVE_RET_OTHER], name);
    else sim_buf_printf(b, "D %lu %lu %s\n", size, align, name);
}

/* .decl が全部の行を読めれば 1。読めない行（古い形式など）があれば 0 で、作り直す */
static int decl_ok(const char *path)
{
    char *text = sim_read_file(path, NULL), *line, *end;
    int ok = text != NULL;
    for (line = text; ok && *line; line = end + 1) {
        char *name;
        int function, ret;
        unsigned long size, align;
        if (!(end = strchr(line, '\n'))) { ok = 0; break; }
        *end = 0;
        ok = decl_parse(line, &name, &function, &size, &ret, &align);
    }
    free(text);
    return ok;
}

/* .decl を読み、対象の名前の宣言を items に集める */
static void decl_merge(undef_set *set, const char *path)
{
    char *text = sim_read_file(path, NULL), *line, *end;
    if (!text) return;
    for (line = text; *line; line = end + 1) {
        unsigned long size, align;
        char *name;
        int i, function, ret;
        undef_t *u;
        if (!(end = strchr(line, '\n'))) break;
        *end = 0;
        if (!decl_parse(line, &name, &function, &size, &ret, &align)) continue;
        if ((i = undef_find(set, name, 0)) < 0 || !undef_target(&set->items[i])) continue;
        u = &set->items[i];
        if (function) {
            u->decl_f = 1;
            /* 戻り値の種類が食い違えば、最初に見つかったものを使って知らせる（製品のソースの宣言の問題） */
            if (!u->ret_set) { u->ret = ret; u->ret_set = 1; }
            else if (u->ret != ret && u->ret_set == 1) {
                fprintf(stderr, "native: 警告: 関数 %s の戻り値の型がソースによって違います。"
                                "最初に見つかったもの（%s）で代わりを作ります\n", u->name, ret_names[u->ret]);
                u->ret_set = 2;
            }
        } else {
            u->decl_d = 1;
            if (!size) u->size_unknown = 1;
            else if (size > u->size) u->size = size;
            if (align > u->align) u->align = align;     /* 食い違えば大きいほう */
        }
    }
    free(text);
}

static int undef_cmp(const void *a, const void *b)
{
    return strcmp((*(const undef_t *const *)a)->name, (*(const undef_t *const *)b)->name);
}

static int c_identifier(const char *s)
{
    size_t i;
    if (!*s) return 0;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if (i && c >= '0' && c <= '9') continue;
        return 0;
    }
    return 1;
}

/* .ll（text）の宣言を .decl に書く。0 なら成功 */
static int write_decls(const unit_t *u, const char *text, size_t len)
{
    sim_buf b;
    int rc;
    sim_buf_init(&b);
    native_ll_decls(text, len, decl_visit, &b);
    rc = sim_write_file(u->decl, sim_buf_cstr(&b), b.len);
    sim_buf_free(&b);
    return rc;
}

/* 代わりのオブジェクトは集合を求める対象に含めない。製品と部品の件数にも加えない。 */
static int build_undef(nat *z, unit_t *units, int nunits, const char *log, char **object, sim_buf *signature, char *err, size_t errlen)
{
    undef_set set;
    sim_buf source, list;
    char *src = sim_path_join(z->dir, "sim_native_undef.c");
    char *obj = sim_path_join(z->dir, "sim_native_undef.obj");
    char *txt = sim_path_join(z->dir, "undef.txt"), *old = NULL;
    unsigned char *need = sim_xcalloc((size_t)nunits, 1);
    undef_t **targets = NULL;
    int i, j, rc = -1, usable = 1, generated = 0, ntargets = 0;
    memset(&set, 0, sizeof set);
    set.refs = sim_xcalloc((size_t)nunits, sizeof *set.refs);
    set.nrefs = sim_xcalloc((size_t)nunits, sizeof *set.nrefs);
    sim_buf_init(&source);
    sim_buf_init(&list);
    sim_buf_adds(&source, "/* sim が自動生成。内容は sim info の一覧と同じです。 */\n");
    for (i = 0; i < nunits; i++) {
        size_t len = 0;
        char detail[200], *data = sim_read_file(units[i].obj, &len);
        int bad;
        set.unit = i;
        bad = !data || native_coff_symbols((const unsigned char *)data, len, undef_visit, &set, detail, sizeof detail);
        free(data);
        if (bad) {
            fprintf(stderr, "native: 警告: %s のシンボル表を読めないため、未定義の自動補完を行わずリンクします\n", units[i].obj);
            usable = 0;
            break;
        }
    }
    if (usable) {
        for (i = 0; i < set.n; i++) if (undef_target(&set.items[i])) ntargets++;
        if (ntargets) {
            /* 宣言を調べるのは、対象の名前を参照している製品と native_src のソースだけ */
            for (i = 0; i < nunits; i++)
                for (j = 0; j < set.nrefs[i] && !need[i]; j++)
                    if (undef_target(&set.items[set.refs[i][j]])) need[i] = 1;
            /* .decl はコンパイルのたびに作る（使い回すソースは、.decl が読めることも条件にしている） */
            for (i = 0; i < nunits && usable; i++) {
                if (!need[i] || !units[i].pre || decl_ok(units[i].decl)) continue;
                fprintf(stderr, "native: 警告: 定義のない名前の宣言（%s）を読めないため、"
                                "未定義の自動補完を行わずリンクします\n", units[i].decl);
                usable = 0;
            }
            if (usable) {
                for (i = 0; i < nunits; i++)
                    if (need[i] && units[i].pre) decl_merge(&set, units[i].decl);
            }
        }
    }
    if (usable && ntargets) {
        targets = sim_xcalloc((size_t)ntargets, sizeof *targets);
        for (i = 0, j = 0; i < set.n; i++) if (undef_target(&set.items[i])) targets[j++] = &set.items[i];
        qsort(targets, (size_t)ntargets, sizeof *targets, undef_cmp);
    }
    for (i = 0; i < ntargets && targets; i++) {
        undef_t *u = targets[i];
        unsigned long size;
        if (u->decl_f) {
            u->function = 1;
            if (u->decl_d)
                fprintf(stderr, "native: 警告: %s は、関数として宣言したソースと変数として宣言したソースがあります。"
                                "関数として代わりを作ります\n", u->name);
        } else if (u->decl_d) {
            /* 大きさが食い違えば大きいほう。大きさ不明（4096 バイト）より大きいものがなければ不明とする */
            if (u->size_unknown && u->size <= UNDEF_DATA_SIZE) u->size = 0;
        } else {
            u->function = 1;
            fprintf(stderr, "native: 警告: %s の宣言がどのソースにも見つかりません。関数として代わりを作ります\n", u->name);
        }
        size = u->size ? u->size : UNDEF_DATA_SIZE;
        if (!u->function || u->ret < 0 || u->ret >= NRET) u->ret = NATIVE_RET_INT;
        if (u->function) sim_buf_printf(signature, "F %s %s\n", ret_names[u->ret], u->name);
        else sim_buf_printf(signature, "D %lu %lu %s\n", u->size, u->align, u->name);
        if (!c_identifier(u->name)) { z->undef_skipped++; continue; }
        if (u->function) {
            /* Windows x64 では float / double は XMM0、それ以外の整数とポインタは RAX で返る */
            static const char *const ctype[] = {"long long", "float", "double", "long long", "long long"};
            sim_buf_printf(&list, "F %s %s\n", ret_names[u->ret], u->name);
            sim_buf_printf(&source, "%s %s(void) { return 0; }\n", ctype[u->ret], u->name);
            if (u->ret == NATIVE_RET_OTHER)
                fprintf(stderr, "native: 警告: 関数 %s の戻り値の型は代わりを正しく作れません（0 が返らないことがあります）\n", u->name);
            else if (u->ret == NATIVE_RET_SRET)
                fprintf(stderr, "native: 警告: 構造体を返す関数 %s の代わりは、戻り値を書きません\n", u->name);
        } else {
            /* 整列は宣言のもの（clang は 16 バイト以上の配列を 16 にそろえ、それを前提に movaps などを出す）。
             * 最小は 16。不明・使えない値（native_coff が 0 にする）も 16 */
            unsigned long align = u->align > UNDEF_ALIGN_MIN ? u->align : UNDEF_ALIGN_MIN;
            sim_buf_printf(&list, "D %lu %lu %s\n", u->size, u->align, u->name);
            sim_buf_printf(&source, "__attribute__((aligned(%lu))) char %s[%lu];\n", align, u->name, size);
        }
        generated++;
    }
    if (!usable) sim_buf_adds(signature, "未定義の自動補完なし\n");
    if (generated) {
        size_t len = 0;
        old = sim_read_file(src, &len);
        if (!old || len != source.len || memcmp(old, sim_buf_cstr(&source), len) || sim_path_kind(obj) != 1) {
            const char *argv[] = {z->cc, "-O2", "-w", "-ffreestanding", "-fno-builtin",
                                  "-mno-stack-arg-probe", "-c", src, "-o", obj, NULL};
            sim_remove_file(obj);
            if (sim_write_file(src, sim_buf_cstr(&source), source.len)) goto write_error;
            if (sim_cc_run_wait(argv, log)) {
                snprintf(err, errlen, "native: 未定義の代わりをコンパイルできません。出力は %s にあります", log);
                goto out;
            }
        }
        *object = sim_xstrdup(obj);
    } else {
        sim_remove_file(src);
        sim_remove_file(obj);
    }
    /* 一覧を保存して読み戻す（sim info に出す） */
    {
        size_t len = 0;
        char *saved = sim_read_file(txt, &len), *line, *end;
        int same = saved && len == list.len && !memcmp(saved, sim_buf_cstr(&list), len);
        free(saved);
        if (!same && sim_write_file(txt, sim_buf_cstr(&list), list.len)) goto write_error;
        if (!(saved = sim_read_file(txt, NULL))) goto write_error;
        for (line = saved; *line; line = end + 1) {
            undef_t *u;
            unsigned long size, align;
            char *name;
            int function, ret;
            if (!(end = strchr(line, '\n'))) break;
            *end = 0;
            if (!decl_parse(line, &name, &function, &size, &ret, &align)) continue;
            z->undefs = sim_xrealloc(z->undefs, ((size_t)z->nundefs + 1) * sizeof *z->undefs);
            u = &z->undefs[z->nundefs++];
            memset(u, 0, sizeof *u);
            u->name = sim_xstrdup(name);
            u->function = function;
            u->ret = ret;
            u->size = size;
            u->align = align;
        }
        free(saved);
    }
    rc = 0;
    goto out;
write_error:
    snprintf(err, errlen, "native: 未定義の一覧または代わりを %s に保存できません", z->dir);
out:
    for (i = 0; i < set.n; i++) free(set.items[i].name);
    for (i = 0; i < nunits; i++) free(set.refs[i]);
    free(set.items); free(set.slots); free(set.refs); free(set.nrefs);
    free(targets); free(need);
    free(src); free(obj); free(txt); free(old);
    sim_buf_free(&source); sim_buf_free(&list);
    return rc;
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
    char *undef_obj = NULL;

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
    /* 2: .decl に関数の戻り値の種類、3: 変数の整列を入れた（古い .decl を使い回さない）、
     * 4: LLVM IR を経由する 4 段のコンパイルにし、.rw に inttoptr の読み替えの数を足した */
    key = hash_str(key, "rewrite-4");
    key = hash_str(key, z->near_on ? "near-on" : "near-off");
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
        if (u->pre) {
            u->input = unit_path(z, src, ".i");
            u->pp = unit_path(z, src, ".pp.c");
            u->rw = unit_path(z, src, ".rw");
            u->decl = unit_path(z, src, ".decl");
            u->ll = unit_path(z, src, ".ll");
            u->nll = unit_path(z, src, ".near.ll");
        }
        u->compile = !unit_fresh(z, &cache, u) || (u->pre && (!read_rewrite_stats(u) || !decl_ok(u->decl)));
        if (!u->compile) { z->nreused++; continue; }
        sim_remove_file(u->obj);
        sim_remove_file(u->sig);
        sim_remove_file(u->dep);
        if (u->pre) {
            sim_remove_file(u->decl);
            sim_remove_file(u->ll);
            sim_remove_file(u->nll);
        }
        u->argv =sim_xcalloc((size_t)nflags + 16, sizeof *u->argv);
        k = 0;
        u->argv[k++] = z->cc;
        if (u->pre) {
            int j;
            u->argv[k++] = "-std=gnu89";
            u->argv[k++] = "-w";
            u->argv[k++] = "-ffreestanding";
            /* 最適化の指定は __OPTIMIZE__ などの定義を変えるので、プリプロセスにも渡す */
            u->argv[k++] = z->opt == 2 ? "-O2" : "-O0";
            for (j = 0; j < z->nmacros; j++) u->argv[k++] = z->macros[j];
            for (j = 0; j < z->nincs; j++) {
                u->argv[k++] = "-I";
                u->argv[k++] = z->incs[j];
            }
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
        u->argv[k++] = u->pre ? "-E" : "-c";
        u->argv[k++] = u->src;
        u->argv[k++] = "-o";
        u->argv[k++] = u->pre ? u->input : u->obj;
        u->argv[k] = NULL;
        jobs[njobs].argv = u->argv;
        jobs[njobs].name = sim_basename(u->src);
        njobs++;
    }
    z->ncompiled = njobs;

    if (njobs) {
        int par = 0, stage, j;
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
        /* 段階 0: プリプロセス（-E）。段階 1: 書き直した .pp.c を LLVM IR（.ll）にする。
         * 段階 2: .ll から宣言（.decl）を取り、inttoptr を読み替えて（.near.ll）オブジェクトにする。DLL に入れる部品もここ。
         * native_opt = 2 の IR は、最適化の水準を -O2 にしたまま LLVM の最適化だけを止めて出す（-O0 の IR に付く
         * optnone が付かず、読み替えの前に番地の定数がたたまれない）。最適化は段階 2 でかける。 */
        for (stage = 0; stage < 3; stage++) {
            njobs = 0;
            for (i = 0; i < nunits; i++) {
                unit_t *u = &units[i];
                if (!u->compile || (stage < 2 && !u->pre)) continue;
                if (stage == 1) {
                    sim_buf out;
                    size_t len = 0;
                    char *text = sim_read_file(u->input, &len);
                    if (!text) {
                        snprintf(err, errlen, "native: %s を読めません", u->input);
                        goto out;
                    }
                    sim_buf_init(&out);
                    native_rewrite(text, len, &out, &u->stats);
                    free(text);
                    if (sim_write_file(u->pp, sim_buf_cstr(&out), out.len)) {
                        snprintf(err, errlen, "native: %s を書けません", u->pp);
                        sim_buf_free(&out);
                        goto out;
                    }
                    sim_buf_free(&out);
                    k = 0;
                    u->argv[k++] = z->cc;
                    /* 共通の引数から、マクロとインクルード・パスを除く（common_flags はこの 2 つを末尾に積む）。 */
                    for (j = 0; j < nflags - z->nmacros - 2 * z->nincs; j++) u->argv[k++] = flags[j];
                    if (z->opt == 2) {
                        u->argv[k++] = "-Xclang";
                        u->argv[k++] = "-disable-llvm-passes";
                    }
                    u->argv[k++] = "-x";
                    u->argv[k++] = "cpp-output";
                    u->argv[k++] = "-S";
                    u->argv[k++] = "-emit-llvm";
                    u->argv[k++] = u->pp;
                    u->argv[k++] = "-o";
                    u->argv[k++] = u->ll;
                    u->argv[k] = NULL;
                } else if (stage == 2 && u->pre) {
                    sim_buf out, stats;
                    size_t len = 0;
                    char *text = sim_read_file(u->ll, &len);
                    int bad;
                    if (!text) {
                        snprintf(err, errlen, "native: %s を読めません", u->ll);
                        goto out;
                    }
                    sim_buf_init(&out);
                    sim_buf_init(&stats);
                    bad = write_decls(u, text, len);
                    if (z->near_on) {
                        native_near(text, len, &out, &u->nst);
                        bad = bad || sim_write_file(u->nll, sim_buf_cstr(&out), out.len);
                    } else memset(&u->nst, 0, sizeof u->nst);
                    free(text);
                    sim_buf_printf(&stats, "%d %d %d %d %d %d %d\n", u->stats.int_replaced,
                                   u->stats.int_dropped, u->stats.short_added, u->stats.asm_removed,
                                   u->nst.const_replaced, u->nst.dyn_inserted, u->nst.kept);
                    bad = bad || sim_write_file(u->rw, sim_buf_cstr(&stats), stats.len);
                    sim_buf_free(&out);
                    sim_buf_free(&stats);
                    if (bad) {
                        snprintf(err, errlen, "native: %s、%s または %s を書けません", u->decl, u->nll, u->rw);
                        goto out;
                    }
                    k = 0;
                    u->argv[k++] = z->cc;
                    for (j = 0; j < nflags - z->nmacros - 2 * z->nincs; j++) u->argv[k++] = flags[j];
                    u->argv[k++] = "-c";
                    u->argv[k++] = z->near_on ? u->nll : u->ll;
                    u->argv[k++] = "-o";
                    u->argv[k++] = u->obj;
                    u->argv[k] = NULL;
                }
                memset(&jobs[njobs], 0, sizeof jobs[njobs]);
                jobs[njobs].argv = u->argv;
                jobs[njobs++].name = sim_basename(u->src);
            }
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
            /* jobs の並びは、この段階で対象になる units の並びと同じ */
            if (stage < 2) {
                for (i = 0, j = 0; i < nunits; i++)
                    if (units[i].compile && units[i].pre) units[i].pre_ns += jobs[j++].elapsed;
                continue;
            }
            for (i = 0, j = 0; i < nunits; i++)
                if (units[i].compile) jobs[j++].elapsed += units[i].pre_ns;
            sim_cc_write_times(jobs, (size_t)njobs, z->dir, "native", say_line, z);
        }
        njobs = z->ncompiled;
        for (i = 0; i < nunits; i++) {
            if (units[i].compile && unit_write_sig(z, &cache, &units[i])) {
                snprintf(err, errlen, "native: %s を書けません", units[i].sig);
                goto out;
            }
        }
    }

    for (i = 0; i < z->nsrcs; i++) {
        native_rewrite_stats *st = &units[i].stats;
        z->rewrite.int_replaced += st->int_replaced;
        z->rewrite.int_dropped += st->int_dropped;
        z->rewrite.short_added += st->short_added;
        z->rewrite.asm_removed += st->asm_removed;
        if (st->asm_removed) z->asm_files++;
        z->near_total.const_replaced += units[i].nst.const_replaced;
        z->near_total.dyn_inserted += units[i].nst.dyn_inserted;
        z->near_total.kept += units[i].nst.kept;
    }

    /* リンク: オブジェクトの組、その中身の値が前回と同じで、DLL も残っていれば作り直さない */
    sim_buf_init(&ls);
    if (build_undef(z, units, nunits, log, &undef_obj, &ls, err, errlen)) {
        sim_buf_free(&ls);
        goto out;
    }
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
        argv[k++] = "-Wl,/opt:noicf";     /* 同じ中身の関数を 1 つにまとめない（入口の書き換えが別の関数に及ばないように） */
        for (i = 0; i < nunits; i++) argv[k++] = units[i].obj;
        if (undef_obj) argv[k++] = undef_obj;
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
    free(undef_obj);
    for (i = 0; i < nunits; i++) {
        free(units[i].src); free(units[i].base); free(units[i].obj); free(units[i].dep); free(units[i].sig);
        free(units[i].input); free(units[i].pp); free(units[i].rw); free(units[i].decl); free(units[i].ll); free(units[i].nll);
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
static volatile LONG g_in_call, g_stop;
static const char *volatile g_call_name;
static void *g_jmp[5];
static volatile uintptr_t g_stack;
static volatile uint64_t g_deadline;
static HANDLE g_thread;
static DWORD g_thread_id;
static SRWLOCK g_thread_lock = SRWLOCK_INIT;
static volatile DWORD g_exception;
static volatile ULONG_PTR g_access, g_address, g_instruction;
static volatile int g_timeout;
static volatile ULONG_PTR g_stopped_at;           /* 時間切れで止めたときの Rip（書き換える前の値） */
static volatile uintptr_t g_code_lo, g_code_hi;   /* 製品の DLL の範囲（時間切れで書き換えてよい位置） */
static nat *volatile g_clear_nat;                 /* sim_native_clear_range が使う（host_clear） */

static __attribute__((noreturn, noinline)) void native_recover(void)
{
    __builtin_longjmp(g_jmp, 1);
}

/* 例外の配送を終えてから、元のスレッドで戻り先へ復帰する。 */
static void recovery_context(CONTEXT *c)
{
    c->Rip = (DWORD64)(uintptr_t)native_recover;
    c->Rsp = (DWORD64)(((g_stack - 1024) & ~(uintptr_t)15) - 8);
    c->EFlags &= ~0x400u;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *x)
{
    const EXCEPTION_RECORD *r = x->ExceptionRecord;
    if (!g_in_call || !(r->ExceptionCode & 0x80000000u) ||
        GetCurrentThreadId() != g_thread_id) return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr, "エラー: native: 関数 %s の実行中に、スタックを使い切りました（再帰が止まらないなど）\n",
                g_call_name ? g_call_name : "?");
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 2);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedCompareExchange(&g_in_call, 0, 1) != 1) return EXCEPTION_CONTINUE_SEARCH;
    g_exception = r->ExceptionCode;
    g_access = r->NumberParameters >= 2 ? r->ExceptionInformation[0] : 0;
    g_address = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    g_instruction = (ULONG_PTR)r->ExceptionAddress;
    recovery_context(x->ContextRecord);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watch_calls(void *unused)
{
    (void)unused;
    while (!g_stop) {
        Sleep(20);
        if (!g_in_call || !g_deadline || sim_now_ns() < g_deadline) continue;
        AcquireSRWLockExclusive(&g_thread_lock);
        if (g_thread && SuspendThread(g_thread) != (DWORD)-1) {
            CONTEXT c;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            /* SuspendThread は止まるのを待たないので、先に GetThreadContext で止まるのを待つ。
             * 止まった位置が製品の DLL の中のときだけ書き換える（b_call の前後、例外の配送中、
             * ハンドラの中では書き換えず、次の確認で判定し直す）。停止前に次の呼び出しへ進んだ場合は、その期限で判定する */
            if (GetThreadContext(g_thread, &c) && c.Rip >= g_code_lo && c.Rip < g_code_hi &&
                g_deadline && sim_now_ns() >= g_deadline &&
                InterlockedCompareExchange(&g_in_call, 0, 1) == 1) {
                g_timeout = 1;
                g_stopped_at = (ULONG_PTR)c.Rip;
                recovery_context(&c);
                if (!SetThreadContext(g_thread, &c)) { g_timeout = 0; g_in_call = 1; }
            }
            ResumeThread(g_thread);
        }
        ReleaseSRWLockExclusive(&g_thread_lock);
    }
    return 0;
}

static void stop_watcher(nat *z)
{
    if (!z->watcher && !z->exception_handler) return;
    if (z->watcher) {
        g_stop = 1;
        /* 20 ミリ秒の待機が終わり、ハンドルを使い終えてから閉じる。 */
        WaitForSingleObject(z->watcher, INFINITE);
        CloseHandle(z->watcher);
    }
    if (z->exception_handler) RemoveVectoredExceptionHandler(z->exception_handler);
    if (g_thread) CloseHandle(g_thread);
    g_thread = NULL;
    g_thread_id = 0;
}

/* ---- 止まった場所（llvm-symbolizer） ----
 * 例外ハンドラと監視のスレッドは番地を控えるだけにし、b_call に戻ってから llvm-symbolizer を起動して、
 * 番地をソースのファイル名と行番号にする。デバッグ情報の行は、プリプロセスの行の情報（# 行 "ファイル"）を
 * 通して、キャッシュの .pp.c ではなく元のソースとヘッダーを指す */

#define SYM_MAX_RUNS 20   /* 1 回の実行で llvm-symbolizer を起動する数の上限（NG の行が多い CSV で遅くならないように） */

/* パスの . と .. をたどって除き、区切りを \ にそろえる（呼び出し側で free） */
static char *tidy_path(const char *p)
{
    size_t n = strlen(p), o = 0, depth = 0, root;
    size_t *mark = sim_xcalloc(n + 1, sizeof *mark);
    char *out = sim_xmalloc(n + 2);
    const char *s = p;
    if (s[0] && s[1] == ':') { out[o++] = s[0]; out[o++] = ':'; s += 2; }
    while (*s == '/' || *s == '\\') { out[o++] = '\\'; s++; }
    root = o;
    while (*s) {
        const char *e = s;
        size_t k;
        while (*e && *e != '/' && *e != '\\') e++;
        k = (size_t)(e - s);
        if (k == 2 && s[0] == '.' && s[1] == '.' && depth > 0) o = mark[--depth];
        else if (k && !(k == 1 && s[0] == '.')) {
            /* 先頭より上へ出る .. は残す（たどれる名前がない） */
            if (!(k == 2 && s[0] == '.' && s[1] == '.')) mark[depth++] = o;
            if (o > root) out[o++] = '\\';
            memcpy(out + o, s, k);
            o += k;
        }
        s = *e ? e + 1 : e;
    }
    out[o] = 0;
    free(mark);
    return out;
}

static int all_digits(const char *s, size_t n)
{
    size_t i;
    if (!n) return 0;
    for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* llvm-symbolizer の場所の行（パス:行:桁）を読む。パスに ':'（ドライブ名）が入るので末尾側から読む。
 * 形が違えば -1。?? や行番号 0 は「分からない」として *file = NULL */
static int parse_loc(const char *s, char **file, unsigned long *line)
{
    const char *c2 = strrchr(s, ':'), *c1;
    *file = NULL;
    *line = 0;
    if (!c2 || c2 == s || !all_digits(c2 + 1, strlen(c2 + 1))) return -1;
    for (c1 = c2 - 1; c1 > s && *c1 != ':'; c1--) ;
    if (*c1 == ':' && all_digits(c1 + 1, (size_t)(c2 - c1 - 1))) {
        *line = strtoul(c1 + 1, NULL, 10);   /* パス:行:桁 */
    } else {
        c1 = c2;                              /* パス:行（桁なし） */
        *line = strtoul(c2 + 1, NULL, 10);
    }
    if (*line == 0 || (c1 - s == 2 && !strncmp(s, "??", 2))) { *line = 0; return 0; }
    *file = sim_xstrndup(s, (size_t)(c1 - s));
    return 0;
}

static void clear_frames(nat *z)
{
    int i;
    for (i = 0; i < z->sym_nframes; i++) { free(z->sym_frames[i].func); free(z->sym_frames[i].file); }
    memset(z->sym_frames, 0, sizeof z->sym_frames);
    z->sym_nframes = 0;
}

/* addr（DLL の優先ベースでの番地）を llvm-symbolizer で調べ、z->sym_frames に入れる。
 * 起動できない・結果を読めなければ z->sym_nframes = -1 */
static void symbolize(nat *z, uint64_t addr)
{
    char a[32], *tmp, *text, *p;
    const char *argv[8];
    sim_buf obj;
    sim_proc *proc;
    size_t len = 0;
    int rc, k = 0, n = 0;
    clear_frames(z);
    snprintf(a, sizeof a, "0x%llX", (unsigned long long)addr);
    sim_buf_init(&obj);
    sim_buf_printf(&obj, "--obj=%s", z->dll);
    {   /* 同じ設定の sim が同時に動いても、相手の出力に追記したり、読む前に消したりしない */
        char *stem = sim_path_join(z->dir, "symbolize");
        tmp = private_tmp(stem, ".tmp");
        free(stem);
    }
    argv[k++] = z->symbolizer;
    argv[k++] = sim_buf_cstr(&obj);
    argv[k++] = "--output-style=LLVM";   /* 1 段ごとに「関数名」「パス:行:桁」の 2 行。番地の区切りは空行 */
    argv[k++] = "--functions=linkage";
    argv[k++] = "--inlines";             /* インライン展開した呼び出し元も、内側から順に出す */
    argv[k++] = a;
    argv[k] = NULL;
    sim_remove_file(tmp);
    proc = sim_proc_spawn(argv, tmp);
    rc = proc ? sim_proc_wait(proc) : -1;
    text = rc == 0 ? sim_read_file(tmp, &len) : NULL;
    sim_remove_file(tmp);
    free(tmp);
    sim_buf_free(&obj);
    if (!text) { z->sym_nframes = -1; return; }
    /* 行に分ける。関数名の行のあとに場所の行が続くものを 1 段とする（エラーの文などは読み飛ばす） */
    {
        char *lines[64];
        int nl = 0, i, ok = 0;
        for (p = text; *p && nl < 64; ) {
            char *e = p + strcspn(p, "\n");
            int more = *e != 0;
            *e = 0;
            if (e > p && e[-1] == '\r') e[-1] = 0;
            lines[nl++] = p;
            p = more ? e + 1 : e;
        }
        for (i = 0; i + 1 < nl; i++) {
            char *file;
            unsigned long line;
            if (!*lines[i]) { if (ok) break; continue; }
            if (parse_loc(lines[i + 1], &file, &line)) continue;
            ok = 1;
            if (n < MAX_FRAMES) {
                z->sym_frames[n].func = strcmp(lines[i], "??") ? sim_xstrdup(lines[i]) : NULL;
                z->sym_frames[n].file = file;
                z->sym_frames[n].line = line;
                n++;
            } else free(file);
            i++;
        }
        free(text);
        z->sym_nframes = ok ? n : -1;
    }
}

/* 例外・時間切れで止まった番地 at の場所を、理由の文 err の末尾に足す。DLL の外なら何もしない */
static void add_location(nat *z, uintptr_t at, int timeout, const char *called, char *err, size_t errlen)
{
    const char *label = timeout ? "止めたときに実行していた場所" : "止まった場所";
    size_t used = strlen(err);
    uint64_t addr;
    sim_buf b;
    int i, shown;
    /* 上限に達したあとは、直前と同じ番地でも足さない（どの行に場所が出るかを分かりやすくする） */
    if (!at || at < g_code_lo || at >= g_code_hi || used + 1 >= errlen || z->sym_limit_said) return;
    addr = z->image_base + (uint64_t)(at - g_code_lo);
    if (!z->sym_last_addr || addr != z->sym_last_addr) {
        if (z->sym_runs >= SYM_MAX_RUNS) {
            if (!z->sym_limit_said && !z->quiet)
                fprintf(stderr, "native: 止まった場所の表示は %d 件までです\n", SYM_MAX_RUNS);
            z->sym_limit_said = 1;
            return;
        }
        z->sym_runs++;
        symbolize(z, addr);
        z->sym_last_addr = addr;
    }
    sim_buf_init(&b);
    if (z->sym_nframes < 0) {
        sim_buf_printf(&b, " %s: DLL の中の位置 0x%llX%s", label, (unsigned long long)addr,
                       z->sym_explained ? "" : "（行番号を出すには llvm-symbolizer が要ります。clang と同じフォルダにあります。"
                                               "native_symbolizer = で指定できます）");
        z->sym_explained = 1;
    } else if (!z->sym_nframes || !z->sym_frames[0].file) {
        sim_buf_printf(&b, " %s: DLL の中の位置 0x%llX（行番号が分かりません）", label, (unsigned long long)addr);
    } else {
        const frame_t *f = &z->sym_frames[0];
        char *full = tidy_path(f->file);
        sim_buf_printf(&b, " %s: %s の %lu 行目（", label, sim_basename(full), f->line);
        if (f->func && (!called || strcmp(f->func, called))) sim_buf_printf(&b, "関数 %s、", f->func);
        sim_buf_printf(&b, "%s:%lu）", full, f->line);   /* パス:行 は、エディタにそのまま渡せる形 */
        free(full);
        for (i = 1, shown = 0; i < z->sym_nframes; i++) {
            f = &z->sym_frames[i];
            if (!f->file) continue;
            full = tidy_path(f->file);
            sim_buf_printf(&b, "%s%s の %lu 行目", shown++ ? " ← " : "。インライン展開した呼び出し元: ",
                           sim_basename(full), f->line);
            if (f->func && (!called || strcmp(f->func, called))) sim_buf_printf(&b, "（関数 %s）", f->func);
            free(full);
        }
    }
    /* 入りきらないときは足さない（文字の途中で切らない） */
    if (used + b.len < errlen) memcpy(err + used, sim_buf_cstr(&b), b.len + 1);
    sim_buf_free(&b);
}

#endif

typedef int64_t (*entry_fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

static int b_call(sim_backend *b, const sim_func *f, const int64_t *args, int nargs,
                  uint64_t *ret_raw, char *err, size_t errlen)
{
    nat *volatile z = b->impl;
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
    if (g_thread_id != GetCurrentThreadId()) {
        HANDLE thread;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &thread, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            snprintf(err, errlen, "native: 呼び出すスレッドのハンドルを作れません");
            return -1;
        }
        AcquireSRWLockExclusive(&g_thread_lock);
        if (g_thread) CloseHandle(g_thread);
        g_thread = thread;
        g_thread_id = GetCurrentThreadId();
        ReleaseSRWLockExclusive(&g_thread_lock);
    }
    g_call_name = f->name;
    g_stack = (uintptr_t)__builtin_frame_address(0);
    g_timeout = 0;
    g_exception = 0;
    g_instruction = 0;
    g_stopped_at = 0;
    if (__builtin_setjmp(g_jmp)) {
        g_in_call = 0;   /* 復帰させた側が 0 にしているが、念のため */
        g_deadline = 0;
        if (g_timeout)
            snprintf(err, errlen, "native: 関数 %s が %u ミリ秒たっても戻りません（native_timeout_ms）。周辺機能は模擬しないので、レジスタのビットが変わるのを待つループは終わりません。待っているレジスタに CSV で値を入れるか、その関数をスタブにしてください",
                     g_call_name, z->timeout_ms);
        else if (g_exception == EXCEPTION_ACCESS_VIOLATION)
            snprintf(err, errlen, "native: 関数 %s の実行中に、番地 0x%llX を%sうとして止まりました。番地を再現しているのは 0x%X〜0x%X だけです（0x10000 より下は Windows が使わせません）",
                     g_call_name, (unsigned long long)g_address,
                     g_access == 1 ? "書こ" : g_access == 8 ? "実行しよ" : "読も", WIN_LO, WIN_HI - 1);
        else if (g_exception == EXCEPTION_INT_DIVIDE_BY_ZERO)
            snprintf(err, errlen, "native: 関数 %s の実行中に、0 での割り算が起きました（マイコンでは止まらずに決まった値になりますが、PC では例外になります）", g_call_name);
        else
            snprintf(err, errlen, "native: 関数 %s の実行中に例外 0x%08lX が起きました", g_call_name, (unsigned long)g_exception);
        /* ここは通常の文脈なので、外部のプログラムを起動してよい */
        add_location(z, g_timeout ? (uintptr_t)g_stopped_at : (uintptr_t)g_instruction, g_timeout, g_call_name, err, errlen);
        return -1;
    }
    g_deadline = z->timeout_ms ? sim_now_ns() + (uint64_t)z->timeout_ms * 1000000u : 0;
    g_in_call = 1;
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
    g_in_call = 0;
    g_deadline = 0;
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
    fprintf(out, "  アセンブラのソース: %d 個（コンパイルしません。参照された未定義の名前には代わりを作ります）\n", z->nasms);
    for (i = 0; i < z->nasms; i++) fprintf(out, "    %s\n", z->asms[i]);
    {
        int funcs = 0, data = 0;
        for (i = 0; i < z->nundefs; i++) {
            if (z->undefs[i].function) funcs++; else data++;
        }
        if (!z->nundefs) fprintf(out, "  どの .c にも定義がないもの: なし\n");
        else {
            fprintf(out, "  どの .c にも定義がないもの: 関数 %d 個（呼ばれると何もせず 0 を返します）、変数 %d 個（0 で埋めた領域）\n", funcs, data);
            for (i = 0; i < z->nundefs && i < 50; i++) {
                const undef_t *u = &z->undefs[i];
                if (u->function)
                    fprintf(out, "    関数 %s%s\n", u->name,
                            u->ret == NATIVE_RET_F32 ? "（float を返す）" :
                            u->ret == NATIVE_RET_F64 ? "（double を返す）" :
                            u->ret == NATIVE_RET_OTHER || u->ret == NATIVE_RET_SRET ? "（戻り値は 0 になりません）" : "");
                else if (u->size) fprintf(out, "    変数 %s（%lu バイト）\n", u->name, u->size);
                else fprintf(out, "    変数 %s（大きさ不明のため %d バイト）\n", u->name, UNDEF_DATA_SIZE);
            }
            if (z->nundefs > 50) fprintf(out, "    ほか %d 個（一覧は %s\\undef.txt）\n", z->nundefs - 50, z->dir);
            fprintf(out, "  動きが必要なものは、同じ名前の関数を C で書いたファイルを native_src に置くと、そちらが使われます\n");
        }
        fprintf(out, "  native undef funcs=%d data=%d skipped=%d\n", funcs, data, z->undef_skipped);
    }
    fprintf(out, "  インクルード・パス: %d 個、定義マクロ: %d 個、定義解除マクロ: %d 個\n", z->nincs, ndefs, nundefs);
    fprintf(out, "  コンパイラ: %s\n", z->cc);
    fprintf(out, "  止まった場所を調べる道具: %s（native_symbolizer）\n", z->symbolizer ? z->symbolizer : "");
    fprintf(out, "  今回コンパイルしたもの: %d 個、使い回したもの: %d 個、DLL: %s\n",
            z->ncompiled, z->nreused, z->linked ? "作り直した" : "前回のまま");
    fprintf(out, "  DLL: %s\n", z->dll ? z->dll : "");
    fprintf(out, "  番地を再現する範囲: 0x%X〜0x%X\n", WIN_LO, WIN_HI - 1);
    fprintf(out, "  書き直し: int を 16 ビットに %d か所、__asm を空に %d か所（ソース %d 個）\n",
            z->rewrite.int_replaced + z->rewrite.int_dropped + z->rewrite.short_added,
            z->rewrite.asm_removed, z->asm_files);
    fprintf(out, "  native rewrite int=%d int_dropped=%d short_added=%d asm=%d asm_files=%d\n",
            z->rewrite.int_replaced, z->rewrite.int_dropped, z->rewrite.short_added,
            z->rewrite.asm_removed, z->asm_files);
    if (z->near_on)
        fprintf(out, "  整数からポインタへの変換: 定数 %d か所を 0xF0000 台に読み替え、実行時の読み替え %d か所（native_near）\n",
                z->near_total.const_replaced, z->near_total.dyn_inserted);
    else fprintf(out, "  整数からポインタへの変換: 読み替えなし（native_near = off）\n");
    fprintf(out, "  native near=%d const=%d dyn=%d kept=%d\n", z->near_on,
            z->near_total.const_replaced, z->near_total.dyn_inserted, z->near_total.kept);
    fprintf(out, "  native sources=%d asm=%d files=%d off=%d compiled=%d reused=%d linked=%d funcs=%d vars=%d opt=%d\n",
            z->nsrcs, z->nasms, z->files_total, z->files_off, z->ncompiled, z->nreused, z->linked,
            (int)z->st.nfuncs, (int)z->st.nvars, z->opt);
    fprintf(out, "  変数: %zu 個、関数: %zu 個（デバッグ情報から）\n", z->st.nvars, z->st.nfuncs);
    {
        int k, stubs = 0, automatic = 0, skips = 0;
        for (k = 0; k < z->nstubs; k++) {
            const stub_t *t = &z->stubs[k];
            if (t->skip) { skips++; fprintf(out, "skip = %s:%llu\n", t->oname, (unsigned long long)t->value); }
            else {
                if (t->automatic) automatic++; else stubs++;
                fprintf(out, "スタブ: %s（0x%llX）-> %s（0x%llX）（先頭を書き換え）\n",
                        t->oname, (unsigned long long)t->orig, t->sname, (unsigned long long)t->target);
            }
        }
        if (!z->nstubs) fprintf(out, "スタブ: なし\n");
        fprintf(out, "  native stubs=%d auto=%d skips=%d\n", stubs, automatic, skips);
    }
    fprintf(out, "  native timeout_ms=%u\n", z->timeout_ms);
    if (z->timeout_ms) fprintf(out, "  1 回の呼び出しの上限: %u ミリ秒（native_timeout_ms）\n", z->timeout_ms);
    else fprintf(out, "  1 回の呼び出しの上限: なし（native_timeout_ms）\n");
    fprintf(out, "  最適化: native_opt = %d\n", z->opt);
    if (z->layout_path) {
        fprintf(out, "  変数の番地の対応（native_layout）: ロードモジュールの変数 %d 個のうち %d 個を対応づけ（対応なし %d 個）、"
                     "大きさの違うもの %d 個、native_src の変数 %d 個は対象外\n",
                z->lay_vars, z->lay_matched, z->lay_unmatched, z->lay_size_diff, z->lay_skipped_src);
        fprintf(out, "    ロードモジュール: %s\n", z->layout_path);
        fprintf(out, "    対応づけの対象: 製品のプロジェクト（mtpj）のソースの変数。native_src のソースの変数 %d 個はクリアしません\n",
                z->lay_src_vars);
        fprintf(out, "    DLL（製品のソース）にあってロードモジュールにない変数: %d 個\n", z->lay_dll_only);
        if (z->w.verbose) {
            int k;
            fprintf(out, "    ロードモジュールにあって DLL（製品のソース）にない変数（%d 個%s）:\n", z->lay_unmatched,
                    z->lay_unmatched > z->n_lm_only ? "。50 個まで" : "");
            for (k = 0; k < z->n_lm_only; k++) fprintf(out, "      %s\n", z->lay_lm_only[k]);
            fprintf(out, "    DLL（製品のソース）にあってロードモジュールにない変数（%d 個%s）:\n", z->lay_dll_only,
                    z->lay_dll_only > z->n_dll_names ? "。50 個まで" : "");
            for (k = 0; k < z->n_dll_names; k++) fprintf(out, "      %s\n", z->lay_dll_names[k]);
            fprintf(out, "    ロードモジュールにあって、DLL では native_src のソースの変数なので対象外にしたもの（%d 個%s）:\n",
                    z->lay_skipped_src, z->lay_skipped_src > z->n_src_names ? "。50 個まで" : "");
            for (k = 0; k < z->n_src_names; k++) fprintf(out, "      %s\n", z->lay_src_names[k]);
        }
    } else fprintf(out, "  変数の番地の対応（native_layout）: なし\n");
    fprintf(out, "  native layout vars=%d matched=%d unmatched=%d size_diff=%d dll_only=%d skipped_src=%d\n",
            z->lay_vars, z->lay_matched, z->lay_unmatched, z->lay_size_diff, z->lay_dll_only, z->lay_skipped_src);
    if (z->w.n) fprintf(out, "警告: %d 件%s\n", z->w.n, z->w.verbose ? "" : "（--verbose で表示）");
}

static void b_close(sim_backend *b)
{
    /* DLL と確保したメモリは、プロセスが終わるまでそのままにする */
    nat *z = b->impl;
    if (z->w.verbose && (z->clear_calls || z->layout_path))
        fprintf(stderr, "native: sim_native_clear_range の呼び出し %llu 回、クリアした変数 延べ %llu 個\n",
                (unsigned long long)z->clear_calls, (unsigned long long)z->clear_vars);
#ifdef _WIN32
    g_clear_nat = NULL;
#endif
    nat_free(z);
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

/* addr から書き換えてよい範囲の終わり: addr を含む実行できるセクションの終わりと、addr より後ろで最初の関数の先頭
 * （デバッグ情報と COFF の表の両方）の小さいほう。実行できるセクションの外なら 0 */
static uint64_t code_limit(nat *z, uint64_t addr)
{
    const BYTE *base = z->lib;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    uint64_t limit = 0;
    size_t i;
    int k;
    for (k = 0; k < nt->FileHeader.NumberOfSections; k++, s++) {
        uint64_t lo = (uint64_t)(uintptr_t)(base + s->VirtualAddress), hi = lo + s->Misc.VirtualSize;
        if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && addr >= lo && addr < hi) { limit = hi; break; }
    }
    if (!limit) return 0;
    for (i = 0; i < z->st.nfuncs; i++) {
        uint64_t a = z->st.funcs[i].f.addr;
        if (a > addr && a < limit) limit = a;
    }
    for (i = 0; i < z->ncode_syms; i++) {
        uint64_t a = z->code_syms[i].addr;
        if (a > addr && a < limit) limit = a;
    }
    return limit;
}

/* 名前だけなら外部を優先し、static だけなら最初の一致を使う（RL78 と同じ）。 */
static int resolve_stub(nat *z, const char *name, stub_t *t, int target)
{
    const char *sl = strrchr(name, '/'), *nm = sl ? sl + 1 : name;
    char file[512];
    const rl78_funcinfo *hit = NULL, *nofile = NULL;
    uint64_t addr = 0;
    const char *resolved = NULL;
    size_t i;
    if (sl) {
        size_t n = (size_t)(sl - name);
        if (!n || n >= sizeof file || !*nm) return -1;
        memcpy(file, name, n); file[n] = 0;
    }
    for (i = 0; i < z->st.nfuncs; i++) {
        const rl78_funcinfo *f = &z->st.funcs[i];
        if (strcmp(f->f.name, nm)) continue;
        if (sl) {
            if (f->f.file && *f->f.file) {
                if (sim_path_suffix_match(f->f.file, file)) { hit = f; break; }
            } else if (!nofile) nofile = f;
        } else {
            if (!hit) hit = f;
            if (f->f.is_external) { hit = f; break; }
        }
    }
    if (!hit) hit = nofile;
    if (hit) { addr = hit->f.addr; resolved = hit->f.name; }
    else {
        /* 別のファイルにしかない同名関数を COFF 経由で選ばない。 */
        if (sl) for (i = 0; i < z->st.nfuncs; i++)
            if (!strcmp(z->st.funcs[i].f.name, nm)) return -1;
        for (i = 0; i < z->ncode_syms; i++) if (!strcmp(z->code_syms[i].name, nm)) {
            addr = z->code_syms[i].addr; resolved = z->code_syms[i].name; break;
        }
    }
    if (!addr) return -1;
    if (target) { t->target = addr; t->sname = resolved; }
    else {
        t->orig = addr; t->oname = resolved;
        t->size = hit ? hit->size : 0;
        t->ret = hit ? hit->f.ret : NULL;
        t->floating = hit && hit->f.ret && hit->f.ret->kind == SIM_T_FLOAT;
        t->limit = code_limit(z, addr);
    }
    return 0;
}

/* clang の関数の間の埋め草（int3、nop、0F 1F の複数バイトの nop。66 / 2E の接頭辞つき）の 1 命令の長さ。埋め草でなければ 0 */
static size_t pad_len(const uint8_t *p, size_t avail)
{
    size_t i = 0, len;
    unsigned m, mod, rm;
    if (!avail) return 0;
    if (p[0] == 0xcc) return 1;
    while (i < avail && i < 14 && (p[i] == 0x66 || p[i] == 0x2e)) i++;
    if (i < avail && p[i] == 0x90) return i + 1;
    if (i + 2 >= avail || p[i] != 0x0f || p[i + 1] != 0x1f) return 0;
    m = p[i + 2]; mod = m >> 6; rm = m & 7; len = i + 3;
    if (mod != 3 && rm == 4) {
        if (len >= avail) return 0;
        if (mod == 0 && (p[len] & 7) == 5) len += 4;
        len++;
    }
    if (mod == 1) len += 1;
    else if (mod == 2 || (mod == 0 && rm == 5)) len += 4;
    return len <= avail ? len : 0;
}

static int write_code(uint64_t addr, const uint8_t *code, size_t n, char *err, size_t errlen)
{
    void *p = (void *)(uintptr_t)addr;
    DWORD old, ignored;
    int protected_ok, flushed;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) goto bad;
    memcpy(p, code, n);
    protected_ok = VirtualProtect(p, n, old, &ignored) != 0;
    flushed = FlushInstructionCache(GetCurrentProcess(), p, n) != 0;
    if (protected_ok && flushed) return 0;
bad:
    snprintf(err, errlen, "native: 関数の入口を書き換えられません（エラー %lu）", (unsigned long)GetLastError());
    return -1;
}

static int restore_stubs(nat *z, char *err, size_t errlen)
{
    int i, failed = 0;
    for (i = z->npatches - 1; i >= 0; i--) {
        patch_t *p = &z->patches[i];
        if (write_code(p->addr, p->saved, p->len, err, errlen)) failed = 1;
    }
    if (!failed) z->npatches = 0;
    return failed ? -1 : 0;
}

static int patch_stub(nat *z, const stub_t *t, char *err, size_t errlen)
{
    uint8_t code[14];
    size_t n = 0, i;
    uint64_t value = t->skip ? t->value : t->target;
    patch_t *p;
    if (t->skip && t->floating) { code[n++] = 0x0f; code[n++] = 0x57; code[n++] = 0xc0; }
    code[n++] = 0x48; code[n++] = 0xb8;
    for (i = 0; i < 8; i++) code[n++] = (uint8_t)(value >> (i * 8));
    if (t->skip) code[n++] = 0xc3;
    else { code[n++] = 0xff; code[n++] = 0xe0; }
    /* 次の関数の先頭（またはセクションの終わり）まで命令が収まり、関数の大きさが分かれば、
     * 関数の終わりから書き換える範囲の終わりまでが埋め草であること（.text の中の表などを壊さない） */
    if (!t->limit || t->limit <= t->orig || t->limit - t->orig < n) goto short_func;
    if (t->size && t->size < n) {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)t->orig;
        size_t room = (size_t)(t->limit - t->orig), k;
        for (i = t->size; i < n; i += k)
            if (!(k = pad_len(bytes + i, room - i))) goto short_func;
    }
    p = &z->patches[z->npatches++];
    p->addr = t->orig; p->len = n;
    memcpy(p->saved, (const void *)(uintptr_t)t->orig, n);
    return write_code(t->orig, code, n, err, errlen);
short_func:
    snprintf(err, errlen, "関数 %s は短すぎて置き換えられません（入口の %zu バイトを書き換えると、次の関数か関数の間の埋め草でないところにかかります）",
             t->oname, n);
    return -1;
}

/* CSV が指定する入口には設定の置換を重ねず、元の命令から書き換える。 */
static int apply_stubs(nat *z, char *err, size_t errlen)
{
    int i, j;
    if (restore_stubs(z, err, errlen)) return -1;
    for (i = 0; i < z->nstubs; i++) {
        for (j = 0; j < z->ncsv && z->csv_stubs[j].orig != z->stubs[i].orig; j++) {}
        if (j == z->ncsv && patch_stub(z, &z->stubs[i], err, errlen)) return -1;
    }
    for (i = 0; i < z->ncsv; i++) if (patch_stub(z, &z->csv_stubs[i], err, errlen)) return -1;
    return 0;
}

static int b_set_csv_stubs(sim_backend *b, const char *const *origs, const char *const *stubs, int n,
                           char *err, size_t errlen)
{
    nat *z = b->impl;
    int i, k;
    char recovery[512];
    z->ncsv = 0;
    if (apply_stubs(z, err, errlen)) goto fail;
    for (i = 0; i < n; i++) {
        stub_t t = {0};
        if (resolve_stub(z, origs[i], &t, 0)) {
            snprintf(err, errlen, "%% 行: 置き換え元の関数 '%s' がロードモジュールに見つかりません", origs[i]); goto fail;
        }
        if (resolve_stub(z, stubs[i], &t, 1)) {
            snprintf(err, errlen, "%% 行: スタブ関数 '%s' がロードモジュールに見つかりません", stubs[i]); goto fail;
        }
        if (t.orig == t.target) continue;
        for (k = 0; k < z->ncsv && z->csv_stubs[k].orig != t.orig; k++) {}
        if (k == MAX_STUBS) { snprintf(err, errlen, "%% 行の置換が多すぎます（%d 個まで）", MAX_STUBS); goto fail; }
        z->csv_stubs[k] = t;
        if (k == z->ncsv) z->ncsv++;
    }
    if (!apply_stubs(z, err, errlen)) return 0;
fail:
    z->ncsv = 0;
    if (apply_stubs(z, recovery, sizeof recovery)) snprintf(err, errlen, "%s", recovery);
    return -1;
}

static int add_stub(nat *z, const char *orig, const char *dest, int automatic, char *err, size_t errlen)
{
    stub_t t = {0};
    int k;
    if (resolve_stub(z, orig, &t, 0)) {
        snprintf(err, errlen, "sim.cfg: stub の置き換え元 '%s' が見つかりません", orig); return -1;
    }
    if (resolve_stub(z, dest, &t, 1)) {
        snprintf(err, errlen, "sim.cfg: stub の置き換え先 '%s' が見つかりません", dest); return -1;
    }
    if (t.orig == t.target) return 0;
    for (k = 0; k < z->nstubs && z->stubs[k].orig != t.orig; k++) {}
    if (k == MAX_STUBS) { snprintf(err, errlen, "スタブが多すぎます（%d 個まで）", MAX_STUBS); return -1; }
    t.automatic = automatic;
    z->stubs[k] = t;
    if (k == z->nstubs) z->nstubs++;
    return 0;
}

static int setup_stubs(nat *z, char *err, size_t errlen)
{
    const char *s = sim_cfg_get(&z->cfg, "stub_auto");
    int on = 0, i;
    size_t j;
    if (s) {
        if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "ON")) on = 1;
        else if (strcmp(s, "off") && strcmp(s, "0") && strcmp(s, "no") && strcmp(s, "false") && strcmp(s, "OFF")) {
            snprintf(err, errlen, "sim.cfg: stub_auto = %s は on / off にしてください", s); return -1;
        }
    }
    if (on) for (j = 0; j < z->st.nfuncs; j++) {
        const char *nm = z->st.funcs[j].f.name;
        stub_t t = {0};
        if (!strncmp(nm, "AMSTB_", 6) && nm[6] && !resolve_stub(z, nm + 6, &t, 0))
            if (add_stub(z, nm + 6, nm, 1, err, errlen)) return -1;
    }
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        char *a, *dest, *end;
        int rc;
        if (strcmp(e->key, "stub")) continue;
        e->used = 1;
        a = sim_xstrdup(e->value); dest = strchr(a, ':');
        if (!dest) { free(a); snprintf(err, errlen, "sim.cfg:%d: stub = 元の関数:スタブ関数 の形にしてください", e->line); return -1; }
        *dest++ = 0;
        end = a + strlen(a); while (end > a && end[-1] == ' ') *--end = 0;
        while (*dest == ' ') dest++;
        rc = add_stub(z, a, dest, 0, err, errlen); free(a);
        if (rc) return -1;
    }
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        stub_t t = {0};
        char *a, *value, *end;
        int k;
        if (strcmp(e->key, "skip")) continue;
        e->used = 1;
        a = sim_xstrdup(e->value); value = strchr(a, ':');
        if (value) *value++ = 0;
        end = a + strlen(a); while (end > a && isspace((unsigned char)end[-1])) *--end = 0;
        if (resolve_stub(z, a, &t, 0)) {
            snprintf(err, errlen, "sim.cfg:%d: skip の関数 '%s' が見つかりません", e->line, a); free(a); return -1;
        }
        for (k = 0; k < z->nstubs; k++) if (z->stubs[k].orig == t.orig) {
            snprintf(err, errlen, "sim.cfg:%d: skip %s は stub / skip と重複しています", e->line, a); free(a); return -1;
        }
        /* 戻り値を書いたときは、今の方式と同じく、型が分かり、その大きさに収まる整数（真偽値は 0 / 1、ポインタ）だけ。
         * 浮動小数を返す関数は 0 だけ（XMM0 を 0 にする） */
        if (value) {
            const sim_type *r = t.ret;
            int ok = !sim_parse_uint(value, UINT64_MAX, &t.value) && r;
            free(a);
            if (ok && r->kind == SIM_T_FLOAT) ok = t.value == 0;
            else if (ok) {
                ok = (r->kind == SIM_T_INT || r->kind == SIM_T_UINT || r->kind == SIM_T_BOOL || r->kind == SIM_T_PTR) &&
                     (r->size == 1 || r->size == 2 || r->size == 4 || r->size == 8) &&
                     (r->size == 8 || t.value < (UINT64_C(1) << (8 * r->size))) && (r->kind != SIM_T_BOOL || t.value <= 1);
            }
            if (!ok) {
                snprintf(err, errlen, "sim.cfg:%d: skip = %s を読めません（skip の戻り値は、型が分かる整数・ポインタで、その大きさに収まる値だけ。"
                         "浮動小数を返す関数は 0 だけ）", e->line, e->value);
                return -1;
            }
        } else free(a);
        if (z->nstubs == MAX_STUBS) { snprintf(err, errlen, "スタブが多すぎます（%d 個まで）", MAX_STUBS); return -1; }
        t.skip = 1; z->stubs[z->nstubs++] = t;
    }
    return apply_stubs(z, err, errlen);
}

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
    z->image_base = image_base;
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
    /* DLL の COFF 表には、デバッグ情報のないアセンブラの代わりも残る。 */
    for (i = 0; symptr && i < nsyms; ) {
        const uint8_t *sym = d + symptr + i * 18;
        unsigned section = pe_u16(sym + 12), aux = sym[17];
        const char *name;
        char short_name[9];
        uint32_t value = pe_u32(sym + 8);
        if ((size_t)aux >= nsyms - i) goto bad;
        if (!pe_u32(sym)) {
            size_t off = pe_u32(sym + 4);
            if (off < 4 || off >= strings_len) goto bad;
            name = (const char *)d + strings + off;
            if (!memchr(name, 0, strings_len - off)) goto bad;
        } else { memcpy(short_name, sym, 8); short_name[8] = 0; name = short_name; }
        if (section && section <= nsects) {
            const uint8_t *h = d + sections + (section - 1) * 40;
            uint64_t rva = (uint64_t)pe_u32(h + 12) + value;
            if ((pe_u32(h + 36) & IMAGE_SCN_MEM_EXECUTE) && rva < image_size && sym[16] == 2) {
                code_sym_t *c;
                z->code_syms = sim_xrealloc(z->code_syms, (z->ncode_syms + 1) * sizeof *z->code_syms);
                c = &z->code_syms[z->ncode_syms++];
                c->name = ar_strdup(&z->st.ar, name); c->addr = loaded + rva;
            }
            /* DLL に入れる部品の関数ポインタ（sim_native_clear_range が sim.exe の側を呼ぶのに使う） */
            if (!(pe_u32(h + 36) & IMAGE_SCN_MEM_EXECUTE) && (pe_u32(h + 36) & IMAGE_SCN_MEM_WRITE) &&
                sym[16] == 2 && !strcmp(name, "__sim_host_clear") && rva + 8 <= image_size)
                z->host_slot = loaded + rva;
        }
        i += 1 + aux;
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
    /* コンパイル単位の名前は、コンパイルした <名前>-<ハッシュ>.pp.c になる（clang は先頭の ./ を省くなど、
     * 渡したパスと同じつづりになるとは限らないので、ファイル名の部分だけで照合する。ハッシュは元のソースの
     * パスから作るので、ファイル名だけで元のソースが決まる）。変数・関数の file を元のソースのパスに直す。
     * file はアリーナの文字列を共有する。元の文字列は解放せず、同じアリーナに複製する。 */
    {
        char **names = sim_xcalloc((size_t)z->nsrcs + 1, sizeof *names);
        const char **fixed = sim_xcalloc((size_t)z->nsrcs + 1, sizeof *fixed);
        const char *last_in = NULL, *last_out = NULL;
        size_t t, total = z->st.nvars + z->st.nfuncs;
        int j;
        for (j = 0; j < z->nsrcs; j++) {
            char *pp = unit_path(z, z->srcs[j], ".pp.c");
            names[j] = sim_xstrdup(sim_basename(pp));
            free(pp);
        }
        for (t = 0; t < total; t++) {
            const char **file = t < z->st.nvars ? &z->st.vars[t].file : &z->st.funcs[t - z->st.nvars].f.file;
            const char *bn;
            size_t bl;
            if (!*file) continue;
            if (*file == last_in) { *file = last_out; continue; }
            bn = sim_basename(*file);
            bl = strlen(bn);
            if (bl < 5 || strcmp(bn + bl - 5, ".pp.c")) continue;
            for (j = 0; j < z->nsrcs; j++) {
                if (strcmp(bn, names[j])) continue;
                if (!fixed[j]) fixed[j] = ar_strdup(&z->st.ar, z->srcs[j]);
                last_in = *file;
                last_out = fixed[j];
                *file = fixed[j];
                break;
            }
        }
        for (j = 0; j < z->nsrcs; j++) free(names[j]);
        free(names);
        free((void *)fixed);
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

/* ---- マイコンの番地と PC の変数の対応（native_layout） ----
 * ロードモジュール（CA78K0R の .lmf か CC-RL の .abs）から、変数のマイコンでの番地と大きさを、今の方式と同じ
 * 読み取り（rl78_lmf_load と rl78_sym_load、または rl78_elf_load、rl78_dwarf_read、rl78_symtab_from_elf）で取り、
 * DLL のデバッグ情報の変数と名前で対応づける。static の変数は、ファイル名と関数名も合わせる。
 * 変数の出どころ（mtpj のソースか native_src のソースか）は区別しない */

/* ロードモジュールを読む。読めなければ -1 */
static int load_layout(nat *z, char *err, size_t errlen)
{
    unsigned char magic[4] = {0};
    char lerr[1024];
    FILE *fp = sim_fopen(z->layout_path, "rb");
    size_t n;
    if (!fp) {
        snprintf(err, errlen, "native: native_layout = %s を読めません", z->layout_path);
        return -1;
    }
    n = fread(magic, 1, 4, fp);
    fclose(fp);
    rl78_symtab_init(&z->lay_st);
    lerr[0] = 0;
    if (n >= 2 && magic[0] == 0x48 && magic[1] == 0x11) {
        /* char の符号（char_unsigned）は番地と大きさに関係しないので既定のまま */
        const char *dot = strrchr(sim_basename(z->layout_path), '.');
        size_t stem = dot ? (size_t)(dot - z->layout_path) : strlen(z->layout_path);
        char *sym;
        if (rl78_lmf_load(&z->lay_elf, &z->lay_st, &z->w, z->layout_path, 0, lerr, sizeof lerr)) goto bad;
        sym = sim_xmalloc(stem + 5);
        memcpy(sym, z->layout_path, stem);
        strcpy(sym + stem, ".sym");
        rl78_sym_load(&z->lay_elf, &z->lay_st, &z->w, sym);
        free(sym);
    } else if (n == 4 && !memcmp(magic, "\x7f" "ELF", 4)) {
        rl78_dwarf_opts dopt;
        memset(&dopt, 0, sizeof dopt);
        if (rl78_elf_load(&z->lay_elf, z->layout_path, lerr, sizeof lerr)) goto bad;
        rl78_dwarf_read(&z->lay_st, &z->lay_elf, &dopt, &z->w);
        rl78_symtab_from_elf(&z->lay_st, &z->lay_elf, &z->w);
    } else {
        snprintf(lerr, sizeof lerr, "先頭 %u バイト: %02X %02X %02X %02X。CA78K0R の .lmf か CC-RL の .abs（ELF）が必要",
                 (unsigned)n, magic[0], magic[1], magic[2], magic[3]);
        goto bad;
    }
    if (!z->lay_st.nvars) {
        snprintf(lerr, sizeof lerr, "変数が 1 つもありません");
        goto bad;
    }
    return 0;
bad:
    snprintf(err, errlen, "native: native_layout = %s のロードモジュールを読めません（%s）", z->layout_path, lerr);
    return -1;
}

/* 表示用の名前: グローバルは 名前、ファイル内 static は ファイル名/名前、関数内 static は ファイル名/関数名@名前 */
static const char *layout_display(nat *z, const sim_var *v)
{
    sim_buf b;
    const char *s;
    sim_buf_init(&b);
    if ((v->func || !v->is_external) && v->file && *v->file) sim_buf_printf(&b, "%s/", sim_basename(v->file));
    if (v->func) sim_buf_printf(&b, "%s@", v->func);
    sim_buf_adds(&b, v->name);
    s = ar_strdup(&z->st.ar, sim_buf_cstr(&b));
    sim_buf_free(&b);
    return s;
}

static int layout_cmp(const void *a, const void *b)
{
    const layout_t *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    return 0;
}

static void layout_note(const char ***list, int *n, const char *name)
{
    if (*n >= 50) return;
    *list = sim_xrealloc((void *)*list, sizeof **list * (size_t)(*n + 1));
    (*list)[(*n)++] = name;
}

/* DLL の変数が、製品のプロジェクト（mtpj）に登録されたソースの変数なら 1。z->srcs の先頭 from_mtpj 個が mtpj のソースで、
 * 変数の file は read_dwarf でそのパスの文字列に置き換えてある。native_src のソース（テストドライバ、スタブ、代わりの C）は 0 */
static int product_var(nat *z, const sim_var *v)
{
    int j;
    if (!v->file) return 0;
    for (j = 0; j < z->from_mtpj && j < z->nsrcs; j++)
        if (!strcmp(v->file, z->srcs[j])) return 1;
    return 0;
}

/* ロードモジュールの変数と DLL の変数を対応づけ（native_layout_match）、表を作る。ロードモジュールはそのあと閉じる。
 * 対応づけの対象は、製品のプロジェクトのソースの変数だけ。native_src のソースの変数は、範囲に入っていてもクリアしない */
static void build_layout(nat *z)
{
    size_t nd = z->st.nvars, nl = z->lay_st.nvars, i;
    native_layout_var *lv = sim_xcalloc(nl ? nl : 1, sizeof *lv), *dv = sim_xcalloc(nd ? nd : 1, sizeof *dv);
    long *match = sim_xcalloc(nl ? nl : 1, sizeof *match);
    unsigned char *used = sim_xcalloc(nd ? nd : 1, 1);
    const char *last_file = NULL;
    int last_product = 0;
    for (i = 0; i < nl; i++) {
        const sim_var *v = &z->lay_st.vars[i];
        lv[i].name = v->name; lv[i].file = v->file; lv[i].func = v->func; lv[i].is_external = v->is_external;
    }
    for (i = 0; i < nd; i++) {
        const sim_var *v = &z->st.vars[i];
        dv[i].name = v->name; dv[i].file = v->file; dv[i].func = v->func; dv[i].is_external = v->is_external;
        /* 同じソースの変数は file の文字列を共有するので、直前の結果を使い回す */
        if (v->file != last_file || !i) { last_file = v->file; last_product = product_var(z, v); }
        dv[i].product = last_product;
        if (!last_product) z->lay_src_vars++;
    }
    native_layout_match(lv, nl, dv, nd, match);
    z->lay = sim_xcalloc(nl ? nl : 1, sizeof *z->lay);
    for (i = 0; i < nl; i++) {
        const sim_var *v = &z->lay_st.vars[i], *hit;
        uint32_t msize, psize;
        z->lay_vars++;
        if (match[i] == NATIVE_LAYOUT_SRC) {
            z->lay_skipped_src++;
            layout_note(&z->lay_src_names, &z->n_src_names, layout_display(z, v));
            continue;
        }
        if (match[i] < 0) {
            z->lay_unmatched++;
            layout_note(&z->lay_lm_only, &z->n_lm_only, layout_display(z, v));
            continue;
        }
        hit = &z->st.vars[match[i]];
        used[match[i]] = 1;
        z->lay_matched++;
        msize = v->type ? v->type->size : 0;
        psize = hit->type ? hit->type->size : 0;
        if (msize != psize) z->lay_size_diff++;
        /* 大きさの分からない変数と、番地が 1MB を超える変数は表に入れない */
        if (!msize || !psize || v->addr >= WIN_HI || msize > WIN_HI - v->addr) continue;
        {
            layout_t *t = &z->lay[z->nlay++];
            range_t *r = find_range(z, hit->addr, psize);
            t->lo = v->addr;
            t->hi = v->addr + msize;
            t->pc = (uint8_t *)(uintptr_t)hit->addr;
            t->pc_size = psize;
            t->writable = r && r->writable;
            t->name = layout_display(z, hit);
        }
    }
    for (i = 0; i < nd; i++) {
        if (used[i] || !dv[i].product) continue;
        z->lay_dll_only++;
        layout_note(&z->lay_dll_names, &z->n_dll_names, layout_display(z, &z->st.vars[i]));
    }
    qsort(z->lay, z->nlay, sizeof *z->lay, layout_cmp);
    for (i = 0; i < z->nlay; i++)
        z->lay[i].max_hi = i && z->lay[i - 1].max_hi > z->lay[i].hi ? z->lay[i - 1].max_hi : z->lay[i].hi;
    free(lv);
    free(dv);
    free(match);
    free(used);
    rl78_elf_free(&z->lay_elf);
    rl78_symtab_free(&z->lay_st);
}

/* DLL の sim_native_clear_range から呼ばれる（製品の関数の実行中に、DLL を呼んだスレッドで動く）。
 * マイコンの番地の範囲 [first, end)（inclusive なら [first, end]）にある変数と、確保したメモリを 0 にする。
 * 警告の 1 回を除いて、ロックを取る処理（printf、malloc）を行わない */

static void host_clear(unsigned long long first, unsigned long long end, int inclusive)
{
    nat *z = g_clear_nat;
    size_t lo, hi, i;
    uint64_t a, given = end;
    if (!z) return;
    if (inclusive) end = end < WIN_HI ? end + 1 : WIN_HI + 1;
    if (first >= end || end > WIN_HI) {
        if (!z->clear_warned_args) {
            z->clear_warned_args = 1;
            fprintf(stderr, "native: 警告: %s の範囲（0x%llX〜0x%llX）が正しくないので、クリアしません"
                            "（先頭 < 終わり ≦ 0x100000 にしてください。この警告は 1 度だけ出します）\n",
                    inclusive ? "sim_native_clear_range_incl" : "sim_native_clear_range", first, (unsigned long long)given);
        }
        return;
    }
    z->clear_calls++;
    if (!z->layout_path) {
        if (!z->clear_warned_nolayout) {
            z->clear_warned_nolayout = 1;
            fprintf(stderr, "native: sim_native_clear_range が呼ばれましたが、native_layout がないので、C の変数はクリアできません\n");
        }
    } else {
        /* max_hi が first を超える最初の項目から、lo が end に届くまで */
        lo = 0;
        hi = z->nlay;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (z->lay[mid].max_hi > first) hi = mid; else lo = mid + 1;
        }
        for (i = lo; i < z->nlay && z->lay[i].lo < end; i++) {
            layout_t *t = &z->lay[i];
            if (t->hi <= first || !t->writable) continue;
            if (t->lo >= first && t->hi <= end) {
                memset(t->pc, 0, t->pc_size);
                z->clear_vars++;
            } else if (t->hi - t->lo == t->pc_size) {
                uint64_t s = t->lo > first ? t->lo : first, e = t->hi < end ? t->hi : end;
                memset(t->pc + (s - t->lo), 0, (size_t)(e - s));
                z->clear_vars++;
            } else if (!t->warned) {
                t->warned = 1;
                fprintf(stderr, "native: 範囲の境界をまたぐ変数 %s は、マイコンと PC で大きさが違うのでクリアしません"
                                "（マイコン %u バイト、PC %u バイト）\n", t->name, (unsigned)(t->hi - t->lo), (unsigned)t->pc_size);
            }
        }
    }
    /* 番地を直接書いたアクセスのためのメモリ（0xF0000〜0xFFFFF） */
    a = first > WIN_LO ? first : WIN_LO;
    if (end > a) memset((void *)(uintptr_t)a, 0, (size_t)(end - a));
}

/* DLL の __sim_host_clear に host_clear の番地を入れる（DLL のセクションの中身を控える前に行う） */
static void connect_host(nat *z)
{
    void (*fn)(unsigned long long, unsigned long long, int) = host_clear;
    g_clear_nat = z;
    if (z->host_slot) memcpy((void *)(uintptr_t)z->host_slot, &fn, sizeof fn);
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
    z->timeout_ms = 10000;
    if ((s = sim_cfg_get(&z->cfg, "native_timeout_ms"))) {
        uint64_t ms;
        if (sim_parse_uint(s, 86400000, &ms)) {
            snprintf(err, errlen, "sim.cfg: native_timeout_ms = %s を読めません（0〜86400000）", s);
            goto fail;
        }
        z->timeout_ms = (unsigned)ms;
    }
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
    z->near_on = 1;
    if ((s = sim_cfg_get(&z->cfg, "native_near"))) {
        if (!strcmp(s, "off")) z->near_on = 0;
        else if (strcmp(s, "on")) {
            snprintf(err, errlen, "sim.cfg: native_near = %s は on / off にしてください", s);
            goto fail;
        }
    }
    if (collect_sources(z, o, err, errlen)) goto fail;
    /* マイコンの番地と変数の対応（ロードモジュールを読めなければ、コンパイルの前にエラーにする） */
    if ((s = sim_cfg_get(&z->cfg, "native_layout")) && *s) {
        z->layout_path = sim_cfg_path(&z->cfg, s);
        if (load_layout(z, err, errlen)) goto fail;
    }

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
    /* 止まった場所を調べる llvm-symbolizer。指定がなければ clang と同じフォルダのもの。
     * 起動できるかは、使うときに分かればよいので、ここでは確かめない */
    if ((s = sim_cfg_get(&z->cfg, "native_symbolizer")) && *s)
        z->symbolizer = strchr(s, '/') || strchr(s, '\\') ? sim_cfg_path(&z->cfg, s) : sim_xstrdup(s);
    else if (strchr(z->cc, '/') || strchr(z->cc, '\\')) {
        char *d = dir_of(z->cc);
        z->symbolizer = sim_path_join(d, "llvm-symbolizer.exe");
        free(d);
    } else z->symbolizer = sim_xstrdup("llvm-symbolizer");
    if (getenv("SIM_NATIVE_RT") && *getenv("SIM_NATIVE_RT")) z->rt_dir = sim_xstrdup(getenv("SIM_NATIVE_RT"));
    else z->rt_dir = sim_path_join(o->exe_dir ? o->exe_dir : ".", "native");

    if (build(z, err, errlen)) goto fail;

    z->lib = sim_dl_open(z->dll, cerr, sizeof cerr);
    if (!z->lib) { snprintf(err, errlen, "native: DLL %s を読み込めません（%s）", z->dll, cerr); goto fail; }
    if (read_dwarf(z, err, errlen)) goto fail;
    if (setup_stubs(z, err, errlen)) goto fail;
    if (z->w.n && !z->w.verbose) fprintf(stderr, "sim: 読み込みの警告が %d 件あります（--verbose で表示）\n", z->w.n);
    connect_host(z);
    if (add_dll_sections(z, z->lib)) { snprintf(err, errlen, "native: DLL のセクションが多すぎます"); goto fail; }
    if (z->layout_path) build_layout(z);
    z->exception_handler = AddVectoredExceptionHandler(1, on_exception);
    if (!z->exception_handler) { snprintf(err, errlen, "native: 例外ハンドラを登録できません"); goto fail; }
    {   /* 止まった場所を調べるのにも使うので、時間の上限がなくても控える */
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)z->lib;
        const IMAGE_NT_HEADERS64 *nh = (const IMAGE_NT_HEADERS64 *)((const char *)z->lib + dh->e_lfanew);
        g_code_lo = (uintptr_t)z->lib;
        g_code_hi = g_code_lo + nh->OptionalHeader.SizeOfImage;
    }
    if (z->timeout_ms) {
        g_stop = 0;
        (void)sim_now_ns();
        z->watcher = CreateThread(NULL, 0, watch_calls, NULL, 0, NULL);
        if (!z->watcher) { snprintf(err, errlen, "native: 監視用のスレッドを作れません"); goto fail; }
    }

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
    b->set_csv_stubs = b_set_csv_stubs;
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
