/* #if の中で関数形式のマクロを呼ぶ形（引数を取るマクロの本体が演算） */
#define PPFN_BASE 1000
#define PPFN_ADD  24
#define PPFN_DIV  2
#define PPFN_CNT(ch)                 (((PPFN_BASE              +PPFN_ADD)-1)/PPFN_DIV)

#if ( PPFN_CNT(0) > 255 )
#define PPFN_BIG (*(volatile unsigned char *)0xF9F0)
#else
#include "ppfn_no_such.h"       /* 無効な枝。見つからない #include に出ない */
#define PPFN_SMALL (*(volatile unsigned char *)0xF9F1)
#endif

/* 展開できない形（## を使う）は、今までどおり判定不能で、両方の枝を読む */
#define PPFN_CAT(a, b) a ## b
#if PPFN_CAT(1, 2) > 5
#define PPFN_CAT_T (*(volatile unsigned char *)0xF9F2)
#else
#define PPFN_CAT_F (*(volatile unsigned char *)0xF9F3)
#endif
