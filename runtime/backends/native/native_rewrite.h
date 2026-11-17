/* native_rewrite.h - プリプロセス済みの製品ソースの書き直し */
#ifndef NATIVE_REWRITE_H
#define NATIVE_REWRITE_H
#include "sim_util.h"

typedef struct {
    int int_replaced, int_dropped, short_added, asm_removed;
} native_rewrite_stats;

/* out に追記する。メモリ不足は sim_buf と同じ扱い。st は今回の数で上書きする。 */
int native_rewrite(const char *text, size_t len, sim_buf *out, native_rewrite_stats *st);
#endif
