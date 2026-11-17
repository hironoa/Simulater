/*
 * sim_native_rt.c - PC 用 DLL の方式（exec = native）で、製品のソースと一緒に DLL に入れる部品
 *
 * DLL は C ライブラリなしで作る（LLVM の配布物の clang と lld-link だけで作れるように）。コンパイラは
 * 構造体の代入や配列の初期化を memcpy / memset の呼び出しにすることがあるので、ここで用意する。
 * このファイルには sim_native_pre.h を差し込まない（int は本当の int）。
 */
typedef unsigned long long sim_size;

/* 浮動小数を使うオブジェクトが参照する印（MSVC の決まり） */
int _fltused = 0;

void *memcpy(void *dst, const void *src, sim_size n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, sim_size n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, sim_size n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, sim_size n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

/* 整数からポインタへの変換で、値が実行時に決まるもの（native_near.c が呼び出しを差し込む）。
 * 1〜0xFFFF は RL78 の near の番地なので 0xF0000 を補う。0（NULL）と 0x10000 以上はそのまま */
unsigned long long __sim_near(unsigned long long v)
{
    return (v - 1ULL < 0xFFFFULL) ? v + 0xF0000ULL : v;
}

/* マイコンの番地の範囲をクリアする（製品のアセンブラの「RAM の範囲をクリアする」関数の代わりから呼ぶ）。
 * 中身は sim.exe の側にある。sim は DLL を読み込んだあと、__sim_host_clear に自分の関数の番地を入れる
 * （空のままなら何もしない）。sim.exe の側は native_layout のロードモジュールで番地と変数を対応づけ、
 * 範囲にある C の変数と、0xF0000〜0xFFFFF の確保したメモリを 0 にする。
 * 1〜0xFFFF は near の番地として 0xF0000 を補う（native_near = off で 16 ビットの番地が届いたときのため） */
void (*__sim_host_clear)(unsigned long long first, unsigned long long end, int inclusive) = 0;

static unsigned long long sim_near_addr(unsigned long long v)
{
    return (v - 1ULL < 0xFFFFULL) ? v + 0xF0000ULL : v;
}

/* [first, end) をクリアする（end は含まない） */
void sim_native_clear_range(unsigned long long first, unsigned long long end)
{
    if (__sim_host_clear) __sim_host_clear(sim_near_addr(first), sim_near_addr(end), 0);
}

/* [first, last] をクリアする（last の番地の 1 バイトまで） */
void sim_native_clear_range_incl(unsigned long long first, unsigned long long last)
{
    if (__sim_host_clear) __sim_host_clear(sim_near_addr(first), sim_near_addr(last), 1);
}
