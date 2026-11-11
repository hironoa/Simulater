#define _CRT_SECURE_NO_WARNINGS
#include "sim_json.h"
#include "sim_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s, *e, *p;
    char *err;
    size_t errlen;
    int failed;
    int depth;
} sj_parser;

static void fail(sj_parser *ps, const char *msg)
{
    if (!ps->failed) {
        ps->failed = 1;
        if (ps->err && ps->errlen)
            snprintf(ps->err, ps->errlen, "JSON の %ld 文字目: %s", (long)(ps->p - ps->s) + 1, msg);
    }
}

static void ws(sj_parser *ps)
{
    while (ps->p < ps->e && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static sj_value *new_val(sj_type t)
{
    sj_value *v = sim_xcalloc(1, sizeof *v);
    v->type = t;
    return v;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(sim_buf *b, unsigned cp)
{
    if (cp < 0x80) sim_buf_addc(b, (char)cp);
    else if (cp < 0x800) { sim_buf_addc(b, (char)(0xC0 | (cp >> 6))); sim_buf_addc(b, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        sim_buf_addc(b, (char)(0xE0 | (cp >> 12)));
        sim_buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sim_buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sim_buf_addc(b, (char)(0xF0 | (cp >> 18)));
        sim_buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sim_buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sim_buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *parse_string(sj_parser *ps, size_t *outlen)
{
    sim_buf b;
    sim_buf_init(&b);
    sim_buf_reserve(&b, 0);
    ps->p++; /* " */
    while (ps->p < ps->e && *ps->p != '"') {
        char c = *ps->p++;
        if (c == '\\') {
            if (ps->p >= ps->e) break;
            c = *ps->p++;
            switch (c) {
            case 'n': sim_buf_addc(&b, '\n'); break;
            case 't': sim_buf_addc(&b, '\t'); break;
            case 'r': sim_buf_addc(&b, '\r'); break;
            case 'b': sim_buf_addc(&b, '\b'); break;
            case 'f': sim_buf_addc(&b, '\f'); break;
            case 'u': {
                unsigned cp = 0;
                int i;
                if (ps->e - ps->p < 4) { fail(ps, "\\u の後が短すぎます"); sim_buf_free(&b); return NULL; }
                for (i = 0; i < 4; i++) {
                    int h = hexv(ps->p[i]);
                    if (h < 0) { fail(ps, "\\u の後が 16 進数ではありません"); sim_buf_free(&b); return NULL; }
                    cp = cp * 16 + (unsigned)h;
                }
                ps->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && ps->e - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    for (i = 0; i < 4; i++) {
                        int h = hexv(ps->p[2 + i]);
                        if (h < 0) ok = 0; else lo = lo * 16 + (unsigned)h;
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                put_utf8(&b, cp);
                break;
            }
            default: sim_buf_addc(&b, c); break;
            }
        } else {
            sim_buf_addc(&b, c);
        }
    }
    if (ps->p >= ps->e) { fail(ps, "文字列が閉じていません"); sim_buf_free(&b); return NULL; }
    ps->p++;
    *outlen = b.len;
    return b.p;
}

static sj_value *parse_value(sj_parser *ps);

static sj_value *parse_container(sj_parser *ps, int is_obj)
{
    sj_value *v = new_val(is_obj ? SJ_OBJ : SJ_ARR), *last = NULL;
    char close = is_obj ? '}' : ']';
    ps->p++;
    ws(ps);
    if (ps->p < ps->e && *ps->p == close) { ps->p++; return v; }
    for (;;) {
        char *key = NULL;
        sj_value *c;
        ws(ps);
        if (is_obj) {
            size_t kl;
            if (ps->p >= ps->e || *ps->p != '"') { fail(ps, "キーの文字列がありません"); sj_free(v); return NULL; }
            key = parse_string(ps, &kl);
            if (!key) { sj_free(v); return NULL; }
            ws(ps);
            if (ps->p >= ps->e || *ps->p != ':') { free(key); fail(ps, "':' がありません"); sj_free(v); return NULL; }
            ps->p++;
        }
        c = parse_value(ps);
        if (!c) { free(key); sj_free(v); return NULL; }
        c->key = key;
        if (last) last->next = c; else v->child = c;
        last = c;
        v->nchild++;
        ws(ps);
        if (ps->p < ps->e && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->e && *ps->p == close) { ps->p++; return v; }
        fail(ps, is_obj ? "',' か '}' がありません" : "',' か ']' がありません");
        sj_free(v);
        return NULL;
    }
}

static sj_value *parse_value(sj_parser *ps)
{
    sj_value *v;
    ws(ps);
    if (ps->p >= ps->e) { fail(ps, "値がありません"); return NULL; }
    if (++ps->depth > 200) { fail(ps, "入れ子が深すぎます"); return NULL; }
    switch (*ps->p) {
    case '{': v = parse_container(ps, 1); break;
    case '[': v = parse_container(ps, 0); break;
    case '"': {
        size_t l;
        char *s = parse_string(ps, &l);
        if (!s) { v = NULL; break; }
        v = new_val(SJ_STR);
        v->str = s;
        v->len = l;
        break;
    }
    case 't':
        if (ps->e - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) { ps->p += 4; v = new_val(SJ_BOOL); v->num = 1; break; }
        fail(ps, "不正な値"); v = NULL; break;
    case 'f':
        if (ps->e - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) { ps->p += 5; v = new_val(SJ_BOOL); v->num = 0; break; }
        fail(ps, "不正な値"); v = NULL; break;
    case 'n':
        if (ps->e - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) { ps->p += 4; v = new_val(SJ_NULL); break; }
        fail(ps, "不正な値"); v = NULL; break;
    default: {
        char tmp[64];
        const char *st = ps->p;
        size_t n;
        char *end;
        while (ps->p < ps->e && (strchr("+-.eE", *ps->p) || (*ps->p >= '0' && *ps->p <= '9'))) ps->p++;
        n = (size_t)(ps->p - st);
        if (n == 0 || n >= sizeof tmp) { fail(ps, "不正な値"); v = NULL; break; }
        memcpy(tmp, st, n);
        tmp[n] = 0;
        v = new_val(SJ_NUM);
        v->num = strtod(tmp, &end);
        v->raw = st;
        v->rawlen = n;
        if (*end) { fail(ps, "不正な数値"); sj_free(v); v = NULL; }
        break;
    }
    }
    ps->depth--;
    return v;
}

sj_value *sj_parse(const char *text, size_t len, char *err, size_t errlen)
{
    sj_parser ps;
    sj_value *v;
    memset(&ps, 0, sizeof ps);
    ps.s = ps.p = text;
    ps.e = text + len;
    ps.err = err;
    ps.errlen = errlen;
    v = parse_value(&ps);
    if (!v) return NULL;
    ws(&ps);
    if (ps.p != ps.e) { fail(&ps, "値の後に余分な文字があります"); sj_free(v); return NULL; }
    return v;
}

void sj_free(sj_value *v)
{
    while (v) {
        sj_value *n = v->next;
        sj_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = n;
    }
}

sj_value *sj_get(const sj_value *obj, const char *key)
{
    sj_value *c;
    if (!obj || obj->type != SJ_OBJ) return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const char *sj_get_str(const sj_value *obj, const char *key, const char *def)
{
    sj_value *v = sj_get(obj, key);
    return v && v->type == SJ_STR ? v->str : def;
}

double sj_get_num(const sj_value *obj, const char *key, double def)
{
    sj_value *v = sj_get(obj, key);
    return v && (v->type == SJ_NUM || v->type == SJ_BOOL) ? v->num : def;
}

int sj_get_bool(const sj_value *obj, const char *key, int def)
{
    sj_value *v = sj_get(obj, key);
    if (!v) return def;
    if (v->type == SJ_BOOL || v->type == SJ_NUM) return v->num != 0;
    return def;
}
