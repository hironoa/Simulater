/* 周辺機能の待ち・例外と関数の置換の試験。 */
#include "types.h"

u1 periph_wait(void)
{
    while (!(*(volatile u1 *)0xFFF30 & 1)) {}
    return 1;
}

u2 periph_read(void)
{
    return *(volatile u2 *)0x0200;
}

u2 periph_task(u1 mode)
{
    if (mode == 0) return (u2)(periph_wait() + 10);
    return (u2)(periph_read() + 20);
}

static u2 periph_local(u2 x) { return (u2)(x * 2); }
u2 periph_calc(u2 x) { return (u2)(periph_local(x) + 1); }
u2 periph_value(void) { return 55; }
u2 periph_use_value(void) { return (u2)(periph_value() + 1); }
float periph_ratio(void) { return 1.5f; }
u2 periph_use_ratio(void) { return (u2)(periph_ratio() * 100); }
