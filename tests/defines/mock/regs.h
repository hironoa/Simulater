#define BASE 0xF900UL
#define df_unit (*((volatile unsigned char*)(BASE + 0x1C)))
#define u1g_reg_ckc (*((volatile alias_u1*)0xFFA4))
#define Fs_ckc_mcm0 (((volatile typeBit*)0xFFA4)->b4)
#define Fs_ckc_css (((volatile typeBit*)0xFFA4)->b6)
#define Guessed (((volatile MissingBits*)0xFFA4)->bit3)
#define UNRESOLVED (*(Missing*)0xF930)

#define NESTED (*((nestedType*)0xF940))
#define NESTED_BYTE (*(u1*)0xF943)
#define MIXED (*((Mixed*)0xF950))
#define MIXED_RAW (*(u1*)0xF952)
#define unsupported_index (((nestedType*)0xF940)->arr[2])
