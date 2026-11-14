/* cond_user.c - cond_product.h を読む側。2 回目の #include はインクルードガードで読まれない */
#include "cond_product.h"
#include "COND_PRODUCT.H"

#define COND_LOCAL (*(volatile unsigned char *)0xF984)
