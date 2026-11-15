#define _CRT_SECURE_NO_WARNINGS
#include "sim_csv.h"
#include "sim_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ 行とセル */

typedef struct { const char *p; size_t n; } span;

typedef struct {
    span pre;       /* 行の識別では読み飛ばすが出力へは写す先頭のバイト（1 行目の UTF-8 BOM） */
    span text;      /* 改行を含まない行 */
    span eol;       /* 改行（"\r\n"、"\n"、"\r" または空） */
    span *cells;    /* 生のセル（引用符を含む） */
    int ncells;
    int lineno;
} line_t;

static void split_cells(line_t *L)
{
    const char *p = L->text.p, *e = L->text.p + L->text.n;
    int cap = 8;
    L->cells = malloc((size_t)cap * sizeof *L->cells);
    L->ncells = 0;
    if (!L->cells) exit(2);
    for (;;) {
        const char *s = p;
        int inq = 0;
        while (p < e) {
            if (*p == '"') inq = !inq;
            else if (*p == ',' && !inq) break;
            p++;
        }
        if (L->ncells == cap) {
            cap *= 2;
            L->cells = sim_xrealloc(L->cells, (size_t)cap * sizeof *L->cells);
        }
        L->cells[L->ncells].p = s;
        L->cells[L->ncells].n = (size_t)(p - s);
        L->ncells++;
        if (p >= e) break;
        p++; /* , */
    }
}

/* skip: 先頭の何バイトを 1 行目の pre にするか（UTF-8 BOM なら 3） */
static line_t *split_lines(const char *d, size_t len, size_t skip, size_t *nout)
{
    size_t cap = 256, n = 0, i = skip;
    line_t *v = sim_xmalloc(cap * sizeof *v);
    while (i < len || (n == 0 && skip > 0)) {
        size_t s = i;
        line_t L;
        L.pre.p = d;
        L.pre.n = n == 0 ? skip : 0;
        while (i < len && d[i] != '\n' && d[i] != '\r') i++;
        L.text.p = d + s;
        L.text.n = i - s;
        L.eol.p = d + i;
        if (i < len && d[i] == '\r' && i + 1 < len && d[i + 1] == '\n') i += 2;
        else if (i < len) i += 1;
        L.eol.n = (size_t)(d + i - L.eol.p);
        L.lineno = (int)n + 1;
        split_cells(&L);
        if (n == cap) { cap *= 2; v = sim_xrealloc(v, cap * sizeof *v); }
        v[n++] = L;
    }
    *nout = n;
    return v;
}

static void free_lines(line_t *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(v[i].cells);
    free(v);
}

/* 文字コードを確かめる。UTF-8 の BOM なら *skip = 3。UTF-16（BOM が FF FE / FE FF）は対応外としてエラー */
static int check_encoding(const char *d, size_t len, size_t *skip, char *err, size_t errlen)
{
    const unsigned char *u = (const unsigned char *)d;
    *skip = 0;
    if (len >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF))) {
        snprintf(err, errlen, "UTF-16 の CSV には対応していません（先頭が UTF-16 の BOM）。UTF-8 か Shift_JIS で保存し直してください");
        return -1;
    }
    if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) *skip = 3;
    return 0;
}

/* NUL バイトを含む最初の行の番号（なければ 0）。NUL があるとセルを C 文字列にしたときに途中で切れ、
 * 行の識別や値が黙って変わるので、ファイルごとエラーにする（BOM のない UTF-16 もここで止まる） */
static int nul_line(const line_t *ls, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (ls[i].text.n && memchr(ls[i].text.p, 0, ls[i].text.n)) return ls[i].lineno;
    return 0;
}

/* 空白とみなすバイト列の長さ（空白・タブ・全角空白（UTF-8 の E3 80 80、Shift_JIS の 81 40））。違えば 0 */
static size_t ws_len(const char *p, const char *e)
{
    const unsigned char *u = (const unsigned char *)p;
    if (p >= e) return 0;
    if (*p == ' ' || *p == '\t') return 1;
    if (e - p >= 3 && u[0] == 0xE3 && u[1] == 0x80 && u[2] == 0x80) return 3;
    if (e - p >= 2 && u[0] == 0x81 && u[1] == 0x40) return 2;
    return 0;
}

/* 行末側の空白。Shift_JIS の全角空白は直前の 2 バイト文字の 2 バイト目と区別できないので、行末側では見ない */
static size_t ws_len_back(const char *p, const char *e)
{
    const unsigned char *u = (const unsigned char *)e;
    if (e <= p) return 0;
    if (e[-1] == ' ' || e[-1] == '\t') return 1;
    if (e - p >= 3 && u[-3] == 0xE3 && u[-2] == 0x80 && u[-1] == 0x80) return 3;
    return 0;
}

