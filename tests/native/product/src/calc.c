/* 試験用の製品のソース（合成）: 計算。CA78K0R 独自の語と、int が 16 ビットであることに頼った書き方を含む */
#include "types.h"

#pragma sfr
#pragma DI
#pragma EI
#pragma NOP

typedef struct {
    u1 a[40];
    u2 n;
} blk_t;

static u2 s_count;          /* 呼び出しをまたいで残る */
static blk_t s_blk;
sreg u1 g_mode;

/* 16 ビットで桁あふれする: 30000 + 30000 は -5536 */
int calc_add(int a, int b)
{
    return a + b;
}

u2 calc_size_of_int(void)
{
    return (u2)sizeof(int);
}

u2 calc_count(void)
{
    DI();
    s_count++;
    EI();
    return s_count;
}

/* GAIN は mtpj の定義マクロ */
u2 calc_gain(u2 x)
{
    return (u2)(x * GAIN);
}

u1 calc_bool(boolean on)
{
    bit f = on;
    NOP();
    g_mode = f ? 1 : 0;
    return g_mode;
}

/* 構造体の初期化と代入（コンパイラが memset / memcpy の呼び出しにする） */
u2 calc_blk(u2 v)
{
    blk_t t = {{0}, 0};
    t.a[39] = (u1)v;
    t.n = v;
    s_blk = t;
    return (u2)(s_blk.n + s_blk.a[39]);
}

u2 calc_sum(const u1 __far *p, u1 n)
{
    u2 s = 0;
    while (n--) s = (u2)(s + *p++);
    return s;
}
