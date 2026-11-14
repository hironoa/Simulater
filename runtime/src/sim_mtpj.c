/* sim_mtpj.c - CS+ のプロジェクトファイルから定義マクロを読む。形式と確度は sim_mtpj.h */
#define _CRT_SECURE_NO_WARNINGS
#include "sim_mtpj.h"
#include "sim_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 要素を 1 つずつ取り出す最小限の XML 走査。属性は読み飛ばし、文字列は次の '<' までとする */
typedef struct {
    const char *p, *end;
    const char *name; size_t name_len;      /* 要素名 */
    const char *text; size_t text_len;      /* 開始タグの直後から次の '<' まで（終了タグ・空要素なら 0） */
    int closing, empty;
} xml;

static int xml_next(xml *x)
{
    while (x->p < x->end) {
        const char *s = memchr(x->p, '<', (size_t)(x->end - x->p)), *q;
        char quote = 0;
        if (!s) break;
        s++;
        if (x->end - s >= 3 && !memcmp(s, "!--", 3)) {      /* コメント */
            const char *e = s + 3;
            while (e + 3 <= x->end && memcmp(e, "-->", 3)) e++;
            if (e + 3 > x->end) break;
            x->p = e + 3; continue;
        }
        if (s < x->end && (*s == '?' || *s == '!')) {
            q = memchr(s, '>', (size_t)(x->end - s));
            if (!q) break;
            x->p = q + 1; continue;
        }
        x->closing = s < x->end && *s == '/';
        if (x->closing) s++;
        x->name = s;
        while (s < x->end && *s != '>' && *s != '/' && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
        x->name_len = (size_t)(s - x->name);
        for (q = s; q < x->end; q++) {
            if (quote) { if (*q == quote) quote = 0; }
            else if (*q == '"' || *q == '\'') quote = *q;
            else if (*q == '>') break;
        }
        if (q >= x->end) break;
        x->empty = q > s && q[-1] == '/';
        x->text = q + 1;
        {
            const char *t = memchr(q + 1, '<', (size_t)(x->end - (q + 1)));
            x->text_len = x->closing || x->empty ? 0 : (size_t)((t ? t : x->end) - (q + 1));
        }
        x->p = q + 1;
        if (!x->name_len) continue;
        return 1;
    }
    x->p = x->end;
    return 0;
}

static int name_is(const xml *x, const char *s) { return strlen(s) == x->name_len && !memcmp(x->name, s, x->name_len); }
static int name_starts(const xml *x, const char *s) { size_t n = strlen(s); return x->name_len >= n && !memcmp(x->name, s, n); }

static void add_utf8(sim_buf *b, unsigned long c)
{
    if (c < 0x80) sim_buf_addc(b, (char)c);
    else if (c < 0x800) { sim_buf_addc(b, (char)(0xC0 | (c >> 6))); sim_buf_addc(b, (char)(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) {
        sim_buf_addc(b, (char)(0xE0 | (c >> 12))); sim_buf_addc(b, (char)(0x80 | ((c >> 6) & 0x3F)));
        sim_buf_addc(b, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x110000) {
        sim_buf_addc(b, (char)(0xF0 | (c >> 18))); sim_buf_addc(b, (char)(0x80 | ((c >> 12) & 0x3F)));
        sim_buf_addc(b, (char)(0x80 | ((c >> 6) & 0x3F))); sim_buf_addc(b, (char)(0x80 | (c & 0x3F)));
    }
}

/* XML の文字参照を戻す */
static char *unescape(const char *s, size_t n)
{
    static const struct { const char *name; char c; } ent[] = {
        {"lt;", '<'}, {"gt;", '>'}, {"amp;", '&'}, {"quot;", '"'}, {"apos;", '\''}
    };
    sim_buf b;
    size_t i = 0, k;
    sim_buf_init(&b);
    while (i < n) {
        if (s[i] == '&') {
            for (k = 0; k < sizeof ent / sizeof ent[0]; k++) {
                size_t len = strlen(ent[k].name);
                if (n - i - 1 >= len && !memcmp(s + i + 1, ent[k].name, len)) { sim_buf_addc(&b, ent[k].c); i += len + 1; break; }
            }
            if (k < sizeof ent / sizeof ent[0]) continue;
            if (i + 2 < n && s[i + 1] == '#') {
                unsigned long c = 0;
                size_t j = i + 2, digits = 0;
                int hex = j < n && (s[j] == 'x' || s[j] == 'X');
                if (hex) j++;
                for (; j < n && digits < 8; j++, digits++) {
                    unsigned ch = (unsigned char)s[j];
                    if (ch >= '0' && ch <= '9') c = c * (hex ? 16u : 10u) + (ch - '0');
                    else if (hex && ch >= 'a' && ch <= 'f') c = c * 16u + (ch - 'a' + 10);
                    else if (hex && ch >= 'A' && ch <= 'F') c = c * 16u + (ch - 'A' + 10);
                    else break;
                }
                if (digits && j < n && s[j] == ';') { add_utf8(&b, c); i = j + 1; continue; }
            }
        }
        sim_buf_addc(&b, s[i++]);
    }
    {
        char *out = sim_xstrdup(sim_buf_cstr(&b));
        sim_buf_free(&b);
        return out;
    }
}

/* Base64 にした UTF-16LE の文字列を UTF-8 にする。Base64 として読めなければ NULL */
static char *base64_utf16(const char *s, size_t n)
{
    unsigned char *raw = sim_xmalloc(n + 4);
    size_t len = 0, i;
    unsigned acc = 0, bits = 0;
    sim_buf b;
    char *out;
    for (i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        else { free(raw); return NULL; }
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; raw[len++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1u; }
    }
    if (!len || (len & 1)) { free(raw); return NULL; }
    sim_buf_init(&b);
    for (i = 0; i + 1 < len; i += 2) {
        unsigned long c = raw[i] | (unsigned long)raw[i + 1] << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
            unsigned long lo = raw[i + 2] | (unsigned long)raw[i + 3] << 8;
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (!c) { sim_buf_free(&b); free(raw); return NULL; }
        add_utf8(&b, c);
    }
    out = sim_xstrdup(sim_buf_cstr(&b));
    sim_buf_free(&b); free(raw);
    return out;
}

static void add_string(char ***list, int *n, const char *s)
{
    *list = sim_xrealloc(*list, (size_t)(*n + 1) * sizeof **list);
    (*list)[(*n)++] = sim_xstrdup(s);
}

/* 「名前」または「名前=値」か */
static int macro_line(const char *s)
{
    if (!sim_is_ident_start((unsigned char)*s)) return 0;
    while (sim_is_ident_char((unsigned char)*s)) s++;
    return !*s || *s == '=';
}

/* 改行区切りの一覧を 1 行ずつ足す。undef では「名前」だけを受け付ける */
static void split_lines(sim_mtpj *m, const char *text, size_t n, int undef)
{
    char *all = unescape(text, n), *p = all;
    while (*p) {
        char *e = p + strcspn(p, "\n"), *q = e, saved = *e;
        while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r')) q--;
        while (p < q && (*p == ' ' || *p == '\t')) p++;
        *q = 0;
        if (*p) {
            if (!macro_line(p) || (undef && strchr(p, '='))) add_string(&m->bad, &m->nbad, p);
            else if (undef) add_string(&m->undefs, &m->nundefs, p);
            else add_string(&m->defs, &m->ndefs, p);
        }
        if (!saved) break;
        p = e + 1;
    }
    free(all);
}

void sim_mtpj_free(sim_mtpj *m)
{
    int i;
    for (i = 0; i < m->nmodes; i++) free(m->modes[i]);
    for (i = 0; i < m->ndefs; i++) free(m->defs[i]);
    for (i = 0; i < m->nundefs; i++) free(m->undefs[i]);
    for (i = 0; i < m->nbad; i++) free(m->bad[i]);
    free(m->modes); free(m->defs); free(m->undefs); free(m->bad);
    free(m->current); free(m->def_element); free(m->undef_element);
    memset(m, 0, sizeof *m);
}

int sim_mtpj_load(sim_mtpj *m, const char *path, const char *mode, char *err, size_t errlen)
{
    size_t len;
    char *data = sim_read_file(path, &len);
    xml x;
    char def_name[64], undef_name[64];
    /* 採用する Instance の候補: 先に見つけた「MacroUndefinitions を併せ持つもの」、なければ最初のもの */
    const char *def_text = NULL, *undef_text = NULL, *first_def = NULL;
    size_t def_len = 0, undef_len = 0, first_len = 0;
    const char *cur_def = NULL, *cur_undef = NULL;
    size_t cur_def_len = 0, cur_undef_len = 0;
    int in_instance = 0, cur_has_def = 0, cur_has_undef = 0, cur_has_any_undef = 0, found_first = 0, i;
    memset(m, 0, sizeof *m);
    if (!data) { snprintf(err, errlen, "mtpj: %s を読めません", path); return -1; }
    if (memchr(data, 0, len)) {
        snprintf(err, errlen, "mtpj: %s は UTF-8 の XML ではありません（NUL を含みます。UTF-16 で保存されていませんか）", path);
        free(data); return -1;
    }
    /* 1 回目: ビルド・モード */
    memset(&x, 0, sizeof x);
    x.p = data; x.end = data + len;
    if (len >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) x.p += 3;
    while (xml_next(&x)) {
        if (x.closing) continue;
        if (name_is(&x, "CurrentBuildMode") && !m->current) m->current = unescape(x.text, x.text_len);
        else if (name_starts(&x, "BuildMode") && x.name_len > 9 && x.name[9] >= '0' && x.name[9] <= '9') {
            size_t k = 9;
            unsigned long idx = 0;
            for (; k < x.name_len && x.name[k] >= '0' && x.name[k] <= '9' && idx < 100000; k++) idx = idx * 10 + (unsigned long)(x.name[k] - '0');
            if (k == x.name_len && idx == (unsigned long)m->nmodes) {
                char *name = base64_utf16(x.text, x.text_len);
                if (!name) name = unescape(x.text, x.text_len);     /* Base64 でなければそのまま */
                add_string(&m->modes, &m->nmodes, name);
                free(name);
            }
        }
    }
    if (!m->nmodes) {
        snprintf(err, errlen, "mtpj: %s にビルド・モード（<BuildMode0>）が見つかりません。CS+ のプロジェクトファイルではないか、未対応の版です", path);
        free(data); sim_mtpj_free(m); return -1;
    }
    m->mode = -1;
    if (mode) {
        for (i = 0; i < m->nmodes; i++) if (!strcmp(m->modes[i], mode)) m->mode = i;
        if (m->mode < 0) {
            sim_buf b;
            sim_buf_init(&b);
            for (i = 0; i < m->nmodes; i++) sim_buf_printf(&b, "%s%s", i ? ", " : "", m->modes[i]);
            snprintf(err, errlen, "mtpj: %s にビルド・モード '%s' がありません（ある名前: %s）", path, mode, sim_buf_cstr(&b));
            sim_buf_free(&b); free(data); sim_mtpj_free(m); return -1;
        }
    } else {
        if (m->current) for (i = 0; i < m->nmodes; i++) if (!strcmp(m->modes[i], m->current)) m->mode = i;
        if (m->mode < 0) m->mode = 0;
    }
    snprintf(def_name, sizeof def_name, "MacroDefinitions-%d", m->mode);
    snprintf(undef_name, sizeof undef_name, "MacroUndefinitions-%d", m->mode);
    /* 2 回目: 定義マクロ */
    memset(&x, 0, sizeof x);
    x.p = data; x.end = data + len;
    for (;;) {
        int more = xml_next(&x);
        int boundary = !more || name_is(&x, "Instance");
        if (boundary && in_instance) {
            if (cur_has_def) {
                if (!found_first) { found_first = 1; first_def = cur_def; first_len = cur_def_len; }
                if (cur_has_any_undef && !m->compiler_instance) {
                    m->compiler_instance = 1;
                    def_text = cur_def; def_len = cur_def_len;
                    if (cur_has_undef) { undef_text = cur_undef; undef_len = cur_undef_len; m->undef_element = sim_xstrdup(undef_name); }
                }
            }
            in_instance = 0;
        }
        if (!more) break;
        if (boundary && !x.closing) {
            in_instance = 1; cur_has_def = cur_has_undef = cur_has_any_undef = 0;
            cur_def = cur_undef = NULL; cur_def_len = cur_undef_len = 0;
            continue;
        }
        if (x.closing) continue;
        if (name_is(&x, def_name)) { cur_has_def = 1; cur_def = x.text; cur_def_len = x.text_len; }
        else if (name_is(&x, undef_name)) { cur_has_undef = cur_has_any_undef = 1; cur_undef = x.text; cur_undef_len = x.text_len; }
        else if (name_starts(&x, "MacroUndefinitions-")) cur_has_any_undef = 1;
        else if (!name_starts(&x, "MacroDefinitions-") && !name_starts(&x, "MacroDefinitionFor")) {
            /* SourceItem0-MacroDefinitions-0 のような、ファイルごとの個別オプションらしい要素 */
            size_t k;
            for (k = 0; k + 16 <= x.name_len; k++)
                if (!memcmp(x.name + k, "MacroDefinitions", 16) && x.text_len) { m->individual++; break; }
        }
    }
    if (!m->compiler_instance && found_first) { def_text = first_def; def_len = first_len; }
    if (found_first) m->def_element = sim_xstrdup(def_name);
    if (def_text) split_lines(m, def_text, def_len, 0);
    if (undef_text) split_lines(m, undef_text, undef_len, 1);
    free(data);
    return 0;
}
