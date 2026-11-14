/* cond_product.h - 品種別に同じ名前のマクロが違う番地になるヘッダ（Shift_JIS）
 * コンパイルスイッチ: PRODUCT（1 / 2 / その他）、SIM、COND_A、COND_B
 * 行末が「表」「ソ」「能」（2 バイト目が 0x5C）のコメントで、次の行がつながらないこと: 機能 */
#ifndef COND_PRODUCT_H
#define COND_PRODUCT_H

#include "cond_switch.h"
#include "sub/cond_types.h"
#include <stddef.h>
#include "no_such_header.h"

#if PRODUCT == 1
#define COND_REG (*(volatile cond_reg_t *)0xF960)  // 品種 1 の一覧表
#elif PRODUCT == 2
#define COND_REG (*(volatile cond_reg_t *)0xF962)  // 品種 2 のソ
#else
#define COND_REG (*(volatile cond_reg_t *)0xF964)  // その他の機能
#endif
#define COND_PAIR (*(volatile cond_pair_t *)0xF980)

#ifdef SIM
#define COND_SIM (*(volatile unsigned char *)0xF966)
#else
#define COND_SIM (*(volatile unsigned char *)0xF967)
#endif

#if defined(COND_A) && !defined(COND_B)
#define COND_AB (*(volatile unsigned char *)0xF968)
#else
#define COND_AB (*(volatile unsigned char *)0xF969)
#endif

/* 入れ子（# の後ろに空白のある書き方） */
#if PRODUCT >= 1
#  if PRODUCT == 1
#    define COND_NEST (*(volatile unsigned char *)0xF96A)
#  else
#    ifdef SIM
#      define COND_NEST (*(volatile unsigned char *)0xF96B)
#    else
#      define COND_NEST (*(volatile unsigned char *)0xF96C)
#    endif
#  endif
#else
#  define COND_NEST (*(volatile unsigned char *)0xF96D)
#endif

/* 別のヘッダ（cond_switch.h）で定義したスイッチを #include 経由で参照する */
#if COND_BOARD == 3 && defined(COND_HAS_SIM)
#define COND_INC (*(volatile unsigned char *)0xF96E)
#elif COND_BOARD == 3
#define COND_INC (*(volatile unsigned char *)0xF96F)
#else
#define COND_INC (*(volatile unsigned char *)0xF970)
#endif

/* 判定不能な式（関数形式マクロの呼び出し）: すべての枝を読むので、内容の違う定義が 2 つ残る */
#if COND_FUNC(1) > 0
#define COND_UNKNOWN (*(volatile unsigned char *)0xF972)
#else
#define COND_UNKNOWN (*(volatile unsigned char *)0xF973)
#endif

/* どこにも定義のないスイッチ（指定漏れとして sim info に出る） */
#if COND_UNSET_SWITCH
#define COND_UNSET (*(volatile unsigned char *)0xF974)
#else
#define COND_UNSET (*(volatile unsigned char *)0xF975)
#endif

/* 0 除算は、短絡評価で選ばれない側なら判定不能にならない */
#if defined(COND_DIV) && (8 / COND_DIV) == 4
#define COND_SHORT (*(volatile unsigned char *)0xF976)
#else
#define COND_SHORT (*(volatile unsigned char *)0xF977)
#endif

/* コンパイラが暗黙に定義するマクロ（CA78K0R の lmf なら __K0R__） */
#ifdef __K0R__
#define COND_K0R (*(volatile unsigned char *)0xF97E)
#else
#define COND_K0R (*(volatile unsigned char *)0xF97F)
#endif

/* mtpj の定義マクロの値にある XML のエスケープ（(1<<4)） */
#if COND_SHIFT == 16
#define COND_ESC (*(volatile unsigned char *)0xF982)
#else
#define COND_ESC (*(volatile unsigned char *)0xF983)
#endif

#endif /* COND_PRODUCT_H */
