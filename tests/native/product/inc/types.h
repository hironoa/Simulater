/* 試験用の製品のソース（合成）: 型の定義。CA78K0R の書き方をまねている */
#ifndef TYPES_H
#define TYPES_H

typedef unsigned char u1;
typedef unsigned int u2;        /* マイコンでは 16 ビット */
typedef signed int s2;
typedef unsigned long u4;

typedef struct {
    u1 b0 : 1;
    u1 b1 : 1;
    u1 b2 : 1;
    u1 b3 : 1;
    u1 b4 : 1;
    u1 b5 : 1;
    u1 b6 : 1;
    u1 b7 : 1;
} typeBit;

#endif
