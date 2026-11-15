#include "cond_pick.h"
#ifdef COND_PICKED_FAR
#define COND_FAR_OK (*(volatile unsigned char *)0xF97A)
#else
#define COND_FAR_OK (*(volatile unsigned char *)0xF97B)
#endif
