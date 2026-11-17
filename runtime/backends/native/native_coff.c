/* native_coff.c - x64 の通常の COFF オブジェクトの外部シンボルと、LLVM IR の宣言を読む */
#include "native_coff.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned u16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int native_coff_symbols(const unsigned char *data, size_t len, native_coff_visit visit,
                        void *ctx, char *err, size_t errlen)
{
    size_t off, count, strings, size, i;
    if (!data || len < 20 || u16(data) != 0x8664) goto bad;
    off = u32(data + 8);
    count = u32(data + 12);
    if (!off && !count) return 0;
    if (off < 20 || off > len || count > (len - off) / 18) goto bad;
    strings = off + count * 18;
    if (len - strings < 4) goto bad;
    size = u32(data + strings);
    if (size < 4 || size > len - strings) goto bad;
    for (i = 0; i < count;) {
        const unsigned char *s = data + off + i * 18;
        const char *name;
        char short_name[9];
        unsigned section = u16(s + 12), aux = s[17];
        if ((size_t)aux >= count - i) goto bad;
        if (!u32(s)) {
            size_t n = u32(s + 4);
            if (n < 4 || n >= size) goto bad;
            name = (const char *)data + strings + n;
            if (!memchr(name, 0, size - n)) goto bad;
        } else {
            memcpy(short_name, s, 8);
            short_name[8] = 0;
            name = short_name;
        }
        if (s[16] == 2 && visit)
            visit(ctx, name, section != 0 || u32(s + 8) != 0, ((u16(s + 14) >> 4) & 3) == 2);
        i += (size_t)aux + 1;
    }
    return 0;
bad:
    if (err && errlen) snprintf(err, errlen, "COFF の形式が読めないか、シンボル表が壊れています");
    return -1;
}

/* ---- LLVM IR のテキストの宣言 ---- */

#define LL_SIZE_LIMIT (1UL << 28)   /* これより大きい型は「不明」とする（掛け算のあふれを防ぐ） */

static int ll_ident(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '$' || c == '-';
}
static int ll_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static const char *ll_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}
/* p から word が始まり、その後ろが名前の文字でなければ、後ろを返す */
static const char *ll_word(const char *p, const char *end, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(end - p) < n || memcmp(p, word, n)) return NULL;
    if (p + n < end && ll_ident((unsigned char)p[n])) return NULL;
    return p + n;
}

/* p は '@' の次。名前（"…" で囲まれていれば \HH を戻す）を malloc して *out に入れ、名前の後ろを返す。
 * 読めない名前、NUL・改行を含む名前は NULL */
static const char *ll_name(const char *p, const char *end, char **out)
{
    const char *q;
    char *buf;
    size_t n = 0;
    *out = NULL;
    if (p < end && *p == '"') {
        q = ++p;
        while (q < end && *q != '"') q++;
        if (q >= end || q == p) return NULL;
        if (!(buf = malloc((size_t)(q - p) + 1))) return NULL;
        while (p < q) {
            int c = (unsigned char)*p;
            if (c == 0x5C /* バックスラッシュ */ && q - p >= 3 && ll_hex((unsigned char)p[1]) >= 0 && ll_hex((unsigned char)p[2]) >= 0) {
                c = ll_hex((unsigned char)p[1]) * 16 + ll_hex((unsigned char)p[2]);
                p += 3;
            } else p++;
            if (c == 0 || c == '\n' || c == '\r') { free(buf); return NULL; }
            buf[n++] = (char)c;
        }
        buf[n] = 0;
        *out = buf;
        return q + 1;
    }
    q = p;
    while (q < end && ll_ident((unsigned char)*q)) q++;
    if (q == p) return NULL;
    if (!(buf = malloc((size_t)(q - p) + 1))) return NULL;
    memcpy(buf, p, (size_t)(q - p));
    buf[q - p] = 0;
    *out = buf;
    return q;
}

/* 型の大きさ（バイト）。分からなければ 0 */
static unsigned long ll_type_size(const char **pp, const char *end, int depth)
{
    const char *p = ll_ws(*pp, end), *q;
    unsigned long v = 0, inner;
    if (depth > 32) return 0;
    if (p + 1 < end && *p == 'i' && p[1] >= '0' && p[1] <= '9') {
        for (q = p + 1; q < end && *q >= '0' && *q <= '9'; q++) {
            v = v * 10 + (unsigned long)(*q - '0');
            if (v > LL_SIZE_LIMIT) return 0;
        }
        if (q < end && ll_ident((unsigned char)*q)) return 0;
        *pp = q;
        return (v + 7) / 8;
    }
    if ((q = ll_word(p, end, "ptr"))) { *pp = q; return 8; }
    if ((q = ll_word(p, end, "float"))) { *pp = q; return 4; }
    if ((q = ll_word(p, end, "double"))) { *pp = q; return 8; }
    if (p < end && *p == '[') {
        p = ll_ws(p + 1, end);
        if (p >= end || *p < '0' || *p > '9') return 0;
        for (; p < end && *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (unsigned long)(*p - '0');
            if (v > LL_SIZE_LIMIT) return 0;
        }
        p = ll_ws(p, end);
        if (!(q = ll_word(p, end, "x"))) return 0;
        inner = ll_type_size(&q, end, depth + 1);
        q = ll_ws(q, end);
        if (q >= end || *q != ']') return 0;
        if (!inner || !v || v > LL_SIZE_LIMIT / inner) return 0;
        *pp = q + 1;
        return v * inner;
    }
    return 0;
}

static void ll_line(const char *p, const char *end, native_ll_visit visit, void *ctx)
{
    char *name = NULL;
    const char *q;
    if ((q = ll_word(p, end, "declare"))) {
        /* declare [属性や戻り値の型] @名前(引数) … 。型や属性に '@' は出ない */
        q = memchr(q, '@', (size_t)(end - q));
        if (!q || !(q = ll_name(q + 1, end, &name))) return;
        if (q < end && *q == '(') visit(ctx, name, 1, 0);
    } else if (p < end && *p == '@') {
        /* @名前 = [linkage など] external [dso_local など] global|constant 型, … */
        int ext = 0;
        if (!(q = ll_name(p + 1, end, &name))) return;
        q = ll_ws(q, end);
        if (q >= end || *q != '=') goto done;
        for (q++;;) {
            const char *t, *w;
            q = ll_ws(q, end);
            if (q >= end) break;
            for (t = q; t < end && *t != ' ' && *t != '\t'; t++) {}
            if ((w = ll_word(q, t, "external")) == t || (w = ll_word(q, t, "extern_weak")) == t) ext = 1;
            else if ((w = ll_word(q, t, "global")) == t || (w = ll_word(q, t, "constant")) == t) {
                if (ext) visit(ctx, name, 0, ll_type_size(&t, end, 0));
                break;
            }
            q = t;
        }
    }
done:
    free(name);
}

void native_ll_decls(const char *text, size_t len, native_ll_visit visit, void *ctx)
{
    const char *p = text, *end = text ? text + len : NULL;
    if (!text || !visit) return;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *le;
        if (!e) e = end;
        le = e;
        if (le > p && le[-1] == '\r') le--;
        ll_line(p, le, visit, ctx);
        p = e < end ? e + 1 : end;
    }
}
