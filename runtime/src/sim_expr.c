#define _CRT_SECURE_NO_WARNINGS
#include "sim_expr.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ 文脈 */

typedef struct {
    char *name;
    const sim_type *type;
    int is_const;
} sym_entry;

typedef struct {
    sim_var var;
    int bit_offset, priority;
    unsigned bit_size;
} defined_var;

struct sim_ctx {
    sim_backend *b;
    const sim_var **vars;
    size_t nvars;
    const sim_func **funcs;
    size_t nfuncs;
    sym_entry *syms;
    size_t nsyms;
    const sim_func *func;
    int64_t args[SIM_MAX_ARGS];
    uint64_t ret_raw;
    void **owned;
    size_t nowned;
    defined_var **defined;
    size_t ndefined;
    char *definition_info;
};

static int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static void build_syms(sim_ctx *c)
{
    size_t i, j;
    c->syms = sim_xcalloc(c->nvars + 1, sizeof *c->syms);
    for (i = 0; i < c->nvars; i++) {
        const sim_var *v = c->vars[i];
        sim_buf nb;
        int dup = 0;
        sim_buf_init(&nb);
        if (v->func) {
            for (j = 0; j < c->nvars; j++)
                if (j != i && c->vars[j]->func && streq(c->vars[j]->func, v->func) && streq(c->vars[j]->name, v->name)) dup = 1;
            if (dup) sim_buf_printf(&nb, "%s/", sim_basename(v->file ? v->file : ""));
            sim_buf_printf(&nb, "%s@%s", v->func, v->name);
        } else if (v->is_external) {
            sim_buf_adds(&nb, v->name);
        } else {
            for (j = 0; j < c->nvars; j++)
                if (j != i && !c->vars[j]->func && streq(c->vars[j]->name, v->name)) dup = 1;
            if (dup) sim_buf_printf(&nb, "%s/", sim_basename(v->file ? v->file : ""));
            sim_buf_adds(&nb, v->name);
        }
        c->syms[c->nsyms].name = nb.p;
        c->syms[c->nsyms].type = v->type;
        c->syms[c->nsyms].is_const = v->is_const;
        c->nsyms++;
    }
}

sim_ctx *sim_ctx_new(sim_backend *b)
{
    sim_ctx *c = sim_xcalloc(1, sizeof *c);
    size_t i;
    c->b = b;
    c->nvars = b->var_count(b);
    c->vars = sim_xcalloc(c->nvars + 1, sizeof *c->vars);
    for (i = 0; i < c->nvars; i++) c->vars[i] = b->var_at(b, i);
    c->nfuncs = b->func_count(b);
    c->funcs = sim_xcalloc(c->nfuncs + 1, sizeof *c->funcs);
    for (i = 0; i < c->nfuncs; i++) c->funcs[i] = b->func_at(b, i);
    build_syms(c);
    return c;
}

void sim_ctx_free(sim_ctx *c)
{
    size_t i;
    if (!c) return;
    for (i = 0; i < c->nsyms; i++) free(c->syms[i].name);
    free(c->syms);
    free(c->vars);
    free(c->funcs);
    for (i = 0; i < c->nowned; i++) free(c->owned[i]);
    free(c->owned); free(c->defined); free(c->definition_info);
    free(c);
}

void *sim_ctx_alloc(sim_ctx *c, size_t n)
{
    void *p = sim_xcalloc(n ? n : 1, 1);
    c->owned = sim_xrealloc(c->owned, (c->nowned + 1) * sizeof *c->owned);
    c->owned[c->nowned++] = p;
    return p;
}
static char *ctx_string(sim_ctx *c, const char *s)
{
    char *p = sim_ctx_alloc(c, strlen(s) + 1);
    strcpy(p, s); return p;
}
void sim_ctx_refresh_symbols(sim_ctx *c)
{
    size_t i;
    for (i = 0; i < c->nsyms; i++) free(c->syms[i].name);
    free(c->syms); c->syms = NULL; c->nsyms = 0;
    build_syms(c);
}
int sim_ctx_define(sim_ctx *c, const char *name, const sim_type *type, uint64_t addr,
                   int bit_offset, unsigned bit_size, const char *file, int priority)
{
    size_t i, slot = c->nvars, k;
    defined_var *d;
    int replaced;
    for (i = 0; i < c->nvars; i++) if (!c->vars[i]->func && !strcmp(c->vars[i]->name, name)) {
        int old_priority = 1;
        for (k = 0; k < c->ndefined; k++) if (&c->defined[k]->var == c->vars[i]) old_priority = c->defined[k]->priority;
        if (old_priority > priority) return 0;
        slot = i; break;
    }
    replaced = slot < c->nvars;
    d = sim_ctx_alloc(c, sizeof *d);
    d->var.name = ctx_string(c, name); d->var.file = ctx_string(c, file ? file : "");
    d->var.addr = addr; d->var.type = type; d->var.is_external = 1;
    d->bit_offset = bit_offset; d->bit_size = bit_size; d->priority = priority;
    c->defined = sim_xrealloc(c->defined, (c->ndefined + 1) * sizeof *c->defined);
    c->defined[c->ndefined++] = d;
    if (slot == c->nvars) {
        c->vars = sim_xrealloc(c->vars, (c->nvars + 1) * sizeof *c->vars); c->nvars++;
    }
    c->vars[slot] = &d->var;
    return replaced ? 2 : 1;
}
const sim_var *sim_ctx_sym_var(sim_ctx *c, size_t i) { return i < c->nvars ? c->vars[i] : NULL; }
const sim_type *sim_ctx_named_type(sim_ctx *c, const char *name)
{
    size_t i;
    for (i = 0; i < c->nvars; i++) {
        const sim_type *t = c->vars[i]->type;
        unsigned depth = 0;
        while (t && depth++ < 16) {
            const char *s = t->name ? t->name : "";
            if (!strcmp(s, name) || (!strncmp(s, "struct ", 7) && !strcmp(s + 7, name)) ||
                (!strncmp(s, "union ", 6) && !strcmp(s + 6, name))) return t;
            t = t->target;
        }
    }
    return NULL;
}
void sim_ctx_set_definition_info(sim_ctx *c, const char *info)
{ free(c->definition_info); c->definition_info = sim_xstrdup(info); }
void sim_ctx_print_definition_info(sim_ctx *c, FILE *out)
{ if (c->definition_info) fputs(c->definition_info, out); }

sim_backend *sim_ctx_backend(sim_ctx *c) { return c->b; }
size_t sim_ctx_sym_count(sim_ctx *c) { return c->nsyms; }
const char *sim_ctx_sym_name(sim_ctx *c, size_t i) { return c->syms[i].name; }
const sim_type *sim_ctx_sym_type(sim_ctx *c, size_t i) { return c->syms[i].type; }
int sim_ctx_sym_const(sim_ctx *c, size_t i) { return c->syms[i].is_const; }
const sim_func *sim_ctx_func(sim_ctx *c) { return c->func; }
void sim_ctx_clear_args(sim_ctx *c) { memset(c->args, 0, sizeof c->args); c->ret_raw = 0; }

/* "file.c/func" を分ける。path は NULL 可 */
static void split_path(const char *s, char *path, size_t pn, const char **rest)
{
    const char *sl = strrchr(s, '/');
    if (!sl) { if (path) path[0] = 0; *rest = s; return; }
    if (path) {
        size_t n = (size_t)(sl - s);
        if (n >= pn) n = pn - 1;
        memcpy(path, s, n);
        path[n] = 0;
    }
    *rest = sl + 1;
}

