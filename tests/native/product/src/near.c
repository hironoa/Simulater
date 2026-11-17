/* 試験用の製品のソース（合成）: 16 ビットに切り詰めた番地を near のポインタにする書き方（hironoa/Simulater#27）。
 * RL78 では near のデータのポインタは 16 ビットで、実際の番地は 0xF0000 を補ったものになる。 */
#include "types.h"

typedef volatile unsigned int vuint16;   /* 16 ビット（int は 16 ビットに読み替えられる） */
#define kBaseAdr   0x0FFFD4u
typedef volatile struct { volatile vuint16 Int; } tC;
typedef tC __near *Ptr;
#define ADDR(val)  ((vuint16)(val))
#define C          ((Ptr) ADDR(kBaseAdr))

u2 near_read(void) { return C->Int; }
void near_write(u2 v) { C->Int = v; }

/* 構造体のメンバの位置を使う形（0x0F0300 を 16 ビットにして、0x2C の位置のメンバ。番地は 0xF032C） */
#define kBlkAdr    0x0F0300u
typedef volatile struct { volatile u1 pad[0x2C]; volatile vuint16 Reg; } tBlk;
typedef tBlk __near *BlkPtr;
#define BLK        ((BlkPtr) ADDR(kBlkAdr))

u2 near_member_read(void) { return BLK->Reg; }
void near_member_write(u2 v) { BLK->Reg = v; }

/* 番地を 16 ビットの表に入れて、実行時に使う形 */
static const u2 near_tbl[2] = { 0xFFD4u, 0xFE10u };
u2 near_tbl_read(u1 i) { return *(volatile u2 *)near_tbl[i]; }

/* 0 は NULL のまま */
u1 near_is_null(u2 v) { u1 *p = (u1 *)v; return p == 0; }

/* 本物のポインタを 64 ビットの整数に入れて戻しても変わらない */
static u2 near_value = 0x1234u;
u2 near_roundtrip(void)
{
    unsigned long long a = (unsigned long long)&near_value;
    return *(u2 *)a;
}
