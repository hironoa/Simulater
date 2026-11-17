/* 試験用の製品のソース（合成）: 番地を直接書いたレジスタと RAM */
#ifndef REG_H
#define REG_H

#include "types.h"

#define P1      (*(volatile u1 *)0xFFF01)
#define P2_bit  (*(volatile typeBit *)0xFFF02)
#define ADC_RAW (*(volatile u2 *)0xFFE10)   /* RAM の番地を直接読む */

#endif
