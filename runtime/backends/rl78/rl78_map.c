/*
 * rl78_map.c - Renesas Optimizing Linker（rlink）のリンクマップ（.map）
 *
 *   *** Mapping List ***            セクション名の行の次に「開始 終了 大きさ 整列」
 *   *** Symbol List ***             （-show=symbol のとき）シンボル名の行の次に「アドレス 大きさ 種類 ,g|,l 参照数」
 *                                    名前が長くなければ同じ行に続くこともあるので両方を読む
 * 数値は 16 進。
 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "sim_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_hex_tok(const char *s, size_t n)
{
    size_t i;
    if (!n) return 0;
    for (i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* 空白区切りで最大 8 個に分ける */
static int tokens(const char *s, size_t n, const char **tp, size_t *tl, int max)
{
    int k = 0;
    size_t i = 0;
    while (i < n && k < max) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
        if (i >= n) break;
        tp[k] = s + i;
        while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') i++;
        tl[k] = (size_t)(s + i - tp[k]);
        k++;
    }
    return k;
}

static const char *g_cur_file = "";

static void push_sym(rl78_map *m, const char *name, size_t nl, uint32_t addr, uint32_t size, const char *kind, size_t kl, int global)
{
    rl78_mapsym *y;
    if (m->nsym == m->capsym) { m->capsym = m->capsym ? m->capsym * 2 : 256; m->sym = sim_xrealloc(m->sym, (size_t)m->capsym * sizeof *m->sym); }
    y = &m->sym[m->nsym++];
    y->name = ar_strndup(&m->ar, name, nl);
    y->file = g_cur_file;
    y->addr = addr;
    y->size = size;
    y->kind = (kl >= 4 && strncmp(kind, "func", 4) == 0) ? 'f' : (kl >= 4 && strncmp(kind, "data", 4) == 0) ? 'd' : 'n';
    y->global = global;
}

int rl78_map_load(rl78_map *m, const char *path)
{
    size_t len, i = 0;
    char *d = sim_read_file(path, &len);
    enum { NONE, MAPPING, SYMBOL } mode = NONE;
    const char *pend_name = NULL;
    size_t pend_len = 0;
    memset(m, 0, sizeof *m);
    if (!d) return -1;
    g_cur_file = "";
    while (i < len) {
        size_t s = i, n;
        const char *tp[8];
        size_t tl[8];
        int k;
        while (i < len && d[i] != '\n') i++;
        n = i - s;
        if (i < len) i++;
        if (n >= 3 && strncmp(d + s, "***", 3) == 0) {
            {
                char *line = sim_xstrndup(d + s, n);
                if (strstr(line, "Mapping List")) mode = MAPPING;
                else if (strstr(line, "Symbol List")) mode = SYMBOL;
                else mode = NONE;
                free(line);
            }
            pend_name = NULL;
            continue;
        }
        if (mode == NONE) continue;
        k = tokens(d + s, n, tp, tl, 8);
        if (k == 0) continue;
        if (mode == MAPPING) {
            if (k == 1 && tp[0][0] == '.') { pend_name = tp[0]; pend_len = tl[0]; continue; }
            if (pend_name && k >= 3 && is_hex_tok(tp[0], tl[0]) && is_hex_tok(tp[1], tl[1]) && is_hex_tok(tp[2], tl[2])) {
                rl78_mapsect *x;
                if (m->nsect == m->capsect) { m->capsect = m->capsect ? m->capsect * 2 : 64; m->sect = sim_xrealloc(m->sect, (size_t)m->capsect * sizeof *m->sect); }
                x = &m->sect[m->nsect++];
                x->name = ar_strndup(&m->ar, pend_name, pend_len);
                x->start = (uint32_t)strtoul(tp[0], NULL, 16);
                x->end = (uint32_t)strtoul(tp[1], NULL, 16);
                x->size = (uint32_t)strtoul(tp[2], NULL, 16);
                pend_name = NULL;
            }
            continue;
        }
        /* Symbol List */
        if (k >= 1 && strncmp(tp[0], "FILE=", 5) == 0) {
            g_cur_file = ar_strndup(&m->ar, tp[0] + 5, tl[0] - 5);
            pend_name = NULL;
            continue;
        }
        if (k >= 1 && strncmp(tp[0], "SECTION=", 8) == 0) { pend_name = NULL; continue; }
        /* 「名前 アドレス 大きさ 種類 ,g 参照数」が 1 行 */
        if (k >= 5 && !is_hex_tok(tp[0], tl[0]) && is_hex_tok(tp[1], tl[1]) && tp[4][0] == ',') {
            push_sym(m, tp[0], tl[0], (uint32_t)strtoul(tp[1], NULL, 16), (uint32_t)strtoul(tp[2], NULL, 16), tp[3], tl[3], tl[4] >= 2 && tp[4][1] == 'g');
            pend_name = NULL;
            continue;
        }
        if (k == 1 && !is_hex_tok(tp[0], tl[0])) { pend_name = tp[0]; pend_len = tl[0]; continue; }
        if (k == 1 && is_hex_tok(tp[0], tl[0]) && tl[0] < 8) { pend_name = tp[0]; pend_len = tl[0]; continue; } /* 16 進に見える短い名前 */
        if (pend_name && k >= 4 && is_hex_tok(tp[0], tl[0]) && tp[3][0] == ',') {
            push_sym(m, pend_name, pend_len, (uint32_t)strtoul(tp[0], NULL, 16), (uint32_t)strtoul(tp[1], NULL, 16), tp[2], tl[2], tl[3] >= 2 && tp[3][1] == 'g');
            pend_name = NULL;
            continue;
        }
        if (pend_name && k >= 3 && is_hex_tok(tp[0], tl[0]) && is_hex_tok(tp[1], tl[1]) && is_hex_tok(tp[2], tl[2])) {
            /* セクションの見出し（開始 終了 大きさ）: 名前ではなかった */
            pend_name = NULL;
        }
    }
    /* 名前は ar に写したので元のバッファは捨ててよい */
    free(d);
    return 0;
}

