#ifdef DUP_SEL
#define DUP_X (*(volatile unsigned char *)0xF9E0)
#else
#define DUP_X (*(volatile unsigned char *)0xF9E1)
#endif