const sim_func *sim_ctx_find_func(sim_ctx *c, const char *name, char *err, size_t errlen)
{
    char path[512];
    const char *fn;
    const sim_func *found = NULL, *ext = NULL;
    int n = 0;
    size_t i;
    split_path(name, path, sizeof path, &fn);
    for (i = 0; i < c->nfuncs; i++) {
        const sim_func *f = c->funcs[i];
        if (!streq(f->name, fn)) continue;
        if (path[0] && !sim_path_suffix_match(f->file ? f->file : "", path)) continue;
        n++;
        found = f;
        if (f->is_external) ext = f;
    }
    if (n == 1) return found;
    if (n > 1 && ext && !path[0]) return ext;
    if (n > 1) {
        sim_buf b;
        sim_buf_init(&b);
        for (i = 0; i < c->nfuncs; i++)
            if (streq(c->funcs[i]->name, fn)) sim_buf_printf(&b, " %s/%s", sim_basename(c->funcs[i]->file ? c->funcs[i]->file : ""), fn);
        snprintf(err, errlen, "関数 '%s' が複数あります。ファイル名を付けてください:%s", name, sim_buf_cstr(&b));
        sim_buf_free(&b);
        return NULL;
    }
    {
        sim_buf b;
        int k = 0;
        sim_buf_init(&b);
        for (i = 0; i < c->nfuncs && k < 5; i++)
            if (sim_strcasestr_found(c->funcs[i]->name, fn) || sim_strcasestr_found(fn, c->funcs[i]->name)) {
                sim_buf_printf(&b, "%s%s", k ? ", " : "", c->funcs[i]->name);
                k++;
            }
        if (k) snprintf(err, errlen, "関数 '%s' が見つかりません（候補: %s）", name, sim_buf_cstr(&b));
        else snprintf(err, errlen, "関数 '%s' が見つかりません", name);
        sim_buf_free(&b);
    }
    return NULL;
}

static int scalar_kind(const sim_type *t)
{
    return t && (t->kind == SIM_T_INT || t->kind == SIM_T_UINT || t->kind == SIM_T_BOOL ||
                 t->kind == SIM_T_PTR || t->kind == SIM_T_FLOAT);
}

int sim_ctx_check_func(const sim_func *f, char *err, size_t errlen)
{
    int i;
    if (!f) return 0;
    if (f->nparams > SIM_MAX_ARGS) {
        snprintf(err, errlen, "関数 '%s' の引数が %d 個あります。対応は %d 個までです", f->name, f->nparams, SIM_MAX_ARGS);
        return -1;
    }
    for (i = 0; i < f->nparams; i++) {
        const sim_type *t = f->param_types[i];
        const char *pn = f->param_names && f->param_names[i] ? f->param_names[i] : "?";
        if (!t || t->kind == SIM_T_FLOAT) {
            snprintf(err, errlen, "関数 '%s' の引数 '%s' は浮動小数です。浮動小数の引数には対応していません", f->name, pn);
            return -1;
        }
        if (!scalar_kind(t)) {
            snprintf(err, errlen, "関数 '%s' の引数 '%s' は構造体などの値渡しです。整数とポインタの引数にだけ対応しています", f->name, pn);
            return -1;
        }
    }
    if (f->ret && f->ret->kind != SIM_T_VOID && !scalar_kind(f->ret)) {
        snprintf(err, errlen, "関数 '%s' の戻り値は構造体などです。void/整数/ポインタ/float/double にだけ対応しています", f->name);
        return -1;
    }
    return 0;
}

int sim_ctx_set_func(sim_ctx *c, const sim_func *f, char *err, size_t errlen)
{
    /* 検証が済んでから差し替える。失敗したら以前の関数と引数をそのまま残す */
    if (sim_ctx_check_func(f, err, errlen)) return -1;
    c->func = f;
    sim_ctx_clear_args(c);
    return 0;
}

sim_ref *sim_ref_parse_for(sim_ctx *c, const sim_func *f, const char *text, char *err, size_t errlen)
{
    const sim_func *saved = c->func;
    sim_ref *r;
    c->func = f;
    r = sim_ref_parse(c, text, err, errlen);
    c->func = saved;
    return r;
}

int sim_ctx_call(sim_ctx *c, char *err, size_t errlen)
{
    if (!c->func) { snprintf(err, errlen, "対象関数が設定されていません"); return -1; }
    c->ret_raw = 0;
    return c->b->call(c->b, c->func, c->args, c->func->nparams, &c->ret_raw, err, errlen);
}

/* ================================================================ 型 */

const char *sim_type_kind_name(const sim_type *t)
{
    if (!t) return "void";
    switch (t->kind) {
    case SIM_T_INT: return "int";
    case SIM_T_UINT: case SIM_T_BOOL: return "uint";
    case SIM_T_FLOAT: return "float";
    case SIM_T_PTR: return "ptr";
    case SIM_T_ARRAY: return "array";
    case SIM_T_STRUCT: case SIM_T_UNION: return "struct";
    case SIM_T_FUNC: return "func";
    case SIM_T_VOID: return "void";
    default: return "unknown";
    }
}

void sim_type_display(const sim_type *t, char *buf, size_t n)
{
    char in[256];
    if (!t) { snprintf(buf, n, "void"); return; }
    if (t->name && t->name[0]) { snprintf(buf, n, "%s", t->name); return; }
    switch (t->kind) {
    case SIM_T_PTR: sim_type_display(t->target, in, sizeof in); snprintf(buf, n, "%s *", in); break;
    case SIM_T_ARRAY: {
        /* 多次元配列は外側の次元から: uint8_t[2][3] */
        const sim_type *e = t;
        char dims[128];
        size_t k = 0;
        dims[0] = 0;
        while (e && e->kind == SIM_T_ARRAY && (!e->name || !e->name[0] || e == t)) {
            k += (size_t)snprintf(dims + k, k < sizeof dims ? sizeof dims - k : 0, "[%u]", e->count);
            if (k >= sizeof dims) k = sizeof dims - 1;
            e = e->target;
        }
        sim_type_display(e, in, sizeof in);
        snprintf(buf, n, "%s%s", in, dims);
        break;
    }
    case SIM_T_STRUCT: snprintf(buf, n, "struct"); break;
    case SIM_T_UNION: snprintf(buf, n, "union"); break;
    default: snprintf(buf, n, "%s", sim_type_kind_name(t)); break;
    }
}

/* ================================================================ 参照 */

enum { RB_MEM, RB_ARG, RB_RET };
enum { ST_ADD, ST_DEREF };
#define MAX_STEPS 64

struct sim_ref {
    char *text;
    int prefix;
    int base;
    uint64_t addr;
    int argi;
    int nsteps;
    struct { int op; int64_t v; const sim_type *pt; } steps[MAX_STEPS];  /* pt = DEREF するポインタの型 */
    const sim_type *type;     /* 前置を除いた式の型 */
    int32_t bit_off;          /* 最後のアドレスからのビット位置（-1 = ビットフィールドでない） */
    uint32_t bit_size;
    int is_const;
    int multi;
    const sim_type *elem;     /* 複数要素/$ の要素の型 */
    uint32_t count;           /* 複数要素の上限（0 = 不明） */
};

const char *sim_ref_text(const sim_ref *r) { return r->text; }
int sim_ref_prefix(const sim_ref *r) { return r->prefix; }
int sim_ref_is_multi(const sim_ref *r) { return r->multi; }
int sim_ref_is_return(const sim_ref *r) { return r->base == RB_RET && r->nsteps == 0; }
const sim_type *sim_ref_type(const sim_ref *r) { return r->multi ? r->elem : r->type; }

int sim_ref_writable(const sim_ref *r)
{
    if (r->is_const) return 0;
    if (r->base == RB_RET && r->nsteps == 0) return 0;
    if (r->prefix == '$') return 1;
    if (r->multi) return r->elem && scalar_kind(r->elem);
    return scalar_kind(r->type);
}

void sim_ref_free(sim_ref *r)
{
    if (!r) return;
    free(r->text);
    free(r);
}

static void suggest(sim_ctx *c, const char *name, sim_buf *out)
{
    size_t i;
    int k = 0;
    for (i = 0; i < c->nsyms && k < 5; i++) {
        const char *sn = c->syms[i].name;
        const char *bn = strrchr(sn, '@');
        bn = bn ? bn + 1 : sim_basename(sn);
        if (sim_strcasestr_found(sn, name) || (strlen(bn) >= 3 && sim_strcasestr_found(name, bn))) {
            sim_buf_printf(out, "%s%s", k ? ", " : "", sn);
            k++;
        }
    }
}

