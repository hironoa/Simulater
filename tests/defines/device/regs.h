#define BASE 0xFF900UL
#define df_unit (*((volatile unsigned char*)(BASE + 0x1C)))
#define u1g_reg_ckc (*((volatile alias_u1*)0xFFFA4))
#define Fs_ckc_mcm0 (((volatile typeBit*)0xFFFA4)->b4)
#define Fs_ckc_css (((volatile typeBit*)0xFFFA4)->b6)
#define Guessed (((volatile MissingBits*)0xFFFA4)->bit3)
#define UNRESOLVED (*(Missing*)0xFF930)
/* SJIS trail backslash: •\ */
