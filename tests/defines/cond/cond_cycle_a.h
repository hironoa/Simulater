/* 循環するインクルード（ガードなし）。打ち切って止まらないこと */
#include "cond_cycle_b.h"
#define COND_CYCLE (*(volatile unsigned char *)0xF97C)