static const sim_var *find_var(sim_ctx *c, const char *path, const char *func, const char *name,
                               char *err, size_t errlen)
{
    size_t i;
    const sim_var *hit = NULL;
    int n = 0;
    if (c->b->lookup_var) {
        const sim_var *v = c->b->lookup_var(c->b, path && path[0] ? path : NULL, func, name);
        if (v) return v;
    }
    if (func) {
        for (i = 0; i < c->nvars; i++) {
            const sim_var *v = c->vars[i];
            if (!v->func || !streq(v->func, func) || !streq(v->name, name)) continue;
            if (path && path[0] && !sim_path_suffix_match(v->file ? v->file : "", path)) continue;
            n++;
            hit = v;
        }
        if (n == 1) return hit;
        if (n > 1) {
            snprintf(err, errlen, "関数内 static '%s@%s' が複数のファイルにあります。'ファイル名/%s@%s' と書いてください", func, name, func, name);
            return NULL;
        }
        snprintf(err, errlen, "関数 '%s' に static 変数 '%s' が見つかりません", func, name);
        return NULL;
    }
    /* パス指定あり: そのファイルの static かグローバル */
    if (path && path[0]) {
        for (i = 0; i < c->nvars; i++) {
            const sim_var *v = c->vars[i];
            if (v->func || !streq(v->name, name)) continue;
            if (!sim_path_suffix_match(v->file ? v->file : "", path)) continue;
            n++;
            hit = v;
        }
        if (n >= 1) return hit;
        snprintf(err, errlen, "ファイル '%s' に変数 '%s' が見つかりません", path, name);
        return NULL;
    }
    /* グローバル */
    for (i = 0; i < c->nvars; i++) {
        const sim_var *v = c->vars[i];
        if (!v->func && v->is_external && streq(v->name, name)) return v;
    }
    /* 一意なファイル内 static */
    for (i = 0; i < c->nvars; i++) {
        const sim_var *v = c->vars[i];
        if (!v->func && !v->is_external && streq(v->name, name)) { n++; hit = v; }
    }
    if (n == 1) return hit;
    if (n > 1) {
        sim_buf b;
        sim_buf_init(&b);
        for (i = 0; i < c->nvars; i++) {
            const sim_var *v = c->vars[i];
            if (!v->func && !v->is_external && streq(v->name, name))
                sim_buf_printf(&b, " %s/%s", sim_basename(v->file ? v->file : ""), name);
        }
        snprintf(err, errlen, "ファイル内 static '%s' が複数あります。ファイル名を付けてください:%s", name, sim_buf_cstr(&b));
        sim_buf_free(&b);
        return NULL;
    }
    {
        sim_buf b;
        sim_buf_init(&b);
        suggest(c, name, &b);
        if (b.len) snprintf(err, errlen, "変数 '%s' が見つかりません（候補: %s）", name, sim_buf_cstr(&b));
        else snprintf(err, errlen, "変数 '%s' が見つかりません", name);
        sim_buf_free(&b);
    }
    return NULL;
}

static int param_index(const sim_func *f, const char *name)
{
    int i;
    if (!f) return -1;
    for (i = 0; i < f->nparams; i++)
        if (f->param_names && streq(f->param_names[i], name)) return i;
    return -1;
}

/* 匿名メンバを含めてメンバを探す。見つかれば 1 と累積オフセット */
static int find_member(const sim_type *t, const char *name, uint32_t base, const sim_member **out, uint32_t *off)
{
    int i;
    for (i = 0; i < t->nmembers; i++) {
        const sim_member *m = &t->members[i];
        if (m->name && m->name[0] && strcmp(m->name, name) == 0) {
            *out = m;
            *off = base + m->offset;
            return 1;
        }
    }
    for (i = 0; i < t->nmembers; i++) {
        const sim_member *m = &t->members[i];
        if ((!m->name || !m->name[0]) && m->type && (m->type->kind == SIM_T_STRUCT || m->type->kind == SIM_T_UNION))
            if (find_member(m->type, name, base + m->offset, out, off)) return 1;
    }
    return 0;
}

static int add_step_t(sim_ref *r, int op, int64_t v, const sim_type *pt, char *err, size_t errlen)
{
    if (op == ST_ADD && r->nsteps > 0 && r->steps[r->nsteps - 1].op == ST_ADD) {
        r->steps[r->nsteps - 1].v += v;
        return 0;
    }
    if (op == ST_ADD && r->nsteps == 0 && r->base == RB_MEM) { r->addr += (uint64_t)v; return 0; }
    if (r->nsteps >= MAX_STEPS) { snprintf(err, errlen, "式が長すぎます"); return -1; }
    r->steps[r->nsteps].op = op;
    r->steps[r->nsteps].v = v;
    r->steps[r->nsteps].pt = pt;
    r->nsteps++;
    return 0;
}

static int add_step(sim_ref *r, int op, int64_t v, char *err, size_t errlen)
{
    return add_step_t(r, op, v, NULL, err, errlen);
}

/* `:バイト数` で使う大きさだけの符号なし整数型 */
static const sim_type g_sized_types[8] = {
    {SIM_T_UINT, "1 バイト", 1, NULL, 0, NULL, 0}, {SIM_T_UINT, "2 バイト", 2, NULL, 0, NULL, 0},
    {SIM_T_UINT, "3 バイト", 3, NULL, 0, NULL, 0}, {SIM_T_UINT, "4 バイト", 4, NULL, 0, NULL, 0},
    {SIM_T_UINT, "5 バイト", 5, NULL, 0, NULL, 0}, {SIM_T_UINT, "6 バイト", 6, NULL, 0, NULL, 0},
    {SIM_T_UINT, "7 バイト", 7, NULL, 0, NULL, 0}, {SIM_T_UINT, "8 バイト", 8, NULL, 0, NULL, 0},
};

/* 末尾の ":数字" を取り除いてバイト数を返す。なければ 0、範囲外なら -1 */
static int strip_size_suffix(char *buf)
{
    char *colon = strrchr(buf, ':');
    char *q;
    long v;
    if (!colon || colon == buf || !colon[1]) return 0;
    for (q = colon + 1; *q; q++) if (*q < '0' || *q > '9') return 0;
    v = strtol(colon + 1, NULL, 10);
    *colon = 0;
    if (v < 1 || v > 8) return -1;
    return (int)v;
}

/* 現在の位置が引数/戻り値そのものなら、メンバや添字は付けられない */
static int at_slot(const sim_ref *r) { return r->base != RB_MEM && r->nsteps == 0; }