/* セルの値（前後の空白を除き、"..." の引用を外す） */
static char *cell_value(const line_t *L, int k)
{
    const char *p, *e;
    sim_buf b;
    if (k >= L->ncells) return sim_xstrdup("");
    p = L->cells[k].p;
    e = p + L->cells[k].n;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
    sim_buf_init(&b);
    sim_buf_reserve(&b, 0);
    if (e - p >= 2 && *p == '"' && e[-1] == '"') {
        p++;
        e--;
        while (p < e) {
            if (*p == '"' && p + 1 < e && p[1] == '"') p++;
            sim_buf_addc(&b, *p++);
        }
    } else {
        sim_buf_add(&b, p, (size_t)(e - p));
    }
    return b.p;
}

/* 1 列目を行の識別用に整える: 前後の空白（全角空白を含む）を除き、"..." の引用を外して、もう一度空白を除く。
 * Excel が保存した "mod"、行頭の全角空白、" #comment " などを、黙ってデータ行やコメントとして扱わないため */
static char *first_key(const line_t *L)
{
    char *v = cell_value(L, 0), *r;
    const char *p = v, *e = v + strlen(v);
    size_t w;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        while ((w = ws_len(p, e)) > 0) p += w;
        while ((w = ws_len_back(p, e)) > 0) e -= w;
        if (pass == 0 && e - p >= 2 && *p == '"' && e[-1] == '"') { p++; e--; }
        else break;
    }
    r = sim_xstrndup(p, (size_t)(e - p));
    free(v);
    return r;
}

/* s が word（英字は大文字小文字を区別しない）で始まれば 1 */
static int ci_prefix(const char *s, const char *word)
{
    for (; *word; s++, word++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*word)) return 0;
    return 1;
}

/* mod 行: 1 列目が mod（MOD、Mod、前後の空白、"mod" も同じ） */
static int is_mod(const line_t *L)
{
    char *k = first_key(L);
    int r = ci_prefix(k, "mod") && (k[3] == 0 || (tolower((unsigned char)k[3]) == 'i' && k[4] == 0));
    free(k);
    return r;
}

static int first_starts(const line_t *L, char ch)
{
    char *k = first_key(L);
    int r = k[0] == ch;
    free(k);
    return r;
}

static int all_empty(const line_t *L)
{
    int k;
    for (k = 0; k < L->ncells; k++) {
        const char *p = L->cells[k].p, *e = p + L->cells[k].n;
        size_t w;
        while (p < e && (w = ws_len(p, e)) > 0) p += w;
        if (p < e) return 0;
    }
    return 1;
}

/* #COMMENT 行: 1 列目が #COMMENT で始まる（大文字小文字・前後の空白・引用は is_mod と同じく問わない） */
static int is_header(const line_t *L)
{
    char *k = first_key(L);
    int r = ci_prefix(k, "#COMMENT");
    free(k);
    return r;
}

static int is_init(const line_t *L)
{
    char *k = first_key(L);
    int r = ci_prefix(k, "#Init");
    free(k);
    return r;
}

/* Read one optional initialization pair before the first data row. COMMENT
   may appear on either side of the pair. Values column 1 is only a comment. */
