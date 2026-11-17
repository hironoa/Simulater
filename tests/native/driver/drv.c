/* 試験用のドライバ（合成）: 製品とは別のフォルダで管理し、sim.cfg の native_src で足す */
#include "types.h"
#include "drv_reg.h"

extern int calc_add(int a, int b);

int DRV_add3(int a, int b, int c)
{
    return calc_add(calc_add(a, b), c);
}

void DRV_set_port(unsigned char v)
{
    DRV_PORT = v;
}
