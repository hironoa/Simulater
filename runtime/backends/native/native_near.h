/* native_near.h - LLVM IR の整数からポインタへの変換（inttoptr）を、RL78 の near の番地の意味に読み替える */
#ifndef NATIVE_NEAR_H
#define NATIVE_NEAR_H
#include "sim_util.h"

#define NATIVE_NEAR_BASE 0xF0000u   /* near の番地（16 ビット）に補う上位 */

typedef struct {
    int const_replaced;   /* 定数の形（値が 1〜0xFFFF）を 0xF0000 台に書き換えた数 */
    int dyn_inserted;     /* 命令の形に、実行時の読み替え（@__sim_near の呼び出し）を差し込んだ数 */
    int kept;             /* 知らない形のため書き換えなかった inttoptr の数（ベクタ、値が定数式、など） */
} native_near_stats;

/* text は clang 18 が出す LLVM IR のテキスト（不透明ポインタ）。out に追記する。st は今回の数で上書きする。
 * 規則: 値 v が 1〜0xFFFF なら v + 0xF0000、0 と 0x10000 以上はそのまま。
 * 命令の形（値が実行時に決まる）は、i64 に広げて @__sim_near（sim_native_rt.c）を通す。 */
int native_near(const char *text, size_t len, sim_buf *out, native_near_stats *st);
#endif