static int block_prefix(line_t *ls, size_t n, size_t start, size_t *hdr,
                        sim_csv_table *t, char *err, size_t cap)
{
    size_t j;
    char *key = first_key(&ls[start]);
    int modi = ci_prefix(key, "modi");
    free(key);
    *hdr = 0;
    for (j = start + 1; j < n && !is_mod(&ls[j]); j++) {
        if (is_init(&ls[j])) {
            int k;
            if (t->ninit) { snprintf(err, cap, "%d 行目: #Init 行が複数あります", ls[j].lineno); return -1; }
            if (ls[j].ncells < 2 || j + 1 >= n || is_mod(&ls[j+1]) ||
                first_starts(&ls[j+1], '#') || first_starts(&ls[j+1], ';')) {
                snprintf(err, cap, "%d 行目: #Init の変数名または次の初期値行がありません", ls[j].lineno); return -1;
            }
            t->ninit = ls[j].ncells - 1;
            t->init_names = sim_xcalloc((size_t)t->ninit, sizeof *t->init_names);
            t->init_cells = sim_xcalloc((size_t)t->ninit, sizeof *t->init_cells);
            t->init_header = j; t->init_values = j + 1;
            key = first_key(&ls[j]);
            t->init_every = ci_prefix(key, "#InitWheneverCall") && strlen(key) == strlen("#InitWheneverCall");
            free(key);
            for (k = 0; k < t->ninit; k++) {
                t->init_names[k] = cell_value(&ls[j], k + 1);
                t->init_cells[k] = cell_value(&ls[j+1], k + 1);
            }
            for (k = t->ninit + 1; k < ls[j+1].ncells; k++) {
                char *v = cell_value(&ls[j+1], k);
                int extra = v[0] != 0;
                free(v);
                if (extra) { snprintf(err, cap, "%d 行目: 初期値が変数名の数より多いです", ls[j+1].lineno); return -1; }
            }
            j++;
        } else if (is_header(&ls[j])) {
            if (!*hdr) *hdr = j;
        } else if (!first_starts(&ls[j], '#') && !first_starts(&ls[j], ';') && !all_empty(&ls[j])) {
            if (!*hdr) { snprintf(err, cap, "%d 行目: #COMMENT 行の前にデータ行があります（初期値の複数行は不可）", ls[j].lineno); return -1; }
            break;
        }
    }
    if (!*hdr) { char *func = cell_value(&ls[start], 1); snprintf(err, cap, "mod '%s' に #COMMENT 行がありません", func); free(func); return -1; }
    if (modi && !t->ninit) t->warning = sim_xstrdup("modi ですが #Init 行がありません。初期化なしで実行します");
    if (!modi && t->ninit) t->warning = sim_xstrdup("mod ブロックの #Init 行を初期化として受け付けます");
    return 0;
}

static void copy_line(sim_buf *out, const line_t *L)
{
    sim_buf_add(out, L->pre.p, L->pre.n);
    sim_buf_add(out, L->text.p, L->text.n);
    sim_buf_add(out, L->eol.p, L->eol.n);
}

static int parse_count(const char *s, int *out)
{
    char *end;
    long v;
    if (!*s) return -1;
    v = strtol(s, &end, 10);
    if (*end || v < 0 || v > 100000) return -1;
    *out = (int)v;
    return 0;
}

