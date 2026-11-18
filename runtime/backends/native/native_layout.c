/* native_layout.c - ロードモジュールの変数と、DLL のデバッグ情報の変数を名前で対応づける */
#include "native_layout.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sim_util.h"

#define STRENGTH_GLOBAL INT_MAX   /* どちらもグローバル */

static int same_func(const char *a, const char *b)
{
    if (!a || !b) return !a && !b;
    return !strcmp(a, b);
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

static int same_char(char a, char b)
{
#ifdef _WIN32
    a = (char)tolower((unsigned char)a);
    b = (char)tolower((unsigned char)b);
#endif
    return a == b;
}

int native_layout_common_tail(const char *a, const char *b)
{
    size_t i, j;
    int n = 0;
    if (!a || !b || !*a || !*b) return 0;
    i = strlen(a);
    j = strlen(b);
    /* 末尾の区切りは飛ばす */
    while (i && is_sep(a[i - 1])) i--;
    while (j && is_sep(b[j - 1])) j--;
    while (i && j) {
        size_t ei = i, ej = j;
        while (i && !is_sep(a[i - 1])) i--;
        while (j && !is_sep(b[j - 1])) j--;
        if (ei - i != ej - j || ei == i) break;
        {
            size_t k;
            for (k = 0; k < ei - i; k++) if (!same_char(a[i + k], b[j + k])) return n;
        }
        n++;
        while (i && is_sep(a[i - 1])) i--;
        while (j && is_sep(b[j - 1])) j--;
    }
    return n;
}

static const native_layout_var *g_sort_base;   /* qsort の比較に添字の元を渡す */

static int index_cmp(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    int c = strcmp(g_sort_base[x].name, g_sort_base[y].name);
    if (c) return c;
    return x < y ? -1 : x > y;
}

typedef struct { long lm, dll; int strength; } pair_t;

static int pair_cmp(const void *a, const void *b)
{
    const pair_t *x = a, *y = b;
    if (x->strength != y->strength) return x->strength > y->strength ? -1 : 1;   /* 強い順 */
    if (x->lm != y->lm) return x->lm < y->lm ? -1 : 1;
    return x->dll < y->dll ? -1 : x->dll > y->dll;
}

int native_layout_match(const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll, long *match, unsigned char *dll_state)
{
    long *order = sim_xcalloc(ndll ? ndll : 1, sizeof *order);
    unsigned char *done = sim_xcalloc(nlm ? nlm : 1, 1);        /* 決まった（対応づけた、またはあいまい）lm の変数 */
    int *cnt_lm = sim_xcalloc(nlm ? nlm : 1, sizeof *cnt_lm), *cnt_dll = sim_xcalloc(ndll ? ndll : 1, sizeof *cnt_dll);
    pair_t *pairs = NULL;
    size_t npairs = 0, cap = 0, i, g, e, k;
    memset(dll_state, NATIVE_LAYOUT_DLL_UNUSED, ndll);
    for (i = 0; i < ndll; i++) order[i] = (long)i;
    g_sort_base = dll;
    qsort(order, ndll, sizeof *order, index_cmp);
    g_sort_base = NULL;
    /* 候補の組（名前と関数名が同じもの）と、その強さ */
    for (i = 0; i < nlm; i++) {
        const native_layout_var *v = &lm[i];
        size_t lo = 0, hi = ndll;
        match[i] = NATIVE_LAYOUT_NONE;
        if (!v->name) continue;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (strcmp(dll[order[mid]].name, v->name) < 0) lo = mid + 1; else hi = mid;
        }
        for (k = lo; k < ndll && !strcmp(dll[order[k]].name, v->name); k++) {
            const native_layout_var *c = &dll[order[k]];
            int s;
            if (!same_func(v->func, c->func)) continue;
            s = !v->func && v->is_external && c->is_external ? STRENGTH_GLOBAL : native_layout_common_tail(v->file, c->file);
            if (s <= 0) continue;
            if (npairs == cap) {
                cap = cap ? cap * 2 : 64;
                pairs = sim_xrealloc(pairs, cap * sizeof *pairs);
            }
            pairs[npairs].lm = (long)i;
            pairs[npairs].dll = order[k];
            pairs[npairs].strength = s;
            npairs++;
        }
    }
    qsort(pairs, npairs, sizeof *pairs, pair_cmp);
    /* 同じ強さの組ごとに、まだ決まっていない lm の変数と、まだ使っていない dll の変数の組だけを見る */
    for (g = 0; g < npairs; g = e) {
        for (e = g; e < npairs && pairs[e].strength == pairs[g].strength; e++) {
            const pair_t *p = &pairs[e];
            if (done[p->lm] || dll_state[p->dll]) continue;
            cnt_lm[p->lm]++;
            cnt_dll[p->dll]++;
        }
        for (k = g; k < e; k++) {
            const pair_t *p = &pairs[k];
            if (done[p->lm] || dll_state[p->dll]) continue;
            if (cnt_lm[p->lm] == 1 && cnt_dll[p->dll] == 1)
                match[p->lm] = p->dll;
            else
                match[p->lm] = NATIVE_LAYOUT_AMBIGUOUS;
        }
        /* 決めた印は、同じ強さの組をすべて見てから付ける（並びに左右されないように） */
        for (k = g; k < e; k++) {
            const pair_t *p = &pairs[k];
            if (!cnt_lm[p->lm] || !cnt_dll[p->dll]) continue;
            done[p->lm] = 1;
            dll_state[p->dll] = match[p->lm] == p->dll ? NATIVE_LAYOUT_DLL_MATCHED : NATIVE_LAYOUT_DLL_AMBIGUOUS;
        }
        for (k = g; k < e; k++) {
            cnt_lm[pairs[k].lm] = 0;
            cnt_dll[pairs[k].dll] = 0;
        }
    }
    free(pairs);
    free(order);
    free(done);
    free(cnt_lm);
    free(cnt_dll);
    return 0;
}