static sim_ref *ref_parse(sim_ctx *c, const char *text, char *err, size_t errlen, int describe)
{
    sim_ref *r = sim_xcalloc(1, sizeof *r);
    char buf[1024], path[512], id1[256], id2[256];
    const char *p, *rest;
    size_t n;
    int has_at = 0, ret = 0, size_override = 0;

    /* 前後の空白を除く */
    while (*text == ' ' || *text == '\t') text++;
    n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
    if (n == 0) { snprintf(err, errlen, "変数名が空です"); free(r); return NULL; }
    if (n >= sizeof buf) { snprintf(err, errlen, "変数名が長すぎます"); free(r); return NULL; }
    memcpy(buf, text, n);
    buf[n] = 0;
    r->text = sim_xstrdup(buf);
    r->bit_off = -1;

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); sim_ref_free(r); return NULL; } while (0)

    size_override = strip_size_suffix(buf);
    if (size_override < 0) FAIL("'%s': ':' の後のバイト数は 1〜8 にしてください", r->text);

    p = buf;
    if (*p == '$' || *p == '&' || *p == '*') { r->prefix = *p; p++; }
    while (*p == ' ') p++;

    /* アドレス直接指定（0xFFF10、0xFFF10:2）。SFR などデバッグ情報にない場所用 */
    if (*p >= '0' && *p <= '9') {
        const char *end = p;
        uint64_t a = 0;
        int rc;
        while (isalnum((unsigned char)*end)) end++;
        rc = sim_parse_uint_n(p, (size_t)(end - p), UINT64_MAX, &a);
        while (*end == ' ') end++;
        if (rc == SIM_NUM_OCTAL) FAIL("'%s': アドレスが 0 で始まる 10 進です（8 進数には対応していません。0x で書いてください）", r->text);
        if (rc == SIM_NUM_RANGE) FAIL("'%s': アドレスが 64 ビットの範囲を超えています", r->text);
        if (rc || *end) FAIL("'%s': アドレスとして読めません（例: 0xFFF10、0xFFF10:2）", r->text);
        if (r->prefix == '$' || r->prefix == '*') FAIL("'%s': アドレス直接指定には '%c' を付けられません", r->text, r->prefix);
        r->base = RB_MEM;
        r->addr = a;
        r->type = &g_sized_types[(size_override ? size_override : 1) - 1];
        if (r->prefix == '&') { r->multi = 1; r->elem = r->type; r->count = 1; }
        return r;
    }

    /* パス: 最後の '/' まで */
    split_path(p, path, sizeof path, &rest);
    p = rest;

    /* 識別子 [@ 識別子 | @@] */
    id1[0] = id2[0] = 0;
    {
        size_t k = 0;
        while (sim_is_ident_char((unsigned char)*p) && k < sizeof id1 - 1) id1[k++] = *p++;
        id1[k] = 0;
        if (*p == '@') {
            has_at = 1;
            p++;
            if (*p == '@') { ret = 1; p++; }
            else {
                k = 0;
                while (sim_is_ident_char((unsigned char)*p) && k < sizeof id2 - 1) id2[k++] = *p++;
                id2[k] = 0;
                if (!id2[0]) FAIL("'%s': '@' の後に名前がありません", buf);
            }
        }
    }
    if (!has_at && !id1[0]) FAIL("'%s': 変数名として読めません", buf);
    if (id1[0] && !sim_is_ident_start((unsigned char)id1[0])) FAIL("'%s': 名前が数字で始まっています", buf);

    if (ret) {
        const sim_func *f = c->func;
        if (!f) FAIL("'%s': 対象関数が決まっていないので戻り値は使えません", buf);
        if (id1[0] && !streq(id1, f->name)) FAIL("'%s': 戻り値を参照できるのは対象関数 '%s' だけです", buf, f->name);
        if (!f->ret || f->ret->kind == SIM_T_VOID) FAIL("'%s': 関数 '%s' の戻り値は void です", buf, f->name);
        r->base = RB_RET;
        r->type = f->ret;
    } else if (has_at && !id1[0]) {
        int pi = param_index(c->func, id2);
        if (!c->func) FAIL("'%s': 対象関数が決まっていないので引数は使えません", buf);
        if (pi < 0) {
            sim_buf b;
            int i;
            sim_buf_init(&b);
            for (i = 0; i < c->func->nparams; i++)
                sim_buf_printf(&b, "%s@%s", i ? ", " : "", c->func->param_names[i] ? c->func->param_names[i] : "?");
            snprintf(err, errlen, "'%s': 関数 '%s' に引数 '%s' はありません（引数: %s）", buf, c->func->name, id2,
                     b.len ? sim_buf_cstr(&b) : "なし");
            sim_buf_free(&b);
            sim_ref_free(r);
            return NULL;
        }
        r->base = RB_ARG;
        r->argi = pi;
        r->type = c->func->param_types[pi];
    } else if (has_at) {
        int pi = -1;
        if (c->func && streq(c->func->name, id1) &&
            (!path[0] || sim_path_suffix_match(c->func->file ? c->func->file : "", path)))
            pi = param_index(c->func, id2);
        if (pi >= 0) {
            r->base = RB_ARG;
            r->argi = pi;
            r->type = c->func->param_types[pi];
        } else {
            char e2[512];
            const sim_var *v = find_var(c, path, id1, id2, e2, sizeof e2);
            if (!v) FAIL("'%s': %s", buf, e2);
            r->base = RB_MEM;
            r->addr = v->addr;
            r->type = v->type;
            r->is_const = v->is_const;
        }
    } else {
        char e2[512];
        const sim_var *v = find_var(c, path, NULL, id1, e2, sizeof e2);
        if (!v) FAIL("'%s': %s", buf, e2);
        r->base = RB_MEM;
        r->addr = v->addr;
        r->type = v->type;
        r->is_const = v->is_const;
    }

    /* 後置 */
    if (r->base == RB_MEM) {
        size_t i;
        for (i = 0; i < c->ndefined; i++) {
            const defined_var *d = c->defined[i];
            if (d->var.addr == r->addr && d->var.type == r->type && !strcmp(d->var.name, id1)) {
                r->bit_off = d->bit_offset; r->bit_size = d->bit_size;
            }
        }
    }
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (r->bit_off >= 0) FAIL("'%s': ビットフィールドの後にメンバや添字は付けられません", buf);
        if (*p == '.' || (p[0] == '-' && p[1] == '>')) {
            int arrow = *p == '-';
            char mn[256];
            size_t k = 0;
            const sim_member *m;
            uint32_t off;
            const sim_type *st = r->type;
            p += arrow ? 2 : 1;
            while (sim_is_ident_char((unsigned char)*p) && k < sizeof mn - 1) mn[k++] = *p++;
            mn[k] = 0;
            if (!mn[0]) FAIL("'%s': '%s' の後にメンバ名がありません", buf, arrow ? "->" : ".");
            if (arrow) {
                if (!st || st->kind != SIM_T_PTR) FAIL("'%s': '->' の左がポインタではありません", buf);
                if (add_step_t(r, ST_DEREF, 0, st, err, errlen)) { sim_ref_free(r); return NULL; }
                st = st->target;
            } else if (at_slot(r)) {
                FAIL("'%s': 構造体の値渡しの引数/戻り値のメンバには対応していません", buf);
            }
            if (!st || (st->kind != SIM_T_STRUCT && st->kind != SIM_T_UNION))
                FAIL("'%s': メンバ '%s' の左が構造体/共用体ではありません", buf, mn);
            if (!find_member(st, mn, 0, &m, &off)) {
                sim_buf b;
                int i;
                sim_buf_init(&b);
                for (i = 0; i < st->nmembers; i++)
                    if (st->members[i].name && st->members[i].name[0]) sim_buf_printf(&b, "%s%s", b.len ? ", " : "", st->members[i].name);
                snprintf(err, errlen, "'%s': メンバ '%s' がありません（メンバ: %s）", buf, mn, sim_buf_cstr(&b));
                sim_buf_free(&b);
                sim_ref_free(r);
                return NULL;
            }
            if (m->bit_offset >= 0) {
                /* off はビットフィールドを含む構造体内の位置。ビット位置から計算し直す */
                uint32_t sbase = off - m->offset;
                uint32_t bits = (uint32_t)m->bit_offset;
                if (add_step(r, ST_ADD, (int64_t)(sbase + bits / 8), err, errlen)) { sim_ref_free(r); return NULL; }
                r->bit_off = (int32_t)(bits % 8);
                r->bit_size = m->bit_size;
            } else {
                if (add_step(r, ST_ADD, (int64_t)off, err, errlen)) { sim_ref_free(r); return NULL; }
            }
            r->type = m->type;
        } else if (*p == '[') {
            const char *end;
            uint64_t idx = 0;
            int rc;
            const sim_type *t = r->type;
            p++;
            while (*p == ' ') p++;
            if (*p == '-') FAIL("'%s': 添字が負です", buf);
            if (*p == '+') p++;
            /* 10 進か 0x 16 進。添字全体を読み、64 ビットに収まらない値や 8 進と紛れる 010 を別の添字にしない */
            for (end = p; isalnum((unsigned char)*end); end++) {}
            if (end == p) FAIL("'%s': 添字が数値ではありません", buf);
            rc = sim_parse_uint_n(p, (size_t)(end - p), (uint64_t)INT64_MAX, &idx);
            if (rc == SIM_NUM_OCTAL)
                FAIL("'%s': 添字 '%.*s' が 0 で始まる 10 進です（8 進数には対応していません。10 進か 0x で書いてください）", buf, (int)(end - p), p);
            if (rc == SIM_NUM_RANGE) FAIL("'%s': 添字 '%.*s' が大きすぎます", buf, (int)(end - p), p);
            if (rc) FAIL("'%s': 添字 '%.*s' が数値ではありません", buf, (int)(end - p), p);
            p = end;
            while (*p == ' ') p++;
            if (*p != ']') FAIL("'%s': ']' がありません", buf);
            p++;
            if (t && t->kind == SIM_T_ARRAY) {
                if (at_slot(r)) FAIL("'%s': 配列の値渡しには対応していません", buf);
                if (t->count > 0 && idx >= t->count)
                    FAIL("'%s': 添字 %llu は配列の要素数 %u の範囲外です", buf, (unsigned long long)idx, t->count);
                if (!t->target) FAIL("'%s': 配列の要素の型が分かりません", buf);
                if (t->target->size && idx > (uint64_t)INT64_MAX / t->target->size) FAIL("'%s': 添字 %llu が大きすぎます", buf, (unsigned long long)idx);
                if (add_step(r, ST_ADD, (int64_t)(idx * t->target->size), err, errlen)) { sim_ref_free(r); return NULL; }
                r->type = t->target;
            } else if (t && t->kind == SIM_T_PTR) {
                if (!t->target || t->target->kind == SIM_T_VOID || t->target->size == 0)
                    FAIL("'%s': void ポインタには添字を付けられません", buf);
                if (idx > (uint64_t)INT64_MAX / t->target->size) FAIL("'%s': 添字 %llu が大きすぎます", buf, (unsigned long long)idx);
                if (add_step_t(r, ST_DEREF, 0, t, err, errlen)) { sim_ref_free(r); return NULL; }
                if (idx && add_step(r, ST_ADD, (int64_t)(idx * t->target->size), err, errlen)) { sim_ref_free(r); return NULL; }
                r->type = t->target;
            } else {
                FAIL("'%s': 添字の左が配列でもポインタでもありません", buf);
            }
        } else {
            FAIL("'%s': '%s' の部分が読めません", buf, p);
        }
    }

    /* ":バイト数" で型を上書きする（型の分からないシンボルや、型と違う幅で読みたいとき） */
    if (size_override) {
        if (r->base != RB_MEM && r->nsteps == 0) FAIL("'%s': 引数/戻り値に ':バイト数' は付けられません", r->text);
        if (r->bit_off >= 0) FAIL("'%s': ビットフィールドに ':バイト数' は付けられません", r->text);
        if (r->prefix == '$' || r->prefix == '*') FAIL("'%s': '%c' と ':バイト数' は一緒に使えません", r->text, r->prefix);
        r->type = &g_sized_types[size_override - 1];
    }

    /* 前置と複数要素 */
    switch (r->prefix) {
    case '$':
        if (!r->type || r->type->kind != SIM_T_PTR) FAIL("'%s': '$' を付けられるのはポインタだけです", buf);
        r->elem = r->type->target;
        break;
    case '&':
        if (r->bit_off >= 0) FAIL("'%s': ビットフィールドに '&' は付けられません", buf);
        if (r->base != RB_MEM && r->nsteps == 0) FAIL("'%s': 引数/戻り値に '&' は付けられません", buf);
        r->multi = 1;
        if (r->type && r->type->kind == SIM_T_ARRAY) { r->elem = r->type->target; r->count = r->type->count; }
        else { r->elem = r->type; r->count = 1; }
        break;
    case '*':
        if (!r->type || r->type->kind != SIM_T_PTR) FAIL("'%s': '*' を付けられるのはポインタだけです", buf);
        if (!r->type->target || r->type->target->size == 0) FAIL("'%s': void ポインタの先は読めません", buf);
        if (add_step_t(r, ST_DEREF, 0, r->type, err, errlen)) { sim_ref_free(r); return NULL; }
        r->multi = 1;
        r->elem = r->type->target;
        r->count = 0;
        break;
    default:
        if (r->type && r->type->kind == SIM_T_ARRAY) {
            r->multi = 1;
            r->elem = r->type->target;
            r->count = r->type->count;
        } else if (!describe && r->type && (r->type->kind == SIM_T_STRUCT || r->type->kind == SIM_T_UNION)) {
            FAIL("'%s': 構造体/共用体です。'%s.メンバ' のようにメンバを指定してください", buf, buf);
        } else if (!describe && r->type && r->type->kind == SIM_T_UNKNOWN) {
            FAIL("'%s': 型が分かりません（デバッグ情報がありません）。'%s:2' のようにバイト数を付けてください", buf, buf);
        } else if (!describe && (!r->type || r->type->kind == SIM_T_VOID || r->type->kind == SIM_T_FUNC)) {
            FAIL("'%s': 値として読めない型です", buf);
        }
    }
    if (r->multi && !describe) {
        if (!r->elem) FAIL("'%s': 要素の型が分かりません", buf);
        if (r->elem->kind == SIM_T_ARRAY) {
            /* 多次元配列: 最も内側の要素まで平らにする */
            const sim_type *e = r->elem;
            uint32_t cnt = r->count;
            while (e && e->kind == SIM_T_ARRAY) { cnt = cnt * (e->count ? e->count : 1); e = e->target; }
            r->elem = e;
            r->count = cnt;
        }
        if (!scalar_kind(r->elem)) FAIL("'%s': 要素が構造体などです。要素を 1 つずつ指定してください（例: %s[0].メンバ）", buf, buf);
    }
