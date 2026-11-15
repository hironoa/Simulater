#define COUNT 1+2
#define NEST COUNT*2
typedef unsigned char Bytes[COUNT*2];
typedef struct { Bytes bytes; unsigned char tail; } Layout;
#define EXP_BYTES (*(Bytes*)0xF940)
#define EXP_TAIL (((Layout*)0xF950)->tail)
#define BASE 0xF900+0x10
#define EXP_ADDR (*(unsigned char*)(BASE*2))
#if COUNT*2 == 5
#define EXP_IF (*(unsigned char*)0xF960)
#endif
typedef unsigned char Nested[NEST];
#define EXP_NEST (*(Nested*)0xF970)
#define WIDTH 1+2
typedef struct { unsigned char a:WIDTH*2; unsigned char b:1; } Bits;
#define EXP_BIT (((Bits*)0xF980)->b)
#define HALF 1+
typedef unsigned char Partial[HALF 2*3];
#define EXP_PARTIAL (*(Partial*)0xF990)
#define OPEN (1+2
#define CLOSE 1+2)
typedef unsigned char Open[OPEN)*2];
typedef unsigned char Close[(CLOSE*2];
#define EXP_OPEN (*(Open*)0xF9A0)
#define EXP_CLOSE (*(Close*)0xF9B0)
