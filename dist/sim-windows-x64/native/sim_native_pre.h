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
 *   - int: 16 ビットの型に読み替える（下の注意を参照）
 *
 * int の読み替えの注意:
 *   - 変数、引数、戻り値、sizeof(int) は 16 ビットになる。
 *   - 計算の途中の桁はそろわない。C の規則で、short 同士の計算は PC の本当の int（32 ビット）に
 *     広げてから行われる。unsigned short の 40000 + 40000 を比較に使うと、マイコンでは 14464、
 *     PC では 80000 として比べられる。代入すれば 16 ビットに切り詰められて同じになる。
 *   - `short int`、`long int` のように int をほかの語と組み合わせた書き方は、コンパイルエラーになる
 *     （`unsigned int`、`signed int` は通る）。
 *   - 標準ヘッダー（stdint.h など）は、この読み替えのもとでは正しく読めない。
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

/* int を 16 ビットに読み替える。必ず最後に置く（これより上の行は本当の int で読ませる） */
#define int short

#endif
