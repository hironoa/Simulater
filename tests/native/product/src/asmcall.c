/* アセンブラにあると想定した関数と変数を使う試験（native では sim が代わりを作る） */
#include "types.h"

extern void start_asm(void);
extern u2 asm_get_value(u1 n);
extern u1 asm_table[8];
extern void asm_handler(void);      /* アドレスを表に入れるだけ（呼ぶのは表から） */
extern u2 asm_word;                 /* 大きさの分かる変数（2 バイト） */

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

u2 asmcall_implicit(void)
{
    asm_implicit(1);                /* 宣言せずに呼ぶ（gnu89 の暗黙の宣言） */
    return 9;
}
