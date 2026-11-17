/*
 * clear_drv.c - 試験用: マイコンの番地の範囲にある変数のクリア（sim_native_clear_range）
 *
 * TEST_ram_clear は、製品のアセンブラの「RAM の範囲をクリアする」関数の代わり（example/native_ram_clear.c の形）。
 * TEST_… の関数は、利用者の製品と同じく、番地を数字で書いて TEST_ram_clear を呼ぶ。
 * 番地は probe（tests/ca78k0r_probe/DefaultBuild/ca78k0r_probe.lmf）の変数の番地（sim list --abs で調べたもの）:
 *   0xFC02E g_bss_uc（1）、0xFC030 g_bss_ui（2）、0xFC032 g_bss_outer（34）、0xFC054 s_file_bss（1）、
 *   0xFC056 g_fn_tbl（8）、0xFC05E L0027（6）、0xFC064 f_static1@buf（4）
 *   0xFC088 g_outer（34）、0xFC0AA g_uni（4）、0xFC0AE g_bits8（1）、0xFC0B0 g_bits16（4）
 *   0xFC0BC g_mat2（6）、0xFC0C2 g_iarr（10）
 *   0xFC108 f_static1@cnt（2）、0xFC10A f_static2@cnt（2）、0xFC10C f_static2@keep（6）、0xFC112 f_static2@nested（1）
 *
 * このファイルの変数は、ロードモジュールにないので「DLL にあってロードモジュールにない変数」になり、
 * どの範囲をクリアしても触られない（probe.c と同じ名前の static と、同じ名前の関数の中の static も置く）。
 * クリアの対象は、製品のプロジェクト（mtpj）のソースの変数と、番地を直接書いたデータ（clear_drv.h の定義を含む）。
 */
#include "clear_drv.h"

/* 「RAM の範囲をクリアする」関数の代わり（終わりの番地を含まない） */
void TEST_ram_clear(unsigned short *first, unsigned short *last)
{
    sim_native_clear_range((unsigned long long)(unsigned long)first, (unsigned long long)(unsigned long)last);
}

/* 終わりの番地を含む版 */
void TEST_ram_clear_incl(unsigned short *first, unsigned short *last)
{
    sim_native_clear_range_incl((unsigned long long)(unsigned long)first, (unsigned long long)(unsigned long)last);
}

/* 電源 OFF: 0xFC030〜0xFC063（g_bss_ui から L0027 まで）。g_bss_uc と f_static1@buf は範囲の外 */
void TEST_power_off(void)
{
    TEST_ram_clear((unsigned short *)0xFC030, (unsigned short *)0xFC064);
}

/* 同じ範囲を 16 ビットの番地で書いたもの（native_near で 0xF0000 が補われる） */
void TEST_power_off_near(void)
{
    TEST_ram_clear((unsigned short *)0xC030, (unsigned short *)0xC064);
}

/* 配列の途中から始まり、配列の途中で終わる: g_mat2[1][0] 〜 g_iarr[1] */
void TEST_clear_arr(void)
{
    TEST_ram_clear((unsigned short *)0xFC0BF, (unsigned short *)0xFC0C6);
}

/* 境界をまたぐ変数の大きさがマイコンと PC で違う: g_outer の後ろ半分はクリアしない。g_uni、g_bits8 はクリアする */
void TEST_clear_cross(void)
{
    TEST_ram_clear((unsigned short *)0xFC0A0, (unsigned short *)0xFC0B0);
}

/* 番地を直接書いたデータ（どの変数もない 0xFE000〜0xFE00F） */
void TEST_clear_abs(void)
{
    TEST_ram_clear((unsigned short *)0xFE000, (unsigned short *)0xFE010);
}

/* 関数の中の static: 終わりの番地を含まない（f_static2@nested は残る） */
void TEST_clear_static(void)
{
    TEST_ram_clear((unsigned short *)0xFC10A, (unsigned short *)0xFC112);
}

/* 関数の中の static: 終わりの番地を含む（f_static2@nested も 0 になる） */
void TEST_clear_static_incl(void)
{
    TEST_ram_clear_incl((unsigned short *)0xFC10A, (unsigned short *)0xFC112);
}

/* RAM の全体（0xF0000〜0xFFFFF） */
void TEST_clear_all(void)
{
    TEST_ram_clear((unsigned short *)0xF0000, (unsigned short *)0x100000);
}

/* テストドライバの、番地を直接書いたデータ（clear_drv.h）: 0xFE020〜0xFE02F */
void TEST_clear_drv(void)
{
    TEST_ram_clear((unsigned short *)0xFE020, (unsigned short *)0xFE030);
}

/* 番地を直接書いたデータを C から読む（CSV の名前の読み書きと同じ場所であることを確かめる） */
unsigned short TEST_drv_direct_sum(void)
{
    return (unsigned short)(DRV_DIRECT_IN + DRV_NEAR_IN + DRV_DIRECT_OUT);
}

/* おかしな引数（先頭が終わりより後ろ）: 何もしない */
void TEST_clear_bad(void)
{
    TEST_ram_clear((unsigned short *)0xFC064, (unsigned short *)0xFC030);
}

/* ロードモジュールにない変数（probe.c と同じ名前のものを含む） */
unsigned int drv_counter;
static unsigned int s_file_ui;          /* probe.c にも同じ名前のファイル内 static がある */

static unsigned int f_static2(void)     /* probe.c にも同じ名前の関数がある（こちらは static） */
{
    static unsigned int cnt;            /* probe.c の f_static2@cnt と同じ名前 */
    return ++cnt;
}

unsigned int TEST_drv_touch(void)
{
    return f_static2() + s_file_ui + drv_counter;
}
