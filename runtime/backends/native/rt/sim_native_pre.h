/*
 * sim_native_pre.h - PC 用 DLL の方式（exec = native）で、製品のソースの先頭に差し込むヘッダー
 *
 * sim が clang に -include で渡す。CA78K0R 独自の書き方を、PC 用のコンパイルで意味のない形に置き換える。
 * 製品のソースは書き換えない。
 *
 * ここで吸収するもの:
 *   - 置き場所や呼び出し方の指定（__far、__near、sreg、callt、norec など）: 空にする
 *   - boolean / bit: 1 バイトの型にする（マイコンでは 1 ビットだが、値は 0 / 1 で同じ）
 *   - 割り込みの許可・禁止などの組み込み関数（DI()、EI()、NOP() など）: 何もしない
 * ほかに、マイコンの番地の範囲をクリアする部品（sim_native_clear_range）を宣言する。
 *
 * int はここでは定義しない。プリプロセス後に sim が字句ごとに書き直して 16 ビットにする。
 * long int / short int は int を消し、unsigned / signed だけの宣言には short を補う。
 * __asm("…") もその段階で空にして数える。システムヘッダーは書き直さない。
 * 計算の途中の桁と、型を書かない宣言は PC の int（32 ビット）のままになる。
 */
#ifndef SIM_NATIVE_PRE_H
#define SIM_NATIVE_PRE_H

#define __SIM_NATIVE__ 1
#define __K0R__ 1
#define __CA78K0R__ 1

/* 置き場所や呼び出し方の指定 */
#define __far
#define __near
#define sreg
#define __sreg
#define callt
#define __callt
#define callf
#define __callf
#define norec
#define __leaf
#define noauto
#define __pascal
#define __directmap
#define __temp
#define __mxcall
#define __flash
#define __flashf
#define __interrupt
#define __interrupt_brk
#define __rtos_interrupt

/* 1 ビットの型 */
typedef unsigned char boolean;
typedef unsigned char __boolean;
typedef unsigned char bit;

/* 組み込み関数（#pragma DI、#pragma NOP などで使えるようになるもの） */
#define DI() ((void)0)
#define EI() ((void)0)
#define NOP() ((void)0)
#define HALT() ((void)0)
#define STOP() ((void)0)
#define BRK() ((void)0)

/* マイコンの番地の範囲をクリアする（sim_native_rt.c。製品のアセンブラの「RAM の範囲をクリアする」関数の代わりから呼ぶ）。
 * 番地は整数で渡す（ポインタは (unsigned long long)(unsigned long)p のように整数にする）。native_layout が要る。
 * 型は int の書き直しに関わらないよう unsigned long long で書く */
void sim_native_clear_range(unsigned long long first, unsigned long long end);          /* [first, end) */
void sim_native_clear_range_incl(unsigned long long first, unsigned long long last);    /* [first, last] */

#endif
