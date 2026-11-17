/* 試験用の製品のソース（合成）: 番地を直接書いたレジスタと RAM */
#ifndef REG_H
#define REG_H

#include "types.h"

#define P1      (*(volatile u1 *)0xFFF01)
#define P2_bit  (*(volatile typeBit *)0xFFF02)
#define ADC_RAW (*(volatile u2 *)0xFFE10)   /* RAM の番地を直接読む */

/* コンパイラが暗黙に定義するマクロで番地が変わる。DLL のコンパイルと、ヘッダーの番地の定義の読み取り
   （defines）が、同じ枝を選ぶことの確認用 */
#ifdef __CA78K0R__
#define P3      (*(volatile u1 *)0xFFF03)
#else
#define P3      (*(volatile u1 *)0xFFF04)
#endif

#endif
