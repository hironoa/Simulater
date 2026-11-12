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

static line_t *split_lines(const char *d, size_t len, size_t *nout)
{
    size_t cap = 256, n = 0, i = 0;
    line_t *v = sim_xmalloc(cap * sizeof *v);
    while (i < len) {
        size_t s = i;
        line_t L;
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

static int first_is(const line_t *L, const char *word)
{
    char *v = cell_value(L, 0);
    int r = strcmp(v, word) == 0;
#ifdef _WIN32
    if (!r) r = _stricmp(v, word) == 0;
#else
    if (!r) {
        size_t i, n = strlen(word);
        r = strlen(v) == n;
        for (i = 0; r && i < n; i++) if (tolower((unsigned char)v[i]) != tolower((unsigned char)word[i])) r = 0;
    }
#endif
    free(v);
    return r;
}

static int is_mod(const line_t *L) { return first_is(L, "mod"); }

static int first_starts(const line_t *L, char ch)
{
    char *v = cell_value(L, 0);
    int r = v[0] == ch;
    free(v);
    return r;
}

static int all_empty(const line_t *L)
{
    int k;
    for (k = 0; k < L->ncells; k++) {
        size_t i;
        for (i = 0; i < L->cells[k].n; i++)
            if (L->cells[k].p[i] != ' ' && L->cells[k].p[i] != '\t') return 0;
    }
    return 1;
}

static int is_header(const line_t *L)
{
    char *v = cell_value(L, 0);
    int r = strncmp(v, "#COMMENT", 8) == 0 || strncmp(v, "#comment", 8) == 0;
    free(v);
    return r;
}

static void copy_line(sim_buf *out, const line_t *L)
{
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

/* mod ブロックを実行する。戻り値は次の行の添字 */
static size_t run_block(sim_ctx *c, const char *path, line_t *ls, size_t n, size_t i, sim_buf *out,
                        const sim_csv_opts *o, sim_csv_stats *st, int *ng_shown)
{
    char err[1024];
    char *func = NULL, *v;
    int nin = 0, nout = 0, k, failed = 0;
    size_t j, hdr = 0;
    col_t *cols = NULL;
    long b_total = 0, b_ok = 0, b_ng = 0, b_nc = 0;

    copy_line(out, &ls[i]);
    func = cell_value(&ls[i], 1);
    v = cell_value(&ls[i], 3);
    if (parse_count(v, &nin)) { snprintf(err, sizeof err, "mod 行の 4 列目（入力数）'%s' が数値ではありません", v); failed = 1; }
    free(v);
    v = cell_value(&ls[i], 4);
    if (!failed && parse_count(v, &nout)) { snprintf(err, sizeof err, "mod 行の 5 列目（出力数）'%s' が数値ではありません", v); failed = 1; }
    free(v);
    if (!failed && !func[0]) { snprintf(err, sizeof err, "mod 行の 2 列目に対象関数名がありません"); failed = 1; }

    /* #COMMENT 行を探す */
    j = i + 1;
    if (!failed) {
        for (; j < n && !is_mod(&ls[j]); j++) {
            if (is_header(&ls[j])) { hdr = j; break; }
            if (first_starts(&ls[j], ';') || first_starts(&ls[j], '#') || all_empty(&ls[j])) continue;
            snprintf(err, sizeof err, "#COMMENT 行の前にデータ行があります（%d 行目）", ls[j].lineno);
            failed = 1;
            break;
        }
        if (!failed && !hdr) { snprintf(err, sizeof err, "mod '%s' に #COMMENT 行がありません", func); failed = 1; }
    }
    if (failed) {
        report(path, ls[i].lineno, err);
        st->errors++;
        for (j = i + 1; j < n && !is_mod(&ls[j]); j++) copy_line(out, &ls[j]);
        free(func);
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

    for (j = hdr + 1; j < n && !is_mod(&ls[j]); j++) {
        line_t *L = &ls[j];
        sim_buf row;
        int judged_ng = 0, judged_ok = 0;
        uint64_t t0, t1, clk0 = 0, clk1 = 0;
        sim_backend *bk = sim_ctx_backend(c);
        if (failed || first_starts(L, ';') || first_starts(L, '#') || all_empty(L)) {
            copy_line(out, L);
            continue;
        }
        sim_buf_init(&row);
        sim_buf_add(&row, L->cells[0].p, L->cells[0].n);
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
                sim_buf_add(&row, cell.p ? cell.p : "", cell.len);
                if (match == 0) {
                    const char *et = exptext;
                    while (*et == ' ' || *et == '\t') et++;
                    sim_buf_printf(&row, "?(%s)", et);
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
    free(func);
    sim_ctx_set_func(c, NULL, err, sizeof err);
    return j;
}

int sim_csv_run_file(sim_ctx *c, const char *path, const sim_csv_opts *o, sim_csv_stats *st)
{
    size_t len, n, i;
    char *data = sim_read_file(path, &len);
    line_t *ls;
    sim_buf out;
    char *opath;
    int ng_shown = 0;
    if (!data) {
        fprintf(stderr, "エラー: %s を読めません\n", path);
        st->errors++;
        return -1;
    }
    ls = split_lines(data, len, &n);
    sim_buf_init(&out);
    sim_buf_reserve(&out, len + 1024);
    fprintf(stdout, "%s\n", path);
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
    size_t n, i, hdr = 0;
    line_t *ls = split_lines(data, len, &n);
    char *v;
    int k;
    memset(t, 0, sizeof *t);
    for (i = 0; i < n && !is_mod(&ls[i]); i++) {}
    if (i >= n) { snprintf(err, errlen, "mod 行がありません"); free_lines(ls, n); return -1; }
    t->func = cell_value(&ls[i], 1);
    v = cell_value(&ls[i], 3);
    if (parse_count(v, &t->nin)) { snprintf(err, errlen, "%d 行目: 入力数 '%s' が数値ではありません", ls[i].lineno, v); free(v); goto fail; }
    free(v);
    v = cell_value(&ls[i], 4);
    if (parse_count(v, &t->nout)) { snprintf(err, errlen, "%d 行目: 出力数 '%s' が数値ではありません", ls[i].lineno, v); free(v); goto fail; }
    free(v);
    for (i = i + 1; i < n && !is_mod(&ls[i]); i++) if (is_header(&ls[i])) { hdr = i; break; }
    if (!hdr) { snprintf(err, errlen, "#COMMENT 行がありません"); goto fail; }
    if (ls[hdr].ncells < 1 + t->nin + t->nout) { snprintf(err, errlen, "#COMMENT 行の変数名が足りません"); goto fail; }
    t->names = sim_xcalloc((size_t)(t->nin + t->nout) + 1, sizeof *t->names);
    for (k = 0; k < t->nin + t->nout; k++) t->names[k] = cell_value(&ls[hdr], 1 + k);
    for (i = hdr + 1; i < n && !is_mod(&ls[i]); i++) {
        char **row;
        if (first_starts(&ls[i], ';') || first_starts(&ls[i], '#') || all_empty(&ls[i])) continue;
        row = sim_xcalloc((size_t)t->nin + 1, sizeof *row);
        for (k = 0; k < t->nin; k++) {
            char *cv = cell_value(&ls[i], 1 + k);
            if (cv[0]) row[k] = cv; else free(cv);
        }
        t->cells = sim_xrealloc(t->cells, (size_t)(t->nrows + 1) * sizeof *t->cells);
        t->cells[t->nrows++] = row;
    }
    free_lines(ls, n);
    return 0;
fail:
    free_lines(ls, n);
    sim_csv_table_free(t);
    return -1;
}
