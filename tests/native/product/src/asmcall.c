/* アセンブラにあると想定した関数と変数を使う試験（native では sim が代わりを作る） */
#include "types.h"

extern void start_asm(void);
extern u2 asm_get_value(u1 n);
extern u1 asm_table[8];
extern void asm_handler(void);      /* アドレスを表に入れるだけ（呼ぶのは表から） */
extern u2 asm_word;                 /* 大きさの分かる変数（2 バイト） */
extern double asm_dbl(double x);    /* 浮動小数を返す（Windows x64 では XMM0 で返る） */
extern float asm_flt(float x);
extern u1 asm_a_flag;               /* 1 バイトの変数（名前順で配列の前に置かれ、配列の位置をずらす） */
extern u1 asm_buf_a[16];            /* 最適化すると 16 バイト境界を前提にしたコピーになりうる */
extern u1 asm_buf_b[16];

static void (*const asmcall_tbl[1])(void) = { asm_handler };

u2 asmcall_run(u1 n)
{
    start_asm();
    return (u2)(asm_get_value(n) + asm_table[n & 7] + 100);
}

u2 asmcall_tbl_run(void)
{
    asmcall_tbl[0]();
    return 7;
}

u2 asmcall_word(void)
{
    return asm_word;
}

u2 asmcall_fp(void)
{
    /* 代わりが 0.0 を返せば 1。引数がそのまま戻り値に見えると 0 */
    return (u2)(asm_dbl(3.5) == 0.0 && asm_flt(2.5f) == 0.0f);
}

u2 asmcall_copy(void)
{
    u1 i;
    for (i = 0; i < 16; i++) asm_buf_b[i] = asm_buf_a[i];
    return (u2)(asm_a_flag + asm_buf_b[15] + 3);
}

u2 asmcall_implicit(void)
{
    asm_implicit(1);                /* 宣言せずに呼ぶ（gnu89 の暗黙の宣言） */
    return 9;
}