#undef FAIL
    return r;
}

/* GUI の型照会は集約型も受け付ける。値の読み書きには従来の検証を使う。 */
sim_ref *sim_ref_parse_symbol(sim_ctx *c, const char *text, char *err, size_t errlen)
{
    sim_ref *r = ref_parse(c, text, err, errlen, 1);
    if (r) { r->multi = 0; } /* sim_ref_type は配列そのものの型を返す */
    return r;
}

sim_ref *sim_ref_parse(sim_ctx *c, const char *text, char *err, size_t errlen)
{
    return ref_parse(c, text, err, errlen, 0);
}

/* ================================================================ バイト列と値 */

static uint64_t load_uint(const uint8_t *p, size_t n, int be)
{
    uint64_t v = 0;
    size_t i;
    if (be) for (i = 0; i < n; i++) v = (v << 8) | p[i];
    else for (i = n; i > 0; i--) v = (v << 8) | p[i - 1];
    return v;
}

static void store_uint(uint8_t *p, size_t n, uint64_t v, int be)
{
    size_t i;
    if (be) for (i = n; i > 0; i--) { p[i - 1] = (uint8_t)v; v >>= 8; }
    else for (i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t mask_bits(uint64_t v, unsigned bits) { return bits >= 64 ? v : (v & ((1ULL << bits) - 1)); }

static int64_t sign_extend(uint64_t v, unsigned bits)
{
    if (bits == 0 || bits >= 64) return (int64_t)v;
    v = mask_bits(v, bits);
    if (v & (1ULL << (bits - 1))) return (int64_t)(v | ~((1ULL << bits) - 1));
    return (int64_t)v;
}

typedef struct { int kind; uint64_t addr; } loc_t;

static int eval_loc(sim_ctx *c, const sim_ref *r, int nsteps, loc_t *L, char *err, size_t errlen)
{
    int i;
    sim_backend *b = c->b;
    L->kind = r->base;
    L->addr = r->addr;
    for (i = 0; i < nsteps; i++) {
        if (r->steps[i].op == ST_DEREF) {
            uint64_t pv;
            const sim_type *pt = r->steps[i].pt;
            size_t psz = pt && pt->size > 0 && pt->size <= 8 ? pt->size : (size_t)b->ptr_size;
            if (L->kind == RB_ARG) pv = mask_bits((uint64_t)c->args[r->argi], (unsigned)psz * 8);
            else if (L->kind == RB_RET) pv = mask_bits(c->ret_raw, (unsigned)psz * 8);
            else {
                uint8_t tmp[8];
                if (b->read(b, L->addr, tmp, psz, err, errlen)) return -1;
                pv = load_uint(tmp, psz, b->big_endian);
            }
            if (pv == 0) { snprintf(err, errlen, "'%s': NULL ポインタの先は読み書きできません", r->text); return -1; }
            L->kind = RB_MEM;
            L->addr = b->ptr_to_addr ? b->ptr_to_addr(b, pt, pv) : pv;
        } else {
            L->addr += (uint64_t)r->steps[i].v;
        }
    }
    return 0;
}

static unsigned type_bits(const sim_type *t) { return t->size >= 8 ? 64 : t->size * 8; }

/* 生のビット列を読む（bit_size > 0 ならビットフィールド） */
static int read_raw(sim_ctx *c, const loc_t *L, const sim_type *t, int32_t bit_off, uint32_t bit_size,
                    uint64_t *raw, char *err, size_t errlen)
{
    sim_backend *b = c->b;
    if (L->kind == RB_ARG) { snprintf(err, errlen, "内部エラー: 引数を read_raw で読もうとしました"); return -1; }
    if (L->kind == RB_RET) { *raw = mask_bits(c->ret_raw, type_bits(t)); return 0; }
    if (bit_off >= 0) {
        uint8_t tmp[9];
        size_t nb = ((size_t)bit_off + bit_size + 7) / 8;
        uint64_t v;
        if (nb > 8) { snprintf(err, errlen, "ビットフィールドが大きすぎます"); return -1; }
        if (b->read(b, L->addr, tmp, nb, err, errlen)) return -1;
        v = load_uint(tmp, nb, b->big_endian);
        if (b->big_endian) v >>= (nb * 8 - (size_t)bit_off - bit_size);
        else v >>= bit_off;
        *raw = mask_bits(v, bit_size);
        return 0;
    }
    {
        uint8_t tmp[8];
        size_t n = t->size > 8 ? 8 : t->size;
        if (n == 0) { snprintf(err, errlen, "大きさ 0 の型は読めません"); return -1; }
        if (b->read(b, L->addr, tmp, n, err, errlen)) return -1;
        *raw = load_uint(tmp, n, b->big_endian);
        return 0;
    }
}

static int write_raw(sim_ctx *c, const sim_ref *r, const loc_t *L, const sim_type *t, int32_t bit_off,
                     uint32_t bit_size, uint64_t raw, char *err, size_t errlen)
{
    sim_backend *b = c->b;
    if (L->kind == RB_ARG) {
        unsigned bits = type_bits(t);
        c->args[r->argi] = t->kind == SIM_T_INT ? sign_extend(raw, bits) : (int64_t)mask_bits(raw, bits);
        return 0;
    }
    if (L->kind == RB_RET) { snprintf(err, errlen, "'%s': 戻り値には書き込めません", r->text); return -1; }
    if (bit_off >= 0) {
        uint8_t tmp[9];
        size_t nb = ((size_t)bit_off + bit_size + 7) / 8;
        uint64_t v, m;
        unsigned sh;
        if (nb > 8) { snprintf(err, errlen, "ビットフィールドが大きすぎます"); return -1; }
        if (b->read(b, L->addr, tmp, nb, err, errlen)) return -1;
        v = load_uint(tmp, nb, b->big_endian);
        sh = b->big_endian ? (unsigned)(nb * 8 - (size_t)bit_off - bit_size) : (unsigned)bit_off;
        m = mask_bits(~0ULL, bit_size) << sh;
        v = (v & ~m) | ((mask_bits(raw, bit_size) << sh) & m);
        store_uint(tmp, nb, v, b->big_endian);
        return b->write(b, L->addr, tmp, nb, err, errlen);
    }
    {
        uint8_t tmp[8];
        size_t n = t->size > 8 ? 8 : t->size;
        store_uint(tmp, n, raw, b->big_endian);
        return b->write(b, L->addr, tmp, n, err, errlen);
    }
}

/* 値 -> 生のビット列 */
/* 整数の項目を double にする（16 進と INT64_MAX を超える値は符号なし） */
static double item_double(const sim_item *it)
{
    if (it->kind == SIM_ITEM_FLOAT) return it->f;
    return (it->hex_digits || it->u64) ? (double)(uint64_t)it->i : (double)it->i;
}

double sim_item_double(const sim_item *it)
{
    if (it->too_big) return NAN;
    return item_double(it);
}

static int encode(const sim_type *t, uint32_t bit_size, const sim_item *it, const char *name, uint64_t *raw,
                  char *err, size_t errlen)
{
    unsigned bits = bit_size ? bit_size : type_bits(t);
    if (it->too_big) {
        if (it->kind == SIM_ITEM_FLOAT) snprintf(err, errlen, "'%s' に書く値が double の範囲を超えています（あふれるか、0 になるほど小さい）", name);
        else snprintf(err, errlen, "'%s' に書く値が 64 ビットの整数の範囲を超えています", name);
        return -1;
    }
    if (t->kind == SIM_T_FLOAT) {
        double d = item_double(it);
        if (t->size == 4) {
            float f = (float)d;
            uint32_t u;
            if (isinf(f)) {
                snprintf(err, errlen, "'%s' は float です。値 %g は収まりません", name, d);
                return -1;
            }
            memcpy(&u, &f, 4);
            *raw = u;
        } else {
            uint64_t u;
            memcpy(&u, &d, 8);
            *raw = u;
        }
        return 0;
    }
    {
        int64_t v;
        if (it->kind == SIM_ITEM_FLOAT) {
            if (it->f != floor(it->f) || fabs(it->f) > 9.2e18) {
                snprintf(err, errlen, "'%s' は整数の変数です。小数 %g は書き込めません", name, it->f);
                return -1;
            }
            v = (int64_t)it->f;
        } else {
            v = it->i;
        }
        if (bits < 64 && !it->hex_digits && !it->u64) {
            int64_t lo = -(int64_t)(1ULL << (bits - 1));
            uint64_t hi = (1ULL << bits) - 1;
            if (v < lo || (v > 0 && (uint64_t)v > hi)) {
                snprintf(err, errlen, "'%s' は %u ビットです。値 %lld は収まりません", name, bits, (long long)v);
                return -1;
            }
        } else if (bits < 64) {
            /* 16 進と、INT64_MAX を超える 10 進・2 進は 64 ビットの符号なしとして比べる */
            if ((uint64_t)v >> bits) {
                if (it->hex_digits) snprintf(err, errlen, "'%s' は %u ビットです。値 0x%llX は収まりません", name, bits, (unsigned long long)v);
                else snprintf(err, errlen, "'%s' は %u ビットです。値 %llu は収まりません", name, bits, (unsigned long long)v);
                return -1;
            }
        }
        *raw = mask_bits((uint64_t)v, bits);
        if (t->kind == SIM_T_BOOL) *raw = *raw ? 1 : 0;
        return 0;
    }
}

typedef struct { int is_float; int is_signed; int64_t i; uint64_t u; double f; } num_t;

static num_t decode(const sim_type *t, uint32_t bit_size, uint64_t raw)
{
    num_t n;
    unsigned bits = bit_size ? bit_size : type_bits(t);
    memset(&n, 0, sizeof n);
    if (t->kind == SIM_T_FLOAT) {
        n.is_float = 1;
        if (t->size == 4) { uint32_t u = (uint32_t)raw; float f; memcpy(&f, &u, 4); n.f = f; }
        else { double d; memcpy(&d, &raw, 8); n.f = d; }
        return n;
    }
    n.u = mask_bits(raw, bits);
    if (t->kind == SIM_T_INT) { n.is_signed = 1; n.i = sign_extend(raw, bits); n.f = (double)n.i; }
    else { n.i = (int64_t)n.u; n.f = (double)n.u; }
    return n;
}

/* 期待値と比べる。整数の型では期待値を入力と同じ規則（encode）でビット列に直してから比べるので、
 * 負の値を符号なしの型に書いた場合、16 進表記、整数値の小数などの解釈が入力側と食い違わない。
 * 入力なら「収まりません」のエラーになる値（ビット幅を超える値、整数でない小数）は、
 * 下位ビットだけで比べずに不一致にする（エラーにして他の行を止めないため） */
static int item_match(const sim_type *t, uint32_t bit_size, uint64_t raw, const sim_item *it)
{
    num_t a = decode(t, bit_size, raw);
    if (a.is_float) {
        double e = item_double(it);
        if (it->too_big) return 0;
        if (t->size == 4) return !isinf((float)e) && (float)e == (float)a.f;
        return e == a.f;
    }
    {
        uint64_t e;
        char err[256];
        if (encode(t, bit_size, it, "", &e, err, sizeof err)) return 0;
        if (t->kind == SIM_T_BOOL) return (a.u != 0) == (e != 0);
        return e == a.u;
    }
}

static void format_num(const sim_type *t, uint32_t bit_size, uint64_t raw, const sim_fmt *fmt, sim_buf *out)
{
    num_t a = decode(t, bit_size, raw);
    char tmp[64];
    if (a.is_float) {
        sim_format_double(tmp, sizeof tmp, a.f, t->size == 4);
        sim_buf_adds(out, tmp);
        return;
    }
    if (fmt && fmt->hex_digits > 0) {
        sim_buf_printf(out, fmt->prefix_upper ? "0X" : "0x");
        sim_buf_printf(out, fmt->upper ? "%0*llX" : "%0*llx", fmt->hex_digits, (unsigned long long)a.u);
        return;
    }
    if (a.is_signed) sim_buf_printf(out, "%lld", (long long)a.i);
    else sim_buf_printf(out, "%llu", (unsigned long long)a.u);
}

/* ================================================================ セルの値 */

int sim_cellval_present(const sim_cellval *v)
{
    return v && (v->n > 0 || v->is_string);
}

void sim_cellval_free(sim_cellval *v)
{
    free(v->items);
    v->items = NULL;
    v->n = 0;
    v->is_string = 0;
}

static void push_item(sim_cellval *v, const sim_item *it)
{
    v->items = sim_xrealloc(v->items, (size_t)(v->n + 1) * sizeof *v->items);
    v->items[v->n++] = *it;
}

static int parse_number(const char *s, size_t n, sim_item *it, char *err, size_t errlen)
{
    char tmp[128];
    const char *p;
    int neg = 0;
    memset(it, 0, sizeof *it);
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n == 0) { snprintf(err, errlen, "'|' の間が空です"); return -1; }
    if (n >= sizeof tmp) { snprintf(err, errlen, "値が長すぎます"); return -1; }
    memcpy(tmp, s, n);
    tmp[n] = 0;
    p = tmp;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    it->kind = SIM_ITEM_INT;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *h = p + 2;
        uint64_t v = 0;
        int digits = 0, upper = 0, lower = 0;
        for (; *h; h++) {
            int d;
            if (*h >= '0' && *h <= '9') d = *h - '0';
            else if (*h >= 'a' && *h <= 'f') { d = *h - 'a' + 10; lower = 1; }
            else if (*h >= 'A' && *h <= 'F') { d = *h - 'A' + 10; upper = 1; }
            else break;
            if (digits >= 16) { snprintf(err, errlen, "16 進数 '%s' が長すぎます", tmp); return -1; }
            v = v * 16 + (uint64_t)d;
            digits++;
        }
        while (*h == 'u' || *h == 'U' || *h == 'l' || *h == 'L') h++;
        if (digits == 0 || *h) { snprintf(err, errlen, "値 '%s' を 16 進数として読めません", tmp); return -1; }
        if (neg) {
            /* -0x8000000000000000 までが 64 ビットの符号付き。それより小さい値を折り返して別の値にしない */
            if (v > (1ULL << 63)) it->too_big = 1;
            else it->i = (int64_t)(0 - v);
            return 0;
        }
        it->i = (int64_t)v;
        it->hex_digits = digits;
        it->hex_upper = upper || !lower;
        it->hex_prefix_upper = p[1] == 'X';
        return 0;
    }
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
        const char *h = p + 2;
        uint64_t v = 0;
        int digits = 0;
        for (; *h == '0' || *h == '1'; h++, digits++) v = v * 2 + (uint64_t)(*h - '0');
        if (*h || digits > 64) { snprintf(err, errlen, "値 '%s' を 2 進数として読めません", tmp); return -1; }
        if (neg) {
            if (v > (1ULL << 63)) it->too_big = 1;
            else it->i = (int64_t)(0 - v);
        } else {
            it->i = (int64_t)v;
            it->u64 = v > (uint64_t)INT64_MAX;
        }
        return 0;
    }
    {
        char *end;
        int isf = 0;
        const char *q;
        for (q = p; *q; q++) if (*q == '.' || *q == 'e' || *q == 'E') isf = 1;
        if (!isdigit((unsigned char)*p) && *p != '.') { snprintf(err, errlen, "値 '%s' を数値として読めません", tmp); return -1; }
        if (isf) {
            double d = 0;
            int rc = sim_parse_double(tmp, &d);
            if (rc == SIM_NUM_BAD) { snprintf(err, errlen, "値 '%s' を数値として読めません", tmp); return -1; }
            it->kind = SIM_ITEM_FLOAT;
            if (rc == SIM_NUM_RANGE) it->too_big = 1;   /* double のあふれ（1e400）や 0 になるほど小さい値 */
            else it->f = d;
            return 0;
        } else {
            unsigned long long u;
            errno = 0;
            u = strtoull(p, &end, 10);
            while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') end++;
            if (*end) { snprintf(err, errlen, "値 '%s' を数値として読めません", tmp); return -1; }
            /* 64 ビットを超える値は strtoull が飽和させる（ERANGE）。その値（-1 と同じビット列）にしない */
            if (errno == ERANGE || (neg && u > (1ULL << 63))) { it->too_big = 1; return 0; }
            if (neg) it->i = (int64_t)(0 - u);
            else {
                it->i = (int64_t)u;
                it->u64 = u > (unsigned long long)INT64_MAX;
            }
            return 0;
        }
    }
}

