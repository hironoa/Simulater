#define _CRT_SECURE_NO_WARNINGS
#include "sim_util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define sim_mkdir_one(p) _mkdir(p)
#else
#define sim_mkdir_one(p) mkdir((p), 0777)
#endif

static void oom(void)
{
    fprintf(stderr, "sim: メモリが足りません\n");
    exit(2);
}

void *sim_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *sim_xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) oom();
    return p;
}

void *sim_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *sim_xstrndup(const char *s, size_t n)
{
    char *p = sim_xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *sim_xstrdup(const char *s) { return sim_xstrndup(s, strlen(s)); }

void sim_buf_init(sim_buf *b) { b->p = NULL; b->len = b->cap = 0; }

void sim_buf_free(sim_buf *b)
{
    free(b->p);
    sim_buf_init(b);
}

void sim_buf_reserve(sim_buf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + extra + 1) nc *= 2;
        b->p = sim_xrealloc(b->p, nc);
        b->cap = nc;
    }
}

void sim_buf_add(sim_buf *b, const void *data, size_t n)
{
    sim_buf_reserve(b, n);
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sim_buf_adds(sim_buf *b, const char *s) { sim_buf_add(b, s, strlen(s)); }
void sim_buf_addc(sim_buf *b, char c) { sim_buf_add(b, &c, 1); }

void sim_buf_vprintf(sim_buf *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    sim_buf_reserve(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void sim_buf_printf(sim_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sim_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

const char *sim_buf_cstr(sim_buf *b)
{
    sim_buf_reserve(b, 0);
    b->p[b->len] = 0;
    return b->p;
}

void sim_buf_json_strn(sim_buf *b, const char *s, size_t n)
{
    size_t i;
    sim_buf_addc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sim_buf_adds(b, "\\\""); break;
        case '\\': sim_buf_adds(b, "\\\\"); break;
        case '\n': sim_buf_adds(b, "\\n"); break;
        case '\r': sim_buf_adds(b, "\\r"); break;
        case '\t': sim_buf_adds(b, "\\t"); break;
        default:
            if (c < 0x20) sim_buf_printf(b, "\\u%04x", c);
            else sim_buf_addc(b, (char)c);
        }
    }
    sim_buf_addc(b, '"');
}

void sim_buf_json_str(sim_buf *b, const char *s) { sim_buf_json_strn(b, s ? s : "", s ? strlen(s) : 0); }

void sim_buf_json_num(sim_buf *b, double v)
{
    char tmp[64];
    if (isnan(v) || isinf(v)) { sim_buf_adds(b, "null"); return; }
    sim_format_double(tmp, sizeof tmp, v, 0);
    sim_buf_adds(b, tmp);
}

char *sim_read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    sim_buf b;
    char tmp[65536];
    size_t n;
    if (!fp) return NULL;
    sim_buf_init(&b);
    sim_buf_reserve(&b, 0);
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) sim_buf_add(&b, tmp, n);
    if (ferror(fp)) { fclose(fp); sim_buf_free(&b); return NULL; }
    fclose(fp);
    if (len) *len = b.len;
    return b.p;
}

int sim_write_file(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (len && fwrite(data, 1, len, fp) != len) { fclose(fp); return -1; }
    return fclose(fp) == 0 ? 0 : -1;
}

const char *sim_basename(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

int sim_path_suffix_match(const char *path, const char *suffix)
{
    size_t lp = strlen(path), ls = strlen(suffix), i;
    if (ls == 0) return 1;
    if (ls > lp) return 0;
    for (i = 0; i < ls; i++) {
        char a = path[lp - ls + i], c = suffix[i];
        if (a == '\\') a = '/';
        if (c == '\\') c = '/';
#ifdef _WIN32
        a = (char)tolower((unsigned char)a); c = (char)tolower((unsigned char)c);
#endif
        if (a != c) return 0;
    }
    if (lp == ls) return 1;
    return path[lp - ls - 1] == '/' || path[lp - ls - 1] == '\\';
}

char *sim_path_join(const char *dir, const char *name)
{
    sim_buf b;
    size_t n = strlen(dir);
    sim_buf_init(&b);
    sim_buf_adds(&b, dir);
    if (n && dir[n - 1] != '/' && dir[n - 1] != '\\') sim_buf_addc(&b, '/');
    sim_buf_adds(&b, name);
    return b.p;
}

int sim_mkdir_p(const char *path)
{
    char *p = sim_xstrdup(path), *q;
    struct stat st;
    for (q = p + 1; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char c = *q;
            *q = 0;
            if (q > p && q[-1] != ':') sim_mkdir_one(p);
            *q = c;
        }
    }
    sim_mkdir_one(p);
    if (stat(p, &st) != 0 || !(st.st_mode & S_IFDIR)) { free(p); return -1; }
    free(p);
    return 0;
}

