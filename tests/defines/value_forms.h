typedef unsigned char u1;
typedef struct { unsigned char b0:1,b1:1,b2:1,b3:1,b4:1,b5:1,b6:1,b7:1; } typeBit;
typedef struct { unsigned char pad; unsigned short words[3]; } RegBlock;
#define BASE 0xF900
#define RAW_BASE 0xF900 + 2
#define df_unit (*((volatile unsigned char*)0xFF91C))
#define u1g_reg_ckc (*((volatile u1*)0xFFFA4))
#define Fs_ckc_mcm0 (((volatile typeBit*)0xFFFA4)->b4)
#define GOOD_ADDR (*(unsigned short*)(BASE + 2))
#define GOOD_INDEX ((unsigned short*)0xF900)[2]
#define GOOD_MEMBER (((*(RegBlock*)0xF920)).words)[1]
#define GOOD_WRAP (*((((unsigned short*)(BASE)))) )
#define GOOD_MACRO (*(unsigned short*)RAW_BASE)
#define READ_ONLY (*(const u1*)0xF940)
typedef const u1 ConstByte;
#define READ_ALIAS (*(ConstByte*)0xF941)
#define POINTER_CONST (*(u1* const)0xF942)
#define MASK_VALUE (*(unsigned short*)0xF900 & 0xFF)
#define ADD_VALUE (*(unsigned short*)0xF900 + 1)
#define SHIFT_VALUE ((*(volatile u1*)0xFFFA4) >> 4 & 1)
#define BOOL_VALUE (((*(u1*)0xFFFA4) & 0x10) != 0)
#define EQ_VALUE (*(u1*)0xF900 == 0)
#define COND_VALUE (*(u1*)0xF900 ? 1 : 0)
#define COMMA_VALUE (*(u1*)0xF900, 1)
#define ASSIGN_VALUE (*(u1*)0xF900 = 1)
#define CALL_VALUE (*(u1*)0xF900)()
#define NOT_VALUE !(*(u1*)0xF900)
#define TILDE_VALUE ~(*(u1*)0xF900)
#define NEG_VALUE -(*(u1*)0xF900)
#define ADDRESS_VALUE &(*(u1*)0xF900)
#define SIZE_VALUE sizeof(*(u1*)0xF900)
#define CAST_VALUE (unsigned char)(*(u1*)0xF900)
#define PTR_ADD (*((unsigned short*)0xF900 + 1))
#define PTR_RAW (*(unsigned short*)0xF900 + 1)
#define UNKNOWN_TYPE (*(Mystery*)0xF900)
#define DOUBLE_CAST (*(u1*)(unsigned long)0xF900)
#define DUP_LVALUE (*(u1*)0xF950)
#define DUP_LVALUE (*(u1*)0xF950 & 1)
#define DUP_VALUE (*(u1*)0xF951 & 1)
#define DUP_VALUE (*(u1*)0xF951)

#define PURE_PRODUCT (BASE * 2)
#define PURE_NUMBER (2 * 3)
