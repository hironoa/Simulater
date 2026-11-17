/* 試験用の製品ソース（合成）: 古い型の書き方とアセンブラ */
#include "types.h"

long int legacy_long = 70000;
unsigned long int legacy_ulong = 70000;
short int legacy_short = 1;
unsigned legacy_unsigned = 2;
signed legacy_signed = -1;

__asm("\textrn\t_legacy_asm_label");

unsigned legacy_sizes(void)
{
    return sizeof(int) + sizeof(unsigned) * 10 + sizeof(long int) * 100 + sizeof(short int) * 1000;
}

signed legacy_asm(void)
{
    signed value = 6;
    __asm("\tdi");
    value++;
    __asm












    ("\tdi");
    __asm("\tei");
    return value;
}

u4 legacy_sum(void)
{
    unsigned long int a = 70000;
    unsigned long int b = 70000;
    return a + b;
}
