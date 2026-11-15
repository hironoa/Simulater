#include "cond_pick.h"
#ifdef COND_PICKED_NEAR
#define COND_NEAR_OK (*(volatile unsigned char *)0xF978)
#else
#define COND_NEAR_OK (*(volatile unsigned char *)0xF979)
#endif
