/* clear_src.c - 試験用: native_src の変数を、対応の有無と範囲に応じてクリアする */

unsigned int g_bss_ui;               /* probe のロードモジュールの 0xFC030 に対応する */
unsigned int drv_unmatched;          /* ロードモジュールにないので毎回クリアする */
const unsigned int drv_const = 13;  /* ロードモジュールにない読み取り専用の変数 */

void TEST_clear_src(unsigned int in_range)
{
    if (in_range)
        sim_native_clear_range_incl(0xFC030, 0xFC031);
    else
        sim_native_clear_range(0xFE000, 0xFE001);
}