void sim_csv_cell(sim_buf *b, const char *v)
{
    size_t n = strlen(v);
    int q = n > 0 && (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t');
    const char *p;
    if (!q && (strchr(v, ',') || strchr(v, '"'))) q = 1;
    if (!q) { sim_buf_adds(b, v); return; }
    sim_buf_addc(b, '"');
    for (p = v; *p; p++) {
        if (*p == '"') sim_buf_addc(b, '"');
        sim_buf_addc(b, *p);
    }
    sim_buf_addc(b, '"');
}

/* "実測?(期待)" を分ける。分けられたら 1 */
static int split_judged(const char *v, char **actual, char **expected)
{
    const char *p = v;
    int inq = 0;
    size_t n = strlen(v);
    if (n < 4 || v[n - 1] != ')') return 0;
    for (; *p; p++) {
        if (*p == '\'') inq = !inq;
        else if (!inq && p[0] == '?' && p[1] == '(') {
            *actual = sim_xstrndup(v, (size_t)(p - v));
            *expected = sim_xstrndup(p + 2, n - (size_t)(p - v) - 3);
            return 1;
        }
    }
    return 0;
}

/* ================================================================ 実行 */

typedef struct {
    sim_ref *ref;
    sim_fmt fmt;
} col_t;

static void report(const char *path, int line, const char *msg)
{
    fprintf(stderr, "エラー: %s:%d: %s\n", path, line, msg);
}

static void warn(const char *path, int line, const char *msg)
{
    fprintf(stderr, "警告: %s:%d: %s\n", path, line, msg);
}

/* mod ブロックを実行する。戻り値は次の行の添字 */
static size_t run_block(sim_ctx *c, const char *path, line_t *ls, size_t n, size_t i, sim_buf *out,
                        const sim_csv_opts *o, sim_csv_stats *st, int *ng_shown)
{
    char err[1024];
    char *func = NULL, *v;
    int nin = 0, nout = 0, k, failed = 0;
    size_t j, hdr = 0;
    col_t *cols = NULL;
    sim_csv_table init = {0};
    sim_ref **init_refs = NULL;
    sim_cellval *init_vals = NULL;
    int initialized = 0, every = 0;
    long b_total = 0, b_ok = 0, b_ng = 0, b_nc = 0, b_rows = 0;

    copy_line(out, &ls[i]);
    func = cell_value(&ls[i], 1);
    v = cell_value(&ls[i], 3);
    if (parse_count(v, &nin)) { snprintf(err, sizeof err, "mod 行の 4 列目（入力数）'%s' が数値ではありません", v); failed = 1; }
    free(v);
    v = cell_value(&ls[i], 4);
    if (!failed && parse_count(v, &nout)) { snprintf(err, sizeof err, "mod 行の 5 列目（出力数）'%s' が数値ではありません", v); failed = 1; }
    free(v);
    if (!failed && !func[0]) { snprintf(err, sizeof err, "mod 行の 2 列目に対象関数名がありません"); failed = 1; }

    if (!failed && block_prefix(ls, n, i, &hdr, &init, err, sizeof err)) failed = 1;
    if (init.warning) warn(path, ls[i].lineno, init.warning);
    if (failed) {
        report(path, ls[i].lineno, err);
        st->errors++;
        for (j = i + 1; j < n && !is_mod(&ls[j]); j++) copy_line(out, &ls[j]);
        free(func);
        sim_csv_table_free(&init);
        return j;
    }
    for (j = i + 1; j < hdr; j++) copy_line(out, &ls[j]);
    copy_line(out, &ls[hdr]);

    /* 対象関数と列 */
    {
        sim_backend *b = sim_ctx_backend(c);
        const sim_func *f;
        if (b->reset(b, err, sizeof err)) { report(path, ls[i].lineno, err); failed = 1; }
        if (!failed) {
            f = sim_ctx_find_func(c, func, err, sizeof err);
            if (!f || sim_ctx_set_func(c, f, err, sizeof err)) { report(path, ls[i].lineno, err); failed = 1; }
        }
    }
    if (!failed && ls[hdr].ncells < 1 + nin + nout) {
        snprintf(err, sizeof err, "#COMMENT 行の変数名が %d 個しかありません（入力 %d + 出力 %d 個が必要）",
                 ls[hdr].ncells - 1, nin, nout);
        report(path, ls[hdr].lineno, err);
        failed = 1;
    }
    if (!failed) {
        cols = sim_xcalloc((size_t)(nin + nout) + 1, sizeof *cols);
        for (k = 0; k < nin + nout; k++) {
            char *name = cell_value(&ls[hdr], 1 + k);
            cols[k].ref = sim_ref_parse(c, name, err, sizeof err);
            if (!cols[k].ref) {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目: %s", 2 + k, err);
                report(path, ls[hdr].lineno, m2);
                failed = 1;
            } else if (k < nin && sim_ref_is_return(cols[k].ref)) {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目: '%s' は戻り値なので入力にできません", 2 + k, name);
                report(path, ls[hdr].lineno, m2);
                failed = 1;
            } else if (k < nin && !sim_ref_writable(cols[k].ref)) {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目: '%s' には書き込めません（const または集合型）", 2 + k, name);
                report(path, ls[hdr].lineno, m2);
                failed = 1;
            } else if (k >= nin && sim_ref_prefix(cols[k].ref) == '$') {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目: '$' は入力にだけ使えます", 2 + k);
                report(path, ls[hdr].lineno, m2);
                failed = 1;
            }
            free(name);
        }
    }

    every = sim_ctx_backend(c)->csv_init == 2 || (!sim_ctx_backend(c)->csv_init && init.init_every);
    init_refs = sim_xcalloc((size_t)init.ninit + 1, sizeof *init_refs);
    init_vals = sim_xcalloc((size_t)init.ninit + 1, sizeof *init_vals);
    for (k = 0; k < init.ninit && !failed; k++) {
        init_refs[k] = sim_ref_parse(c, init.init_names[k], err, sizeof err);
        if (!init_refs[k] || !sim_ref_writable(init_refs[k]) || sim_ref_is_return(init_refs[k])) {
            if (init_refs[k]) snprintf(err, sizeof err, "初期化変数 %s には書き込めません", init.init_names[k]);
            failed = 1;
        } else if (sim_parse_cell(init.init_cells[k], strlen(init.init_cells[k]), &init_vals[k], err, sizeof err) ||
                   sim_ref_check_value(init_refs[k], &init_vals[k], err, sizeof err)) failed = 1;
        if (failed) report(path, ls[init.init_values].lineno, err);
    }
    for (j = hdr + 1; j < n && !is_mod(&ls[j]); j++) {
        line_t *L = &ls[j];
        sim_buf row;
        int judged_ng = 0, judged_ok = 0;
        uint64_t t0, t1, clk0 = 0, clk1 = 0;
        sim_backend *bk = sim_ctx_backend(c);
        int skipped = first_starts(L, ';') || first_starts(L, '#') || all_empty(L);
        if (init.ninit && (j == init.init_header || j == init.init_values)) { copy_line(out, L); continue; }
        if (is_init(L)) { report(path, L->lineno, "#Init 行が複数あるか、データ行の後にあります"); failed = 1; }

        if (!skipped) b_rows++;
        if (failed || skipped) {
            copy_line(out, L);
            continue;
        }
        sim_buf_init(&row);
        sim_buf_add(&row, L->cells[0].p, L->cells[0].n);
        /* Initial values precede row inputs, so overlapping row inputs win. */
        if (every || !initialized) {
            for (k = 0; k < init.ninit && !failed; k++)
                if (sim_ref_write(c, init_refs[k], &init_vals[k], err, sizeof err)) {
                    report(path, L->lineno, err); failed = 1;
                }
            initialized = 1;
        }
        /* 入力 */
        for (k = 0; k < nin && !failed; k++) {
            char *cv = cell_value(L, 1 + k);
            sim_cellval val;
            if (sim_parse_cell(cv, strlen(cv), &val, err, sizeof err) ||
                sim_ref_write(c, cols[k].ref, &val, err, sizeof err)) {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目 '%s': %s", 2 + k, sim_ref_text(cols[k].ref), err);
                report(path, L->lineno, m2);
                failed = 1;
            }
            sim_cellval_free(&val);
            sim_buf_addc(&row, ',');
            if (k + 1 < L->ncells) sim_buf_add(&row, L->cells[1 + k].p, L->cells[1 + k].n);
            free(cv);
        }
        if (failed) { sim_buf_free(&row); copy_line(out, L); continue; }
        /* 呼び出し */
        if (bk->exec_stats) { sim_exec_stats es; bk->exec_stats(bk, &es); clk0 = es.clocks; }
        t0 = sim_now_ns();
        if (sim_ctx_call(c, err, sizeof err)) {
            /* 暴走・未定義命令・戻らないなど: その行を NG にして続ける */
            t1 = sim_now_ns();
            st->seconds += (double)(t1 - t0) / 1e9;
            report(path, L->lineno, err);
            if (!o->quiet && *ng_shown < 20) {
                fprintf(stdout, "  NG %s:%d 実行エラー: %s\n", sim_basename(path), L->lineno, err);
                (*ng_shown)++;
            }
            for (k = nin; k < nin + nout; k++) {
                sim_buf_addc(&row, ',');
                if (1 + k < L->ncells) sim_buf_add(&row, L->cells[1 + k].p, L->cells[1 + k].n);
            }
            b_total++;
            b_ng++;
            sim_buf_adds(&row, ",NG");
            if (bk->exec_stats) { sim_exec_stats es; bk->exec_stats(bk, &es); clk1 = es.clocks; }
            if (o->fixed_time) sim_buf_adds(&row, ",0.0000ms");
            else if (bk->exec_stats && bk->cpu_hz > 0) sim_buf_printf(&row, ",%.4fms", (double)(clk1 - clk0) / bk->cpu_hz * 1e3);
            else sim_buf_printf(&row, ",%.4fms", (double)(t1 - t0) / 1e6);
            sim_buf_add(out, row.p, row.len);
            sim_buf_add(out, L->eol.p, L->eol.n);
            sim_buf_free(&row);
            continue;
        }
        t1 = sim_now_ns();
        if (bk->exec_stats) { sim_exec_stats es; bk->exec_stats(bk, &es); clk1 = es.clocks; }
        st->seconds += (double)(t1 - t0) / 1e9;
        /* 出力 */
        for (k = nin; k < nin + nout && !failed; k++) {
            char *cv = cell_value(L, 1 + k), *act = NULL, *exp = NULL;
            const char *exptext = cv;
            sim_cellval ev;
            int match = -1;
            sim_buf cell;
            if (split_judged(cv, &act, &exp)) exptext = o->expect_actual ? act : exp;
            sim_buf_init(&cell);
            if (sim_parse_cell(exptext, strlen(exptext), &ev, err, sizeof err)) {
                char m2[1200];
                snprintf(m2, sizeof m2, "%d 列目 '%s' の期待値: %s", 2 + k, sim_ref_text(cols[k].ref), err);
                report(path, L->lineno, m2);
                failed = 1;
            } else {
                if (sim_cellval_present(&ev)) {
                    if (!ev.is_string && ev.items[0].hex_digits > 0) {
                        cols[k].fmt.hex_digits = ev.items[0].hex_digits;
                        cols[k].fmt.upper = ev.items[0].hex_upper;
                        cols[k].fmt.prefix_upper = ev.items[0].hex_prefix_upper;
                    } else {
                        cols[k].fmt.hex_digits = 0;
                    }
                }
                if (sim_ref_check(c, cols[k].ref, &ev, &cols[k].fmt, &cell, &match, err, sizeof err)) {
                    char m2[1200];
                    snprintf(m2, sizeof m2, "%d 列目 '%s': %s", 2 + k, sim_ref_text(cols[k].ref), err);
                    report(path, L->lineno, m2);
                    failed = 1;
                }
            }
            sim_cellval_free(&ev);
            sim_buf_addc(&row, ',');
            if (!failed) {
                /* 実測値（不一致なら "実測?(期待)"）を組み立ててから、セル全体を CSV として引用する。
                 * 期待値の文字列は入力のときに引用を外してあるので、',' や '"' を含むと列が割れる */
                sim_buf res;
                sim_buf_init(&res);
                sim_buf_add(&res, cell.p ? cell.p : "", cell.len);
                if (match == 0) {
                    const char *et = exptext;
                    while (*et == ' ' || *et == '\t') et++;
                    sim_buf_printf(&res, "?(%s)", et);
                }
                sim_csv_cell(&row, sim_buf_cstr(&res));
                sim_buf_free(&res);
                if (match == 0) {
                    const char *et = exptext;
                    while (*et == ' ' || *et == '\t') et++;
                    judged_ng = 1;
                    if (!o->quiet && *ng_shown < 20) {
                        fprintf(stdout, "  NG %s:%d %s = %s（期待 %s）\n", sim_basename(path), L->lineno,
                                sim_ref_text(cols[k].ref), sim_buf_cstr(&cell), et);
                        (*ng_shown)++;
                    }
                } else if (match == 1) {
                    judged_ok = 1;
                }
            }
            sim_buf_free(&cell);
            free(cv);
            free(act);
            free(exp);
        }
        if (failed) { sim_buf_free(&row); copy_line(out, L); continue; }
        b_total++;
        if (judged_ng) { b_ng++; sim_buf_adds(&row, ",NG"); }
        else if (judged_ok) { b_ok++; sim_buf_adds(&row, ",OK"); }
        else { b_nc++; sim_buf_adds(&row, ",NO Check"); }
        if (o->fixed_time) sim_buf_adds(&row, ",0.0000ms");
        else if (bk->exec_stats && bk->cpu_hz > 0) sim_buf_printf(&row, ",%.4fms", (double)(clk1 - clk0) / bk->cpu_hz * 1e3);
        else sim_buf_printf(&row, ",%.4fms", (double)(t1 - t0) / 1e6);
        sim_buf_add(out, row.p, row.len);
        sim_buf_add(out, L->eol.p, L->eol.n);
        sim_buf_free(&row);
    }
    if (failed) st->errors++;
    else if (b_rows == 0) {
        /* コメントアウトだけの雛形 CSV を壊さないよう、エラーにはしない。ただし黙って 0 件の成功にもしない */
        snprintf(err, sizeof err, "mod '%s' に実行するデータ行がありません（すべて ; 行・# 行・空行）", func);
        warn(path, ls[i].lineno, err);
    }
    fprintf(stdout, "  mod %s: 総数 %ld, OK %ld, NG %ld, NO Check %ld%s\n", func, b_total, b_ok, b_ng, b_nc,
            failed ? "（エラーで中断）" : "");
    st->total += b_total;
    st->ok += b_ok;
    st->ng += b_ng;
    st->nocheck += b_nc;
    if (cols) {
        for (k = 0; k < nin + nout; k++) sim_ref_free(cols[k].ref);
        free(cols);
    }
    for (k = 0; k < init.ninit; k++) { sim_ref_free(init_refs[k]); sim_cellval_free(&init_vals[k]); }
    free(init_refs); free(init_vals); sim_csv_table_free(&init);
    free(func);
    sim_ctx_set_func(c, NULL, err, sizeof err);
    return j;
}

/* まだ存在しない出力同士はファイル ID では比べられない。
 * 全入力で outdir は共通なので、同じ出力パスになるファイル名を ASCII の大小を無視して比べる。 */
static int output_name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + ('a' - 'A'));
        if (ac != bc) return 0;
    }
    return *a == *b;
}