int sim_parse_cell(const char *text, size_t len, sim_cellval *out, char *err, size_t errlen)
{
    const char *s = text, *e = text + len;
    memset(out, 0, sizeof *out);
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    if (s == e) return 0;
    if (*s == '\'') {
        const char *q = e - 1;
        const char *p;
        if (e - s < 2 || *q != '\'') { snprintf(err, errlen, "文字列 %.*s の閉じる ' がありません", (int)(e - s), s); return -1; }
        out->is_string = 1;
        for (p = s + 1; p < q; p++) {
            sim_item it;
            memset(&it, 0, sizeof it);
            it.kind = SIM_ITEM_INT;
            it.i = (unsigned char)*p;
            push_item(out, &it);
        }
        return 0;
    }
    {
        const char *p = s;
        while (p <= e) {
            const char *bar = p;
            sim_item it;
            while (bar < e && *bar != '|') bar++;
            if (parse_number(p, (size_t)(bar - p), &it, err, errlen)) { sim_cellval_free(out); return -1; }
            push_item(out, &it);
            if (bar >= e) break;
            p = bar + 1;
        }
    }
    return 0;
}

/* ================================================================ 書き込みと比較 */

static const sim_type g_byte_type = {SIM_T_UINT, "unsigned char", 1, NULL, 0, NULL, 0};

