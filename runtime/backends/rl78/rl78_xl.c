/*
 * rl78_xl.c - 「C への変換方式」をバックエンドから使う部分
 *
 * コア（rl78/src/rl78_xlate.c）は RL78 の機械語を C に変換するだけなので、ここで:
 *   - どの関数を変換するかを決める（CALL の飛び先の記録 + 呼び出した対象関数 + 前回までの一覧）
 *   - C のファイルを書き、clang を並列に起動してコンパイルし、DLL にまとめる
 *   - できた DLL をキャッシュのフォルダに置き、次回の起動で読み込む
 *
 * キャッシュのフォルダはロードモジュールの隣の .simxl/<名前>-<鍵>/。鍵は「変換の対象になるメモリの中身、CPU の設定、
 * 変換に使うソース、コンパイラ」から作るので、ロードモジュールや sim を作り直すと別のフォルダになる
 * （古いフォルダは消す）。フォルダの中:
 *   entries.txt   変換した関数の入口（16 進、1 行 1 個）
 *   uNNN.c / .o   関数の入口の番地 >> 10 ごとのまとまり。内容が前回と同じなら .o を使い回す
 *   main.c / .o   まとまりをつなぐ表
 *   xl-N.dll      できあがり（読み込み中のファイルは上書きできないので、作るたびに番号を進める）
 *   current.txt   いま使う DLL の名前
 *   cc.log        コンパイラの出力
 *   cc_times.txt  今回コンパイルしたファイルの所要秒数（長い順）
 */
#include "rl78_xl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_cc.h"
#include "sim_platform.h"
#include "sim_util.h"

#define BUCKET_SHIFT 10
#define NBUCKETS (0x100000u >> BUCKET_SHIFT)

#if defined(_WIN32)
#define DLL_EXT ".dll"
#define OBJ_EXT ".obj"
#elif defined(__APPLE__)
#define DLL_EXT ".dylib"
#define OBJ_EXT ".o"
#else
#define DLL_EXT ".so"
#define OBJ_EXT ".o"
#endif

/* コアが変換に使うソース（sim の実行ファイルの隣の xlate/ に置く） */
static const char *const RT_FILES[] = {
    "rl78.h", "rl78_internal.h", "rl78_exec_defs.h", "rl78_exec_ops.inc", "rl78_xlate_rt.h", NULL
};

struct rl78_xl {
    rl78_cpu *cpu;
    int mode;
    int quiet;
    uint32_t rom_size;
    char *rt_dir;       /* 変換に使うソースのフォルダ */
    char *cc;           /* clang */
    char *dir;          /* キャッシュのフォルダ */
    int jobs;
    uint8_t *known;     /* ビット表: 変換済みの DLL に入っている関数の入口 */
    size_t nknown;
    uint8_t *noted;     /* ビット表: 今回の実行で呼び出した対象関数 */
    uint8_t *rom;       /* 開いたときのコードフラッシュの中身（変換はいつもこれから作る。鍵もこれから計算した） */
    uint8_t *dirty;     /* ビット表: 変換した命令のバイトのうち、いま rom と違う値になっているもの */
    size_t ndirty;
    void *lib;
    int loaded;         /* DLL を読み込んで取り付けた */
    int gen;            /* いまの DLL の番号 */
    char why_off[400];  /* 使えない理由（空なら使える） */
};

/* ---- 小物 ---- */

static int bit(const uint8_t *m, uint32_t a) { return (m[a >> 3] >> (a & 7u)) & 1; }
static void setbit(uint8_t *m, uint32_t a) { m[a >> 3] |= (uint8_t)(1u << (a & 7u)); }

static char *join(const char *dir, const char *name) { return sim_path_join(dir, name); }

static char *fmt_name(const char *dir, const char *fmt, unsigned v, const char *ext)
{
    char name[64];
    snprintf(name, sizeof name, fmt, v);
    strncat(name, ext, sizeof name - strlen(name) - 1);
    return join(dir, name);
}

static void buf_put(void *ctx, const char *s, size_t n) { sim_buf_add((sim_buf *)ctx, s, n); }

