#define _CRT_SECURE_NO_WARNINGS
#include "sim_cfg.h"
#include "sim_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sim_cfg_init(sim_cfg *c) { memset(c, 0, sizeof *c); }

char *sim_cfg_locate(const char *cfg_path, const char *abs_path)
{
    if (cfg_path) return sim_xstrdup(cfg_path);
    if (abs_path) {
        const char *b = sim_basename(abs_path);
        char *dir = sim_xstrndup(abs_path, (size_t)(b - abs_path));
        char *path = sim_path_join(dir[0] ? dir : ".", "sim.cfg");
        FILE *fp = sim_fopen(path, "rb");
        free(dir);
        if (fp) { fclose(fp); return path; }
        free(path);
    }
    return sim_xstrdup("sim.cfg");
}

void sim_cfg_free(sim_cfg *c)
{
    int i;
    for (i = 0; i < c->n; i++) { free(c->e[i].key); free(c->e[i].value); }
    free(c->e);
    free(c->path);
    free(c->dir);
    memset(c, 0, sizeof *c);
}

static char *trim_dup(const char *s, size_t n)
{
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return sim_xstrndup(s, n);
}

int sim_cfg_load(sim_cfg *c, const char *path, char *err, size_t errlen)
{
    size_t len, i = 0;
    int line = 0;
    char *d = sim_read_file(path, &len);
    if (!d) return -1;
    /* Windows: Shift_JIS などで保存した設定ファイルは UTF-8 に直す（中のパスをプログラム内の UTF-8 に揃える） */
    {
        size_t n2 = 0;
        char *u = sim_ansi_to_utf8(d, len, &n2);
        if (u) { free(d); d = u; len = n2; }
    }
    c->path = sim_xstrdup(path);
    {
        const char *b = sim_basename(path);
        c->dir = b > path ? sim_xstrndup(path, (size_t)(b - path - 1)) : sim_xstrdup(".");
        if (!c->dir[0]) { free(c->dir); c->dir = sim_xstrdup("/"); }
    }
    /* UTF-8 の BOM */
    if (len >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB && (unsigned char)d[2] == 0xBF) i = 3;
    while (i < len) {
        size_t s = i, e, k;
        const char *eq, *hash;
        line++;
        while (i < len && d[i] != '\n') i++;
        e = i;
        if (i < len) i++;
        hash = memchr(d + s, '#', e - s);
        if (hash) e = (size_t)(hash - d);
        /* 空行 */
        for (k = s; k < e && (d[k] == ' ' || d[k] == '\t' || d[k] == '\r'); k++) {}
        if (k == e) continue;
        eq = memchr(d + s, '=', e - s);
        if (!eq) {
            snprintf(err, errlen, "%s:%d: 'KEY = VALUE' の形ではありません", path, line);
            free(d);
            return -2;
        }
        c->e = sim_xrealloc(c->e, (size_t)(c->n + 1) * sizeof *c->e);
        c->e[c->n].key = trim_dup(d + s, (size_t)(eq - (d + s)));
        for (k = 0; c->e[c->n].key[k]; k++) c->e[c->n].key[k] = (char)tolower((unsigned char)c->e[c->n].key[k]);
        c->e[c->n].value = trim_dup(eq + 1, (size_t)(d + e - (eq + 1)));
        c->e[c->n].line = line;
        c->e[c->n].used = 0;
        c->n++;
    }
    free(d);
    return 0;
}

const char *sim_cfg_get(sim_cfg *c, const char *key)
{
    int i;
    const char *v = NULL;
    for (i = 0; i < c->n; i++)
        if (strcmp(c->e[i].key, key) == 0) { v = c->e[i].value; c->e[i].used = 1; }
    return v;
}

char *sim_cfg_path(const sim_cfg *c, const char *rel)
{
    if (!rel) return NULL;
    if (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':')) return sim_xstrdup(rel);
    if (!c->dir) return sim_xstrdup(rel);
    return sim_path_join(c->dir, rel);
}

int sim_cfg_csv_init(sim_cfg *c, const char *override, char *err, size_t errlen)
{
    const char *s = sim_cfg_get(c, "csv_init");
    if (override) s = override;
    if (!s || !strcmp(s, "auto")) return 0;
    if (!strcmp(s, "once")) return 1;
    if (!strcmp(s, "every")) return 2;
    snprintf(err, errlen, "csv_init = %s は読めません（auto / once / every）", s);
    return -1;
}