/* 値が書き込める形か（const・戻り値・'$' の値・要素数・型に収まるか）をメモリに触れずに確かめる。
 * ポインタの先（NULL など）のようにその時のメモリで決まるものは確かめない */
int sim_ref_check_value(const sim_ref *r, const sim_cellval *v, char *err, size_t errlen)
{
    int i;
    if (!sim_cellval_present(v)) return 0;
    if (r->is_const) { snprintf(err, errlen, "'%s' は const なので書き込めません", r->text); return -1; }
    if (r->base == RB_RET && r->nsteps == 0) { snprintf(err, errlen, "'%s': 戻り値は入力にできません", r->text); return -1; }
    if (r->prefix == '$') {
        const sim_type *e = r->elem && r->elem->size ? r->elem : &g_byte_type;
        if (v->n != 1 || v->is_string || v->items[0].kind != SIM_ITEM_INT || v->items[0].too_big || v->items[0].u64 ||
            v->items[0].i < 0) {
            snprintf(err, errlen, "'%s': '$' の値は確保する要素数（0 以上の整数）です", r->text);
            return -1;
        }
        /* 要素数 × 要素サイズは 64 ビットで求め、size_t に収まらなければ（32 ビット環境）エラーにする */
        if (v->items[0].i > 0x1000000 || (uint64_t)v->items[0].i * e->size > (uint64_t)SIZE_MAX) {
            snprintf(err, errlen, "'%s': 要素数が大きすぎます", r->text);
            return -1;
        }
        return 0;
    }
    if (r->multi) {
        if (r->count > 0 && (uint32_t)v->n > r->count) {
            snprintf(err, errlen, "'%s': 値が %d 個ありますが、要素数は %u です", r->text, v->n, r->count);
            return -1;
        }
        for (i = 0; i < v->n; i++) {
            uint64_t raw;
            if (encode(r->elem, 0, &v->items[i], r->text, &raw, err, errlen)) return -1;
        }
        return 0;
    }
    if (v->n != 1) {
        if (v->is_string) snprintf(err, errlen, "'%s' は 1 つの値の変数です。文字列は '&配列' や '*ポインタ' に書いてください", r->text);
        else snprintf(err, errlen, "'%s' は 1 つの値の変数です。'|' で複数の値は書けません", r->text);
        return -1;
    }
    {
        uint64_t raw;
        return encode(r->type, r->bit_off >= 0 ? r->bit_size : 0, &v->items[0], r->text, &raw, err, errlen);
    }
}

