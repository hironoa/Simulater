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

/* 確保していない番地（0x10000 より下）を読む。実行するとエラーで止まることの確認用 */
u1 io_read_low(void)
{
    return *(volatile u1 *)0x0100;
}