int sim_strcasestr_found(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n == 0) return 1;
    for (; *hay; hay++) {
        for (i = 0; i < n; i++) {
            if (!hay[i]) return 0;
            if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i])) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

int sim_is_ident_start(int c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int sim_is_ident_char(int c) { return sim_is_ident_start(c) || (c >= '0' && c <= '9'); }

void sim_format_double(char *out, size_t n, double v, int is_float32)
{
    int prec;
    if (isnan(v)) { snprintf(out, n, "nan"); return; }
    if (isinf(v)) { snprintf(out, n, v < 0 ? "-inf" : "inf"); return; }
    if (v == 0) { snprintf(out, n, signbit(v) ? "-0" : "0"); return; }
    if (v == floor(v) && fabs(v) < 1e15) { snprintf(out, n, "%.0f", v); return; }
    for (prec = 1; prec <= 17; prec++) {
        snprintf(out, n, "%.*g", prec, v);
        if (is_float32) {
            if ((float)strtod(out, NULL) == (float)v) return;
        } else {
            if (strtod(out, NULL) == v) return;
        }
    }
}

/* ================================================================ 数値の文字列 */

int sim_parse_uint_n(const char *s, size_t n, uint64_t max, uint64_t *out)
{
    char tmp[128];
    char *end;
    size_t i, start = 0;
    int base = 10;
    unsigned long long v;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; start = 2; }
    if (start == n) return SIM_NUM_BAD;
    for (i = start; i < n; i++) {
        if (base == 16 ? !isxdigit((unsigned char)s[i]) : !isdigit((unsigned char)s[i])) return SIM_NUM_BAD;
    }
    /* 0 で始まる 10 進（010）は C では 8 進なので、どちらの意味か決められない */
    if (base == 10 && n > 1 && s[0] == '0') return SIM_NUM_OCTAL;
    if (n >= sizeof tmp) return SIM_NUM_RANGE;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    errno = 0;
    v = strtoull(tmp + start, &end, base);
    if (*end) return SIM_NUM_BAD;
    if (errno == ERANGE || (uint64_t)v > max) return SIM_NUM_RANGE;
    *out = (uint64_t)v;
    return SIM_NUM_OK;
}

int sim_parse_uint(const char *s, uint64_t max, uint64_t *out)
{
    return sim_parse_uint_n(s, strlen(s), max, out);
}

int sim_parse_double(const char *s, double *out)
{
    const char *p = s;
    char *end;
    double d;
    int digits = 0;
    /* 10 進の数字・小数点・指数だけ（nan / inf / 16 進の浮動小数 / 前後の空白は受け付けない） */
    if (*p == '+' || *p == '-') p++;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p)) digits++;
        else if (*p != '.' && *p != 'e' && *p != 'E' && *p != '+' && *p != '-') return SIM_NUM_BAD;
    }
    if (!digits) return SIM_NUM_BAD;
    if (!isdigit((unsigned char)s[*s == '+' || *s == '-']) && s[*s == '+' || *s == '-'] != '.') return SIM_NUM_BAD;
    errno = 0;
    d = strtod(s, &end);
    if (end == s || *end) return SIM_NUM_BAD;
    if (!isfinite(d)) return SIM_NUM_RANGE;
    /* あふれ（HUGE_VAL）と、0 でない値が 0 になるアンダーフロー。非正規化数で ERANGE を立てる実装もあるので、0 以外は受け付ける */
    if (errno == ERANGE && d == 0) {
        for (p = s; *p && *p != 'e' && *p != 'E'; p++) if (*p >= '1' && *p <= '9') return SIM_NUM_RANGE;
    }
    *out = d;
    return SIM_NUM_OK;
}