int sim_ref_write(sim_ctx *c, const sim_ref *r, const sim_cellval *v, char *err, size_t errlen)
{
    loc_t L;
    int i;
    /* 空セル（n = 0 で文字列でもない）は「設定しない」。空文字列 '' は終端の 0 を書く */
    if (!sim_cellval_present(v)) return 0;
    if (sim_ref_check_value(r, v, err, errlen)) return -1;
    if (eval_loc(c, r, r->nsteps, &L, err, errlen)) return -1;
    if (r->prefix == '$') {
        const sim_type *e = r->elem && r->elem->size ? r->elem : &g_byte_type;
        uint64_t addr = 0;
        if (v->items[0].i > 0 && c->b->alloc(c->b, (size_t)((uint64_t)v->items[0].i * e->size), &addr, err, errlen)) return -1;
        return write_raw(c, r, &L, r->type, -1, 0, addr, err, errlen);
    }
    if (r->multi) {
        const sim_type *e = r->elem;
        int n = v->n;
        if (L.kind != RB_MEM) { snprintf(err, errlen, "'%s': 書き込み先がメモリではありません", r->text); return -1; }
        for (i = 0; i < n; i++) {
            uint64_t raw;
            loc_t Li = L;
            Li.addr = L.addr + (uint64_t)i * e->size;
            if (encode(e, 0, &v->items[i], r->text, &raw, err, errlen)) return -1;
            if (write_raw(c, r, &Li, e, -1, 0, raw, err, errlen)) return -1;
        }
        /* 文字列は入る余地があれば終端の 0 も書く */
        if (v->is_string && (r->count == 0 || (uint32_t)n < r->count)) {
            loc_t Li = L;
            Li.addr = L.addr + (uint64_t)n * e->size;
            if (write_raw(c, r, &Li, e, -1, 0, 0, err, errlen)) return -1;
        }
        return 0;
    }
    {
        uint64_t raw;
        if (encode(r->type, r->bit_off >= 0 ? r->bit_size : 0, &v->items[0], r->text, &raw, err, errlen)) return -1;
        return write_raw(c, r, &L, r->type, r->bit_off, r->bit_size, raw, err, errlen);
    }
}

static int printable_str_byte(uint64_t ch) { return ch >= 0x20 && ch != '\'' && ch != ',' && ch != '"' && ch != '|' && ch < 0x100 && ch != 0x7f; }

int sim_ref_check(sim_ctx *c, const sim_ref *r, const sim_cellval *expected, const sim_fmt *fmt,
                  sim_buf *out, int *match, char *err, size_t errlen)
{
    loc_t L;
    int has_exp = sim_cellval_present(expected);
    *match = -1;
    if (r->prefix == '$') { snprintf(err, errlen, "'%s': '$' は入力にだけ使えます", r->text); return -1; }
    if (eval_loc(c, r, r->nsteps, &L, err, errlen)) return -1;
    if (L.kind == RB_ARG) {
        /* 引数そのもの */
        num_t dummy;
        uint64_t raw = mask_bits((uint64_t)c->args[r->argi], type_bits(r->type));
        (void)dummy;
        if (has_exp) {
            if (expected->n != 1) { snprintf(err, errlen, "'%s' は 1 つの値の変数です", r->text); return -1; }
            *match = item_match(r->type, 0, raw, &expected->items[0]);
        }
        format_num(r->type, 0, raw, fmt, out);
        return 0;
    }
    if (!r->multi) {
        uint64_t raw;
        uint32_t bs = r->bit_off >= 0 ? r->bit_size : 0;
        if (read_raw(c, &L, r->type, r->bit_off, r->bit_size, &raw, err, errlen)) return -1;
        if (has_exp) {
            if (expected->n != 1 && !expected->is_string) { snprintf(err, errlen, "'%s' は 1 つの値の変数です。期待値に '|' は使えません", r->text); return -1; }
            if (expected->is_string && expected->n != 1) { snprintf(err, errlen, "'%s' は 1 つの値の変数です。文字列は比べられません", r->text); return -1; }
            *match = item_match(r->type, bs, raw, &expected->items[0]);
        }
        if (has_exp && expected->is_string && printable_str_byte(raw)) {
            sim_buf_printf(out, "'%c'", (char)raw);
        } else {
            format_num(r->type, bs, raw, fmt, out);
        }
        return 0;
    }
    {
        const sim_type *e = r->elem;
        int n, i, as_string, all = 1;
        uint64_t *raws;
        if (L.kind != RB_MEM) { snprintf(err, errlen, "'%s': 読み出し先がメモリではありません", r->text); return -1; }
        if (has_exp && expected->is_string && expected->n == 0) {
            /* 空文字列 '': 先頭の要素が 0（終端）かを比べる。入力の '' が終端の 0 だけを書くのと対。
             * 表示は期待値なしと同じ（配列なら終端までの文字列）。空なら '' と書く */
            uint64_t first;
            if (read_raw(c, &L, e, -1, 0, &first, err, errlen)) return -1;
            *match = first == 0;
            if (first == 0) { sim_buf_adds(out, "''"); return 0; }
            has_exp = 0;
            expected = NULL;
        }
        if (has_exp && r->count > 0 && (uint32_t)expected->n > r->count) {
            snprintf(err, errlen, "'%s': 期待値が %d 個ありますが、要素数は %u です", r->text, expected->n, r->count);
            return -1;
        }
        /* 期待値（または配列の要素数）の全要素を読んで比べる。途中で打ち切らない */
        if (has_exp) n = expected->n;
        else if (r->count > 0) n = (int)r->count;
        else n = 1;
        raws = sim_xmalloc(sizeof *raws * (size_t)n);
        for (i = 0; i < n; i++) {
            loc_t Li = L;
            Li.addr = L.addr + (uint64_t)i * e->size;
            if (read_raw(c, &Li, e, -1, 0, &raws[i], err, errlen)) { free(raws); return -1; }
        }
        if (has_exp) {
            for (i = 0; i < n; i++) if (!item_match(e, 0, raws[i], &expected->items[i])) all = 0;
            *match = all;
        }
        as_string = 0;
        if (e->size == 1 && e->kind != SIM_T_FLOAT && (has_exp ? expected->is_string : 1)) {
            int len = n;
            if (!has_exp) { for (len = 0; len < n && raws[len] != 0; len++) {} }
            as_string = len > 0 || has_exp;
            for (i = 0; i < len; i++) if (!printable_str_byte(raws[i])) as_string = 0;
            if (!has_exp && len == 0) as_string = 0;
            if (as_string) {
                sim_buf_addc(out, '\'');
                for (i = 0; i < len; i++) sim_buf_addc(out, (char)raws[i]);
                sim_buf_addc(out, '\'');
                free(raws);
                return 0;
            }
        }
        for (i = 0; i < n; i++) {
            if (i) sim_buf_addc(out, '|');
            format_num(e, 0, raws[i], fmt, out);
        }
        free(raws);
        return 0;
    }
}

int sim_ref_read_double(sim_ctx *c, const sim_ref *r, double *out)
{
    loc_t L;
    char err[256];
    uint64_t raw;
    num_t n;
    const sim_type *t;
    uint32_t bs = 0;
    if (r->prefix == '$') return -1;
    if (eval_loc(c, r, r->nsteps, &L, err, sizeof err)) return -1;
    if (L.kind == RB_ARG) {
        raw = mask_bits((uint64_t)c->args[r->argi], type_bits(r->type));
        t = r->type;
    } else if (r->multi) {
        t = r->elem;
        if (L.kind != RB_MEM || read_raw(c, &L, t, -1, 0, &raw, err, sizeof err)) return -1;
    } else {
        t = r->type;
        bs = r->bit_off >= 0 ? r->bit_size : 0;
        if (read_raw(c, &L, t, r->bit_off, r->bit_size, &raw, err, sizeof err)) return -1;
    }
    n = decode(t, bs, raw);
    *out = n.is_float ? n.f : (n.is_signed ? (double)n.i : (double)n.u);
    return 0;
}
