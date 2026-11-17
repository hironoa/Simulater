/*
 * clear_drv.h - 試験用: テストドライバの側で、番地を直接書いて #define で定義したデータ
 *
 * sim は native_src のソースが読むヘッダーの定義を自動で読むので、CSV に名前で書ける。
 * 0xFE020〜0xFE04F には probe の変数がない（probe の RAM の変数は 0xFBF00〜0xFC113 と 0xFFE20〜）。
 * TEST_clear_drv は 0xFE020〜0xFE02F をクリアする。
 */
#ifndef CLEAR_DRV_H
#define CLEAR_DRV_H

#define DRV_DIRECT_IN  (*(volatile unsigned char *)0xFE020)    /* 範囲の中 */
#define DRV_NEAR_IN    (*(volatile unsigned char *)0xE022)     /* 16 ビットの near の形。0xFE022 として範囲の中 */
#define DRV_DIRECT_OUT (*(volatile unsigned short *)0xFE040)   /* 範囲の外 */

#endif