void rl78_map_free(rl78_map *m)
{
    free(m->sym);
    free(m->sect);
    ar_free(&m->ar);
    memset(m, 0, sizeof *m);
}

void rl78_symtab_from_map(rl78_symtab *t, const rl78_map *m, rl78_warns *w)
{
    int i;
    (void)w;
    for (i = 0; i < m->nsym; i++) {
        const rl78_mapsym *y = &m->sym[i];
        const char *name, *func;
        int st;
        size_t k;
        int found = 0;
        rl78_split_symname(t, y->name, &name, &func, &st);
        if (!name[0] || y->name[0] == '.') continue;
        if (y->kind == 'd') {
            for (k = 0; k < t->nvars; k++) {
                if (t->vars[k].addr == y->addr && strcmp(t->vars[k].name, name) == 0) {
                    found = 1;
                    /* .symtab から来た変数にはファイルが分からないので、マップの FILE= を使う */
                    if ((!t->vars[k].file || !t->vars[k].file[0]) && y->file[0]) t->vars[k].file = ar_strdup(&t->ar, y->file);
                }
            }
            if (!found) {
                sim_var *v = rl78_symtab_add_var(t);
                sim_type *u = ar_alloc(&t->ar, sizeof *u);
                u->kind = SIM_T_UNKNOWN;
                u->name = "?";
                u->size = y->size;
                v->name = name;
                v->func = func;
                v->file = ar_strdup(&t->ar, y->file);
                v->addr = y->addr;
                v->type = u;
                v->is_external = y->global && !st && !func;
            }
        } else if (y->kind == 'f') {
            for (k = 0; k < t->nfuncs; k++) if (t->funcs[k].f.addr == y->addr) found = 1;
            if (!found) {
                rl78_funcinfo *fi = rl78_symtab_add_func(t);
                fi->f.name = name;
                fi->f.file = "";
                fi->f.addr = y->addr;
                fi->f.is_external = y->global && !st;
                fi->size = y->size;
            }
        }
    }
}