int sim_csv_check_outputs(const sim_csv_opts *o)
{
    char **paths = sim_xcalloc((size_t)o->ninputs + 1, sizeof *paths);
    int i, j, collision = 0;
    for (i = 0; i < o->ninputs; i++)
        paths[i] = sim_path_join(o->outdir, sim_basename(o->inputs[i]));
    for (i = 0; i < o->ninputs && !collision; i++) {
        for (j = 0; j < i; j++) {
            if (output_name_equal(sim_basename(paths[i]), sim_basename(paths[j]))) {
                fprintf(stderr, "エラー: 入力 %s と %s の出力先が衝突します（出力先: %s）。別々に実行するか、出力フォルダを分けてください\n",
                        o->inputs[j], o->inputs[i], paths[j]);
                collision = 1;
                break;
            }
        }
    }
    for (i = 0; i < o->ninputs; i++) free(paths[i]);
    free(paths);
    return collision ? -1 : 0;
}

int sim_csv_run_file(sim_ctx *c, const char *path, const sim_csv_opts *o, sim_csv_stats *st)
{
    size_t len, n, i, skip, first_mod;
    char *data = sim_read_file(path, &len);
    line_t *ls;
    sim_buf out;
    char *opath;
    char err[512];
    int ng_shown = 0, bad;
    if (!data) {
        fprintf(stderr, "エラー: %s を読めません\n", path);
        st->errors++;
        return -1;
    }
    fprintf(stdout, "%s\n", path);
    if (check_encoding(data, len, &skip, err, sizeof err)) {
        fprintf(stderr, "エラー: %s: %s\n", path, err);
        st->errors++;
        free(data);
        return -1;
    }
    ls = split_lines(data, len, skip, &n);
    if ((bad = nul_line(ls, n)) > 0) {
        report(path, bad, "NUL バイト（0x00）を含む行があります。UTF-16 など対応していない文字コードで保存されていないか確かめてください");
        st->errors++;
        free_lines(ls, n); free(data);
        return -1;
    }
    for (first_mod = 0; first_mod < n && !is_mod(&ls[first_mod]); first_mod++) {}
    if (first_mod >= n) {
        fprintf(stderr, "エラー: %s: mod 行がありません（1 列目が mod の行がないので、実行するテストがありません）\n", path);
        st->errors++;
        free_lines(ls, n); free(data);
        return -1;
    }
    {
        /* 最初の mod 行より前の行はどのテストにも属さない。; 行と空行のほかは、mod 行の書き間違いの可能性があるので警告する */
        int lo = 0, hi = 0;
        for (i = 0; i < first_mod; i++) {
            if (first_starts(&ls[i], ';') || all_empty(&ls[i])) continue;
            if (!lo) lo = ls[i].lineno;
            hi = ls[i].lineno;
        }
        if (lo) {
            if (lo == hi) snprintf(err, sizeof err, "最初の mod 行より前に、どのテストにも属さない行があります（%d 行目）。実行しません", lo);
            else snprintf(err, sizeof err, "最初の mod 行より前に、どのテストにも属さない行があります（%d〜%d 行目）。実行しません", lo, hi);
            warn(path, lo, err);
        }
    }
    sim_buf_init(&out);
    sim_buf_reserve(&out, len + 1024);
    for (i = 0; i < n;) {
        if (is_mod(&ls[i])) i = run_block(c, path, ls, n, i, &out, o, st, &ng_shown);
        else copy_line(&out, &ls[i++]);
    }
    if (sim_mkdir_p(o->outdir)) {
        fprintf(stderr, "エラー: 出力フォルダ %s を作れません\n", o->outdir);
        st->errors++;
        free_lines(ls, n); free(data); sim_buf_free(&out);
        return -1;
    }
    opath = sim_path_join(o->outdir, sim_basename(path));
    {
        /* 入力（この実行で渡した他の入力も）を上書きしない。パスの表記ではなくファイルそのもので比べる。
         * 出力先がまだなければ同じファイルではありえないので、そのまま書く */
        const char *same = sim_same_file(opath, path) ? path : NULL;
        int k;
        for (k = 0; !same && o->inputs && k < o->ninputs; k++)
            if (sim_same_file(opath, o->inputs[k])) same = o->inputs[k];
        if (same) {
            fprintf(stderr, "エラー: 結果の出力先 %s が入力と同じです（入力: %s）。-o で別のフォルダを指定してください\n", opath, same);
            free(opath); free_lines(ls, n); free(data); sim_buf_free(&out);
            st->errors++;
            return -1;
        }
    }
    if (sim_write_file(opath, out.p ? out.p : "", out.len)) {
        fprintf(stderr, "エラー: %s を書けません\n", opath);
        st->errors++;
        free(opath); free_lines(ls, n); free(data); sim_buf_free(&out);
        return -1;
    }
    fprintf(stdout, "  -> %s\n", opath);
    free(opath);
    free_lines(ls, n);
    free(data);
    sim_buf_free(&out);
    return 0;
}