/* path の中身が data と同じなら 1 */
static int same_content(const char *path, const char *data, size_t len)
{
    size_t n = 0;
    char *old = sim_read_file(path, &n);
    int same = old && n == len && memcmp(old, data, len) == 0;
    free(old);
    return same;
}

static void say(rl78_xl *x, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void say(rl78_xl *x, const char *fmt, ...)
{
    va_list ap;
    if (x->quiet) return;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---- コンパイラ ---- */

/* 1 つの .c をコンパイルする引数を作る（argv は 24 個以上の配列） */
static void compile_args(rl78_xl *x, const char **argv, const char *src, const char *obj)
{
    int n = 0;
    argv[n++] = x->cc;
    argv[n++] = "-std=c11";
    argv[n++] = "-O2";
    argv[n++] = "-fno-unroll-loops";
    argv[n++] = "-ffreestanding";
    argv[n++] = "-w";
#ifdef _WIN32
    argv[n++] = "-mno-stack-arg-probe";   /* C ランタイムがないので、スタックの確保の確認（__chkstk）を呼ばせない */
#else
    argv[n++] = "-fPIC";
#endif
    argv[n++] = "-DRL78_XL_DLL";
    argv[n++] = "-I";
    argv[n++] = x->rt_dir;
    argv[n++] = "-c";
    argv[n++] = src;
    argv[n++] = "-o";
    argv[n++] = obj;
    argv[n] = NULL;
}

/* ---- キャッシュ ---- */

static void load_entries(rl78_xl *x)
{
    char *path = join(x->dir, "entries.txt"), *text, *p;
    size_t n = 0;
    text = sim_read_file(path, &n);
    free(path);
    memset(x->known, 0, 0x100000 / 8);
    x->nknown = 0;
    for (p = text; p && *p;) {
        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        if (v < x->rom_size && !bit(x->known, (uint32_t)v)) { setbit(x->known, (uint32_t)v); x->nknown++; }
        p = end;
        while (*p == '\n' || *p == '\r' || *p == ' ') p++;
    }
    free(text);
}

/* キャッシュにある DLL を読み込んで取り付ける。なければ何もしない（0）。壊れていたら理由を言って 0 */
static int load_current(rl78_xl *x)
{
    char *cur = join(x->dir, "current.txt"), *name, *path, err[300];
    size_t n = 0;
    const rl78_xl_module *(*get)(const rl78_xl_host *);
    const rl78_xl_module *m;
    name = sim_read_file(cur, &n);
    free(cur);
    if (!name) return 0;
    while (n && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' ')) name[--n] = 0;
    if (!n || strchr(name, '/') || strchr(name, '\\')) { free(name); return 0; }
    sscanf(name, "xl-%d", &x->gen);
    path = join(x->dir, name);
    free(name);
    x->lib = sim_dl_open(path, err, sizeof err);
    if (!x->lib) {
        say(x, "変換: 前回の変換結果を読み込めません（%s: %s）。今回は解釈実行で動かし、終わってから作り直します\n", path, err);
        free(path);
        return 0;
    }
    free(path);
    *(void **)&get = sim_dl_sym(x->lib, "rl78_xl_module_get");
    m = get ? get(rl78_xl_host_get()) : NULL;
    if (!m || rl78_xl_attach(x->cpu, m, err, sizeof err)) {
        say(x, "変換: 前回の変換結果は使えません（%s）。今回は解釈実行で動かし、終わってから作り直します\n", m ? err : "入口が見つかりません");
        return 0;
    }
    load_entries(x);
    x->loaded = 1;
    return 1;
}

/* ---- 開く ---- */

rl78_xl *rl78_xl_open(rl78_cpu *cpu, const rl78_xl_opts *o)
{
    rl78_xl *x = sim_xcalloc(1, sizeof *x);
    uint64_t key = SIM_CC_FNV_INIT;
    char keytext[64], *base = NULL, *absdir, *sub;
    const char *absname;
    uint8_t *rom;
    int i;

    x->cpu = cpu;
    x->mode = o->mode;
    x->quiet = o->quiet;
    x->rom_size = o->rom_size;
    x->jobs = o->jobs > 0 ? o->jobs : sim_cpu_count();
    if (x->jobs > 64) x->jobs = 64;
    x->known = sim_xcalloc(0x100000 / 8, 1);
    x->noted = sim_xcalloc(0x100000 / 8, 1);
    x->rt_dir = o->rt_dir ? sim_xstrdup(o->rt_dir) : join(o->exe_dir ? o->exe_dir : ".", "xlate");
    if (o->cc && *o->cc) x->cc = sim_xstrdup(o->cc);
    else if (getenv("SIM_XLATE_CC") && *getenv("SIM_XLATE_CC")) x->cc = sim_xstrdup(getenv("SIM_XLATE_CC"));
    else x->cc = sim_xstrdup("clang");

    if (!x->rom_size) {
        snprintf(x->why_off, sizeof x->why_off, "コードフラッシュの範囲（rom_size）が分かりません");
        return x;
    }
    /* 鍵: 変換の対象になるメモリ、変換に使うソース、CPU の設定、変換の版 */
    rom = x->rom = sim_xmalloc(x->rom_size);
    x->dirty = sim_xcalloc(0x100000 / 8, 1);
    rl78_peek(cpu, 0, rom, x->rom_size);
    key = sim_cc_fnv(key, rom, x->rom_size);
    key = sim_cc_fnv(key, &x->rom_size, sizeof x->rom_size);
    key = sim_cc_fnv(key, o->core_desc ? o->core_desc : "", o->core_desc ? strlen(o->core_desc) : 0);
    {   /* 変換したコードは、素の RAM の範囲をページの表を引かずに読み書きする。範囲が違えば別のコードになる */
        uint32_t plain[2];
        rl78_xl_plain_range(cpu, &plain[0], &plain[1]);
        key = sim_cc_fnv(key, plain, sizeof plain);
    }
    for (i = 0; RT_FILES[i]; i++) {
        char *p = join(x->rt_dir, RT_FILES[i]), *t;
        size_t n = 0;
        t = sim_read_file(p, &n);
        if (!t) {
            snprintf(x->why_off, sizeof x->why_off, "変換に使うソース %s がありません（sim の実行ファイルの隣の xlate フォルダごと置いてください）", p);
            free(p);
            return x;
        }
        key = sim_cc_fnv(key, t, n);
        free(t);
        free(p);
    }
    key = sim_cc_fnv(key, DLL_EXT, sizeof DLL_EXT);
    snprintf(keytext, sizeof keytext, "%016llx", (unsigned long long)key);

    /* フォルダ: 指定がなければロードモジュールの隣の .simxl */
    absname = o->abs_path ? sim_basename(o->abs_path) : "module";
    if (o->dir && *o->dir) {
        base = sim_xstrdup(o->dir);
    } else {
        size_t dl = o->abs_path ? (size_t)(absname - o->abs_path) : 0;
        absdir = sim_xmalloc(dl + 2);
        if (dl) { memcpy(absdir, o->abs_path, dl); absdir[dl] = 0; } else strcpy(absdir, ".");
        base = join(absdir, ".simxl");
        free(absdir);
    }
    {
        sim_buf nm;
        sim_buf_init(&nm);
        sim_buf_adds(&nm, absname);
        sim_buf_addc(&nm, '-');
        sub = sim_xstrdup(sim_buf_cstr(&nm));
        sim_buf_adds(&nm, keytext);
        x->dir = join(base, sim_buf_cstr(&nm));
        if (sim_path_kind(base) == 2) sim_cc_sweep_old(base, sub, sim_buf_cstr(&nm));
        sim_buf_free(&nm);
        free(sub);
    }
    free(base);

    load_current(x);
    rl78_set_call_profile(cpu, 1);
    return x;
}

int rl78_xl_loaded(const rl78_xl *x) { return x ? x->loaded : 0; }
size_t rl78_xl_known(const rl78_xl *x) { return x ? x->nknown : 0; }
const char *rl78_xl_dir(const rl78_xl *x) { return x && x->dir ? x->dir : ""; }
const char *rl78_xl_why_off(const rl78_xl *x) { return x ? x->why_off : ""; }

void rl78_xl_note_entry(rl78_xl *x, uint32_t addr)
{
    if (x && addr < x->rom_size) setbit(x->noted, addr);
}

/* テスト CSV の入力などで、プログラムの外からコードフラッシュの 1 バイトを書き換えた。変換した命令のバイトが
 * 開いたときと違う値になっている間は、変換したコードを使わない（変換したコードは書き換えを知らないので、
 * 古い命令のまま動いてしまう）。元の値に戻れば、また使う */
void rl78_xl_host_wrote(rl78_xl *x, uint32_t addr, uint8_t value)
{
    int was, now;
    if (!x || !x->rom || addr >= x->rom_size || !rl78_xl_covers(x->cpu, addr)) return;
    was = bit(x->dirty, addr);
    now = value != x->rom[addr];
    if (now && !was) { setbit(x->dirty, addr); x->ndirty++; }
    else if (!now && was) { x->dirty[addr >> 3] &= (uint8_t)~(1u << (addr & 7u)); x->ndirty--; }
    rl78_xl_set_enabled(x->cpu, x->ndirty == 0);
}

/* メモリを開いたときの状態に戻した（リセット） */
void rl78_xl_memory_restored(rl78_xl *x)
{
    if (!x || !x->dirty) return;
    memset(x->dirty, 0, 0x100000 / 8);
    x->ndirty = 0;
    rl78_xl_set_enabled(x->cpu, 1);
}

/* 変換したコードを使ってよい状態なら使う（照合が一時的に止めた後に呼ぶ） */
void rl78_xl_resume(rl78_xl *x)
{
    if (x) rl78_xl_set_enabled(x->cpu, x->ndirty == 0);
}

/* ---- 変換してコンパイルする ---- */

/* 1 つのコンパイル: ソース、オブジェクト、コンパイラの引数 */
typedef struct {
    char *src, *obj;
    const char *argv[24];
} job_t;

static void say_line(void *ctx, const char *line) { say(ctx, "%s", line); }

int rl78_xl_update(rl78_xl *x, const uint32_t *extra, size_t nextra)
{
    const uint8_t *prof = rl78_call_profile(x->cpu);
    uint8_t *entries;
    uint32_t a, *list;
    size_t n = 0, nnew = 0, i, ninsns = 0, ntotal = 0;
    rl78_xl_leaders ld;
    job_t *jobs;
    sim_cc_job *cjobs;
    size_t njobs = 0;
    char *log, *path, *dllname = NULL, *dllpath = NULL;
    const char **link;
    size_t nlink = 0;
    uint64_t t0 = sim_now_ns();
    uint8_t *cur_rom;
    int rc = -1, failed = 0;
    unsigned b;

    if (x->why_off[0]) { say(x, "変換: できません（%s）\n", x->why_off); return -1; }
    entries = sim_xcalloc(0x100000 / 8, 1);
    memcpy(entries, x->known, 0x100000 / 8);
    for (i = 0; i < 0x100000 / 8; i++) entries[i] |= x->noted[i] | (prof ? prof[i] : 0);
    for (i = 0; i < nextra; i++) if (extra[i] < x->rom_size) setbit(entries, extra[i]);
    list = sim_xmalloc(sizeof *list * 0x10000);
    for (a = 0; a < x->rom_size; a++) {
        if (!bit(entries, a)) continue;
        if (n < 0x10000) list[n++] = a;
        if (!bit(x->known, a)) nnew++;
    }
    /* コードフラッシュの外（RAM の上のコードなど）は変換しないので、一覧からも外す */
    for (a = x->rom_size; a < 0x100000; a++) if (bit(entries, a)) entries[a >> 3] &= (uint8_t)~(1u << (a & 7u));
    if (n == 0 || (nnew == 0 && x->loaded)) { free(entries); free(list); return 0; }

    if (!sim_cc_works(x->cc)) {
        say(x, "変換: clang を起動できません（%s）。sim.cfg の xlate_cc = に clang のパスを書いてください。結果には影響しません（解釈実行のまま）\n", x->cc);
        free(entries);
        free(list);
        return -1;
    }
    if (sim_mkdir_p(x->dir)) {
        say(x, "変換: フォルダ %s を作れません\n", x->dir);
        free(entries);
        free(list);
        return -1;
    }
    say(x, "変換: 新しい関数 %u 個（合計 %u 個）を C に変換してコンパイルします（%d 並列。フォルダ %s）\n",
        (unsigned)nnew, (unsigned)n, x->jobs, x->dir);
    log = join(x->dir, "cc.log");
    sim_remove_file(log);

    /* 変換はいつも開いたときの中身から作る（鍵もそこから計算した）。実行中にテスト CSV の入力などでコードフラッシュが
     * 書き換わっていても、その内容で変換結果を作って保存しない。変換の間だけ中身を戻す */
    cur_rom = sim_xmalloc(x->rom_size);
    rl78_peek(x->cpu, 0, cur_rom, x->rom_size);
    rl78_load(x->cpu, 0, x->rom, x->rom_size);

    /* まとまりごとに C を作る。内容が前回と同じでオブジェクトもあれば、コンパイルし直さない */
    rl78_xl_leaders_init(&ld);
    jobs = sim_xcalloc(NBUCKETS + 2, sizeof *jobs);
    link = sim_xcalloc(NBUCKETS + 16, sizeof *link);
    for (b = 0, i = 0; b < NBUCKETS && i < n; b++) {
        size_t j = i, k = 0;
        sim_buf src;
        char *cpath, *opath;
        while (j < n && (list[j] >> BUCKET_SHIFT) == b) j++;
        if (j == i) continue;
        sim_buf_init(&src);
        if (rl78_xlate_unit(x->cpu, list + i, j - i, entries, buf_put, &src, &ld, &k)) {
            say(x, "変換: メモリが足りません\n");
            sim_buf_free(&src);
            failed = 1;
            break;
        }
        ntotal += k;
        cpath = fmt_name(x->dir, "u%03x", b, ".c");
        opath = fmt_name(x->dir, "u%03x", b, OBJ_EXT);
        if (!(same_content(cpath, src.p ? src.p : "", src.len) && sim_path_kind(opath) == 1)) {
            sim_remove_file(opath);
            if (sim_write_file(cpath, src.p ? src.p : "", src.len)) { say(x, "変換: %s に書けません\n", cpath); failed = 1; }
            jobs[njobs].src = sim_xstrdup(cpath);
            jobs[njobs].obj = sim_xstrdup(opath);
            njobs++;
            ninsns += k;
        }
        link[nlink++] = opath;   /* 持ち主はこの配列 */
        free(cpath);
        sim_buf_free(&src);
        i = j;
    }
    if (!failed) {
        sim_buf src;
        sim_buf_init(&src);
        if (rl78_xlate_main(x->cpu, &ld, buf_put, &src)) failed = 1;
        path = join(x->dir, "main.c");
        if (!failed && sim_write_file(path, src.p ? src.p : "", src.len)) { say(x, "変換: %s に書けません\n", path); failed = 1; }
        jobs[njobs].src = path;
        jobs[njobs].obj = join(x->dir, "main" OBJ_EXT);
        link[nlink++] = sim_xstrdup(jobs[njobs].obj);
        njobs++;
        sim_buf_free(&src);
    }
    rl78_xl_leaders_free(&ld);
    rl78_load(x->cpu, 0, cur_rom, x->rom_size);
    free(cur_rom);

    /* 並列にコンパイルする（遅いファイルがあっても他の枠を止めない） */
    cjobs = sim_xcalloc(njobs ? njobs : 1, sizeof *cjobs);
    if (!failed) {
        say(x, "変換: %u 命令のうち %u 命令（%u ファイル）をコンパイルします\n", (unsigned)ntotal, (unsigned)ninsns, (unsigned)njobs);
        for (i = 0; i < njobs; i++) {
            compile_args(x, jobs[i].argv, jobs[i].src, jobs[i].obj);
            cjobs[i].argv = jobs[i].argv;
            cjobs[i].name = sim_basename(jobs[i].src);
        }
        if (sim_cc_run_jobs(cjobs, njobs, x->jobs, log, "変換", say_line, x)) failed = 1;
        sim_cc_write_times(cjobs, njobs, x->dir, "変換", say_line, x);
        if (failed) say(x, "変換: コンパイルに失敗しました。コンパイラの出力は %s にあります。結果には影響しません（解釈実行のまま）\n", log);
    }

    /* リンク */
    if (!failed) {
        const char **argv = sim_xcalloc(nlink + 16, sizeof *argv);
        size_t k = 0;
        char name[64];
        snprintf(name, sizeof name, "xl-%d" DLL_EXT, x->gen + 1);
        dllname = sim_xstrdup(name);
        dllpath = join(x->dir, name);
        argv[k++] = x->cc;
#if defined(_WIN32)
        argv[k++] = "-shared";
        argv[k++] = "-nostdlib";
        argv[k++] = "-fuse-ld=lld";       /* LLVM の配布物に入っている lld-link。Windows SDK は要らない */
        argv[k++] = "-Wl,/noentry";
#elif defined(__APPLE__)
        argv[k++] = "-dynamiclib";        /* libSystem は OS の一部 */
#else
        argv[k++] = "-shared";
        argv[k++] = "-nostdlib";
#endif
        for (i = 0; i < nlink; i++) argv[k++] = link[i];
        argv[k++] = "-o";
        argv[k++] = dllpath;
        argv[k] = NULL;
        if (sim_cc_run_wait(argv, log) != 0) {
            say(x, "変換: DLL を作れませんでした。コンパイラの出力は %s にあります。結果には影響しません（解釈実行のまま）\n", log);
            failed = 1;
        }
        free(argv);
    }

    if (!failed) {
        sim_buf e;
        char *p;
        /* 一覧を先に書き、最後に current.txt を切り替える（途中で止まっても、前の DLL と一覧の組が残る） */
        sim_buf_init(&e);
        for (i = 0; i < n; i++) sim_buf_printf(&e, "%05X\n", (unsigned)list[i]);
        p = join(x->dir, "entries.txt");
        failed = sim_write_file(p, e.p ? e.p : "", e.len) != 0;
        free(p);
        sim_buf_free(&e);
        if (!failed) {
            p = join(x->dir, "current.txt");
            failed = sim_write_file(p, dllname, strlen(dllname)) != 0;
            free(p);
        }
        if (failed) say(x, "変換: %s に書けません\n", x->dir);
    }
    if (!failed) {
        /* 前の DLL を消す（このプロセスが読み込んでいる間は消せないことがある。そのときは残る） */
        if (x->gen > 0) {
            char *old = fmt_name(x->dir, "xl-%d", (unsigned)x->gen, DLL_EXT);
            sim_remove_file(old);
            free(old);
        }
        x->gen++;
        memcpy(x->known, entries, 0x100000 / 8);
        x->nknown = n;
        say(x, "変換: できました（%.1f 秒）。次の実行から使います\n", (double)(sim_now_ns() - t0) / 1e9);
        rc = (int)nnew;
    }

    for (i = 0; i < njobs; i++) { free(jobs[i].src); free(jobs[i].obj); }
    for (i = 0; i < nlink; i++) free((char *)link[i]);
    free(jobs);
    free(cjobs);
    free(link);
    free(log);
    free(dllname);
    free(dllpath);
    free(entries);
    free(list);
    return failed ? -1 : rc;
}

void rl78_xl_close(rl78_xl *x)
{
    if (!x) return;
    rl78_xl_attach(x->cpu, NULL, NULL, 0);
    free(x->rt_dir);
    free(x->cc);
    free(x->dir);
    free(x->known);
    free(x->noted);
    free(x->rom);
    free(x->dirty);
    free(x);
}
