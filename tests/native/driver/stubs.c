/* 製品とは別に管理する置換先。 */
#include "types.h"

u1 AMSTB_periph_wait(void) { return 7; }
u2 AMSTB_periph_read(void) { return 100; }
u2 AMSTB_periph_local(u2 x) { return (u2)(x + 1000); }
u1 STUB_wait_alt(void) { return 3; }
unsigned short AMSTB_asm_get_value(unsigned char n) { return (unsigned short)(n + 500); }
