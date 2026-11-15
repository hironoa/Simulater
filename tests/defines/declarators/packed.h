#pragma pack(push, 1)
typedef struct { unsigned char a; unsigned short b; } Packed;
#pragma pack(pop)
typedef struct __packed { unsigned char a; unsigned short b; } ExplicitPacked;
#define DECLPACKED (((Packed*)0xF800)->a)
#define DECLEXPLICITPACKED (*(ExplicitPacked*)0xF810)
