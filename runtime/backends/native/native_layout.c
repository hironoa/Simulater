/* native_layout.c - ロードモジュールの変数と、DLL のデバッグ情報の変数を名前で対応づける */
#include "native_layout.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sim_util.h"

static int same_func(const char *a, const char *b)
{
    if (!a || !b) return !a && !b;
    return !strcmp(a, b);
}

/* how = 0: 末尾の一致（どちらがどちらの末尾でもよい）、how = 1: ファイル名の部分の一致（大文字小文字を区別しない） */
static int same_file(const char *a, const char *b, int how)
{
    const char *x, *y;
    if (!a || !b || !*a || !*b) return 0;
    if (how == 0) return sim_path_suffix_match(a, b) || sim_path_suffix_match(b, a);
    for (x = sim_basename(a), y = sim_basename(b); *x && *y; x++, y++)
        if (tolower((unsigned char)*x) != tolower((unsigned char)*y)) return 0;
    return !*x && !*y;
}

static const native_layout_var *g_sort_base;   /* qsort の比較に添字の元を渡す */

static int index_cmp(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    int c = strcmp(g_sort_base[x].name, g_sort_base[y].name);
    if (c) return c;
    return x < y ? -1 : x > y;      /* 同じ名前は元の順（結果を決まったものにする） */
}

/* name の候補（dll の添字）の中から、v に当たるものを選ぶ。want_product が 1 なら製品のもの、0 なら native_src のもの */
static long pick(const native_layout_var *v, const native_layout_var *dll, const long *order, size_t first, size_t ndll,
                 const unsigned char *used, int want_product)
{
    int pass;
    size_t k;
    /* 0: どちらもグローバル、1: ファイル名の末尾が一致、2: ファイル名の部分が一致 */
    for (pass = 0; pass < 3; pass++) {
        for (k = first; k < ndll && !strcmp(dll[order[k]].name, v->name); k++) {
            const native_layout_var *c = &dll[order[k]];
            if (!c->product != !want_product || used[order[k]] || !same_func(v->func, c->func)) continue;
            if (pass == 0 ? (!v->func && v->is_external && c->is_external) : same_file(v->file, c->file, pass - 1))
                return order[k];
        }
    }
    return NATIVE_LAYOUT_NONE;
}

int native_layout_match(const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll, long *match)
{
    long *order = sim_xcalloc(ndll ? ndll : 1, sizeof *order);
    unsigned char *used = sim_xcalloc(ndll ? ndll : 1, 1);
    size_t i;
    for (i = 0; i < ndll; i++) order[i] = (long)i;
    g_sort_base = dll;
    qsort(order, ndll, sizeof *order, index_cmp);
    g_sort_base = NULL;
    for (i = 0; i < nlm; i++) {
        const native_layout_var *v = &lm[i];
        size_t lo = 0, hi = ndll;
        long hit;
        if (!v->name) { match[i] = NATIVE_LAYOUT_NONE; continue; }
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (strcmp(dll[order[mid]].name, v->name) < 0) lo = mid + 1; else hi = mid;
        }
        hit = pick(v, dll, order, lo, ndll, used, 1);
        if (hit >= 0) {
            used[hit] = 1;
            match[i] = hit;
        } else {
            match[i] = pick(v, dll, order, lo, ndll, used, 0) >= 0 ? NATIVE_LAYOUT_SRC : NATIVE_LAYOUT_NONE;
        }
    }
    free(order);
    free(used);
    return 0;
}