/* ================================================================ GUI 用の表 */

void sim_csv_table_free(sim_csv_table *t)
{
    for (int k = 0; k < t->ninit; k++) { free(t->init_names[k]); free(t->init_cells[k]); }
    free(t->init_names); free(t->init_cells); free(t->warning);
    int r, k;
    free(t->func);
    for (k = 0; k < t->nin + t->nout; k++) free(t->names ? t->names[k] : NULL);
    free(t->names);
    for (r = 0; r < t->nrows; r++) {
        for (k = 0; k < t->nin; k++) free(t->cells[r][k]);
        free(t->cells[r]);
    }
    free(t->cells);
    memset(t, 0, sizeof *t);
}

int sim_csv_load_table(const char *data, size_t len, sim_csv_table *t, char *err, size_t errlen)
{
    size_t n, i, hdr = 0, skip;
    line_t *ls;
    char *v;
    int k;
    memset(t, 0, sizeof *t);
    if (check_encoding(data, len, &skip, err, errlen)) return -1;
    ls = split_lines(data, len, skip, &n);
    if ((k = nul_line(ls, n)) > 0) {
        snprintf(err, errlen, "%d 行目に NUL バイト（0x00）があります。UTF-16 など対応していない文字コードで保存されていないか確かめてください", k);
        free_lines(ls, n);
        return -1;
    }
    for (i = 0; i < n && !is_mod(&ls[i]); i++) {}
    if (i >= n) { snprintf(err, errlen, "mod 行がありません"); free_lines(ls, n); return -1; }
    t->func = cell_value(&ls[i], 1);
    v = cell_value(&ls[i], 3);
    if (parse_count(v, &t->nin)) { snprintf(err, errlen, "%d 行目: 入力数 '%s' が数値ではありません", ls[i].lineno, v); free(v); goto fail; }
    free(v);
    v = cell_value(&ls[i], 4);
    if (parse_count(v, &t->nout)) { snprintf(err, errlen, "%d 行目: 出力数 '%s' が数値ではありません", ls[i].lineno, v); free(v); goto fail; }
    free(v);
    if (block_prefix(ls, n, i, &hdr, t, err, errlen)) goto fail;
    if (ls[hdr].ncells < 1 + t->nin + t->nout) { snprintf(err, errlen, "#COMMENT 行の変数名が足りません"); goto fail; }
    t->names = sim_xcalloc((size_t)(t->nin + t->nout) + 1, sizeof *t->names);
    for (k = 0; k < t->nin + t->nout; k++) t->names[k] = cell_value(&ls[hdr], 1 + k);
    for (i = hdr + 1; i < n && !is_mod(&ls[i]); i++) {
        char **row;
        if (t->ninit && (i == t->init_header || i == t->init_values)) continue;
        if (is_init(&ls[i])) { snprintf(err, errlen, "#Init 行が複数あるか、データ行の後にあります"); goto fail; }
        if (first_starts(&ls[i], ';') || first_starts(&ls[i], '#') || all_empty(&ls[i])) continue;
        row = sim_xcalloc((size_t)t->nin + 1, sizeof *row);
        for (k = 0; k < t->nin; k++) {
            char *cv = cell_value(&ls[i], 1 + k);
            if (cv[0]) row[k] = cv; else free(cv);
        }
        t->cells = sim_xrealloc(t->cells, (size_t)(t->nrows + 1) * sizeof *t->cells);
        t->cells[t->nrows++] = row;
    }
    for (t->nblocks = 1; i < n; i++) if (is_mod(&ls[i])) t->nblocks++;
    free_lines(ls, n);
    return 0;
fail:
    free_lines(ls, n);
    sim_csv_table_free(t);
    return -1;
}

void sim_csv_table_expand_init(sim_csv_table *t, int mode)
{
    int k, row, old = t->nin, n = t->ninit;
    int every = mode == 2 || (!mode && t->init_every);
    char **names;
    if (!n) return;
    names = sim_xcalloc((size_t)(n + old + t->nout), sizeof *names);
    for (k = 0; k < n; k++) names[k] = sim_xstrdup(t->init_names[k]);
    for (k = 0; k < old + t->nout; k++) names[n+k] = t->names[k];
    free(t->names); t->names = names;
    for (row = 0; row < t->nrows; row++) {
        char **cells = sim_xcalloc((size_t)(n + old), sizeof *cells);
        for (k = 0; k < n; k++)
            if ((every || row == 0) && t->init_cells[k][0]) cells[k] = sim_xstrdup(t->init_cells[k]);
        for (k = 0; k < old; k++) cells[n+k] = t->cells[row][k];
        free(t->cells[row]); t->cells[row] = cells;
    }
    t->nin += n; t->init_columns = n;
}
