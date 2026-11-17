/* 試験用の製品のソース（合成）: 番地を直接書いたレジスタと RAM の読み書き */
#include "reg.h"

void io_write_port(u1 v)
{
    P1 = v;
    P2_bit.b1 = 1;
    if (v & 1) P2_bit.b7 = 1;
}

void io_write_p3(u1 v)
{
    P3 = v;
}

u2 io_read_adc(void)
{
    return ADC_RAW;
}

/* 確保していない番地（0x10000 より下）を読む。実行した行だけが NG になることの確認用 */
u1 io_read_low(void)
{
    return *(volatile u1 *)0x0100;   /* この行番号（24）を tests/rl78/run_rl78_tests.bat が見ている */
}

u2 io_div(u2 a, u2 b)
{
    return (u2)(a / b);   /* この行番号（29）を tests/rl78/run_rl78_tests.bat が見ている */
}

u1 io_wait_flag(void)
{
    while ((P1 & 0x80) == 0) { }   /* この行番号（34）を tests/rl78/run_rl78_tests.bat が見ている */
    return 1;
}
