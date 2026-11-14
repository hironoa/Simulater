typedef unsigned char u1;
typedef u1 alias_u1;
typedef struct { u1 b0:1; u1 b1:1; u1 b2:1; u1 b3:1; u1 b4:1; u1 b5:1; u1 b6:1; u1 b7:1; } typeBit;
typedef struct { u1 lead; u1 arr[3]; union { u1 c; unsigned short w; } u; } nestedType;
typedef struct { u1 a:4; unsigned short b:3; u1 c:1; } Mixed;
