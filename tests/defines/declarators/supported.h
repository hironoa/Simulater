typedef unsigned T_DECLUNSIGNEDTYPEDEF;
#define DECLUNSIGNEDTYPEDEF (*(T_DECLUNSIGNEDTYPEDEF*)0xF500)
typedef struct { unsigned value; } T_DECLUNSIGNEDMEMBER;
#define DECLUNSIGNEDMEMBER (((T_DECLUNSIGNEDMEMBER*)0xF504)->value)
#define DECLUNSIGNEDCAST (*(unsigned*)0xF508)
typedef signed T_DECLSIGNEDTYPEDEF;
#define DECLSIGNEDTYPEDEF (*(T_DECLSIGNEDTYPEDEF*)0xF50C)
typedef struct { signed value; } T_DECLSIGNEDMEMBER;
#define DECLSIGNEDMEMBER (((T_DECLSIGNEDMEMBER*)0xF510)->value)
#define DECLSIGNEDCAST (*(signed*)0xF514)
typedef short int T_DECLSHORTTYPEDEF;
#define DECLSHORTTYPEDEF (*(T_DECLSHORTTYPEDEF*)0xF518)
typedef struct { short int value; } T_DECLSHORTMEMBER;
#define DECLSHORTMEMBER (((T_DECLSHORTMEMBER*)0xF51C)->value)
#define DECLSHORTCAST (*(short int*)0xF520)
typedef unsigned short int T_DECLUSHORTTYPEDEF;
#define DECLUSHORTTYPEDEF (*(T_DECLUSHORTTYPEDEF*)0xF524)
typedef struct { unsigned short int value; } T_DECLUSHORTMEMBER;
#define DECLUSHORTMEMBER (((T_DECLUSHORTMEMBER*)0xF528)->value)
#define DECLUSHORTCAST (*(unsigned short int*)0xF52C)
typedef long int T_DECLLONGTYPEDEF;
#define DECLLONGTYPEDEF (*(T_DECLLONGTYPEDEF*)0xF530)
typedef struct { long int value; } T_DECLLONGMEMBER;
#define DECLLONGMEMBER (((T_DECLLONGMEMBER*)0xF534)->value)
#define DECLLONGCAST (*(long int*)0xF538)
typedef long unsigned int T_DECLULONGTYPEDEF;
#define DECLULONGTYPEDEF (*(T_DECLULONGTYPEDEF*)0xF53C)
typedef struct { long unsigned int value; } T_DECLULONGMEMBER;
#define DECLULONGMEMBER (((T_DECLULONGMEMBER*)0xF540)->value)
#define DECLULONGCAST (*(long unsigned int*)0xF544)
typedef int unsigned T_DECLINTUNSIGNEDTYPEDEF;
#define DECLINTUNSIGNEDTYPEDEF (*(T_DECLINTUNSIGNEDTYPEDEF*)0xF548)
typedef struct { int unsigned value; } T_DECLINTUNSIGNEDMEMBER;
#define DECLINTUNSIGNEDMEMBER (((T_DECLINTUNSIGNEDMEMBER*)0xF54C)->value)
#define DECLINTUNSIGNEDCAST (*(int unsigned*)0xF550)
typedef signed char T_DECLSCHARTYPEDEF;
#define DECLSCHARTYPEDEF (*(T_DECLSCHARTYPEDEF*)0xF554)
typedef struct { signed char value; } T_DECLSCHARMEMBER;
#define DECLSCHARMEMBER (((T_DECLSCHARMEMBER*)0xF558)->value)
#define DECLSCHARCAST (*(signed char*)0xF55C)
typedef char signed T_DECLCHARSIGNEDTYPEDEF;
#define DECLCHARSIGNEDTYPEDEF (*(T_DECLCHARSIGNEDTYPEDEF*)0xF560)
typedef struct { char signed value; } T_DECLCHARSIGNEDMEMBER;
#define DECLCHARSIGNEDMEMBER (((T_DECLCHARSIGNEDMEMBER*)0xF564)->value)
#define DECLCHARSIGNEDCAST (*(char signed*)0xF568)
typedef const unsigned char T_DECLQUAL0TYPEDEF;
#define DECLQUAL0TYPEDEF (*(T_DECLQUAL0TYPEDEF*)0xF56C)
typedef struct { const unsigned char value; } T_DECLQUAL0MEMBER;
#define DECLQUAL0MEMBER (((T_DECLQUAL0MEMBER*)0xF570)->value)
#define DECLQUAL0CAST (*(const unsigned char*)0xF574)
typedef unsigned volatile char T_DECLQUAL1TYPEDEF;
#define DECLQUAL1TYPEDEF (*(T_DECLQUAL1TYPEDEF*)0xF578)
typedef struct { unsigned volatile char value; } T_DECLQUAL1MEMBER;
#define DECLQUAL1MEMBER (((T_DECLQUAL1MEMBER*)0xF57C)->value)
#define DECLQUAL1CAST (*(unsigned volatile char*)0xF580)
typedef unsigned char const T_DECLQUAL2TYPEDEF;
#define DECLQUAL2TYPEDEF (*(T_DECLQUAL2TYPEDEF*)0xF584)
typedef struct { unsigned char const value; } T_DECLQUAL2MEMBER;
#define DECLQUAL2MEMBER (((T_DECLQUAL2MEMBER*)0xF588)->value)
#define DECLQUAL2CAST (*(unsigned char const*)0xF58C)
typedef __near unsigned char T_DECLQUAL3TYPEDEF;
#define DECLQUAL3TYPEDEF (*(T_DECLQUAL3TYPEDEF*)0xF590)
typedef struct { __near unsigned char value; } T_DECLQUAL3MEMBER;
#define DECLQUAL3MEMBER (((T_DECLQUAL3MEMBER*)0xF594)->value)
#define DECLQUAL3CAST (*(__near unsigned char*)0xF598)
typedef unsigned __far char T_DECLQUAL4TYPEDEF;
#define DECLQUAL4TYPEDEF (*(T_DECLQUAL4TYPEDEF*)0xF59C)
typedef struct { unsigned __far char value; } T_DECLQUAL4MEMBER;
#define DECLQUAL4MEMBER (((T_DECLQUAL4MEMBER*)0xF5A0)->value)
#define DECLQUAL4CAST (*(unsigned __far char*)0xF5A4)
typedef unsigned char __sreg T_DECLQUAL5TYPEDEF;
#define DECLQUAL5TYPEDEF (*(T_DECLQUAL5TYPEDEF*)0xF5A8)
typedef struct { unsigned char __sreg value; } T_DECLQUAL5MEMBER;
#define DECLQUAL5MEMBER (((T_DECLQUAL5MEMBER*)0xF5AC)->value)
#define DECLQUAL5CAST (*(unsigned char __sreg*)0xF5B0)
typedef __saddr unsigned char T_DECLQUAL6TYPEDEF;
#define DECLQUAL6TYPEDEF (*(T_DECLQUAL6TYPEDEF*)0xF5B4)
typedef struct { __saddr unsigned char value; } T_DECLQUAL6MEMBER;
#define DECLQUAL6MEMBER (((T_DECLQUAL6MEMBER*)0xF5B8)->value)
#define DECLQUAL6CAST (*(__saddr unsigned char*)0xF5BC)
typedef unsigned char __near const T_DECLQUAL7TYPEDEF;
#define DECLQUAL7TYPEDEF (*(T_DECLQUAL7TYPEDEF*)0xF5C0)
typedef struct { unsigned char __near const value; } T_DECLQUAL7MEMBER;
#define DECLQUAL7MEMBER (((T_DECLQUAL7MEMBER*)0xF5C4)->value)
#define DECLQUAL7CAST (*(unsigned char __near const*)0xF5C8)
typedef const unsigned __sreg char volatile __saddr T_DECLQUAL8TYPEDEF;
#define DECLQUAL8TYPEDEF (*(T_DECLQUAL8TYPEDEF*)0xF5CC)
typedef struct { const unsigned __sreg char volatile __saddr value; } T_DECLQUAL8MEMBER;
#define DECLQUAL8MEMBER (((T_DECLQUAL8MEMBER*)0xF5D0)->value)
#define DECLQUAL8CAST (*(const unsigned __sreg char volatile __saddr*)0xF5D4)
#define COUNT 2
#define LENGTH (COUNT * 2)
typedef unsigned char MultiU1, *MultiPU1, MultiA4[LENGTH];
#define DECLMULTIU1 (*(MultiU1*)0xF700)
#define DECLMULTIARRAY (*(MultiA4*)0xF710)
typedef unsigned char ExpressionArray[(1 << COUNT) + 1];
#define DECLEXPRARRAY (*(ExpressionArray*)0xF720)
struct Separate { unsigned char a; unsigned short b; };
typedef struct { unsigned char a, b[2]; struct Separate separate; unsigned char matrix[2][3]; unsigned char tail; } Members;
#define DECLMEMBERS (*(Members*)0xF740)
typedef struct { unsigned char a:1; unsigned char :3; unsigned char b:2; unsigned char :0; unsigned char c:1; } Padding;
#define DECLPADDING (*(Padding*)0xF760)
typedef struct { struct { unsigned char x; } named; union { unsigned char y; unsigned short z; }; unsigned char tail; } Nested;
#define DECLNESTED (*(Nested*)0xF770)

typedef unsigned const char TDECLMODCONSTMIDTYPEDEF;
#define DECLMODCONSTMIDTYPEDEF (*(TDECLMODCONSTMIDTYPEDEF*)0xF000)
typedef struct { unsigned const char value; } TDECLMODCONSTMIDMEMBER;
#define DECLMODCONSTMIDMEMBER (((TDECLMODCONSTMIDMEMBER*)0xF004)->value)
#define DECLMODCONSTMIDCAST (*(unsigned const char*)0xF008)
typedef volatile unsigned char TDECLMODVOLATILEPRETYPEDEF;
#define DECLMODVOLATILEPRETYPEDEF (*(TDECLMODVOLATILEPRETYPEDEF*)0xF00C)
typedef struct { volatile unsigned char value; } TDECLMODVOLATILEPREMEMBER;
#define DECLMODVOLATILEPREMEMBER (((TDECLMODVOLATILEPREMEMBER*)0xF010)->value)
#define DECLMODVOLATILEPRECAST (*(volatile unsigned char*)0xF014)
typedef unsigned char volatile TDECLMODVOLATILEPOSTTYPEDEF;
#define DECLMODVOLATILEPOSTTYPEDEF (*(TDECLMODVOLATILEPOSTTYPEDEF*)0xF018)
typedef struct { unsigned char volatile value; } TDECLMODVOLATILEPOSTMEMBER;
#define DECLMODVOLATILEPOSTMEMBER (((TDECLMODVOLATILEPOSTMEMBER*)0xF01C)->value)
#define DECLMODVOLATILEPOSTCAST (*(unsigned char volatile*)0xF020)
typedef unsigned __near char TDECLMODNEARMIDTYPEDEF;
#define DECLMODNEARMIDTYPEDEF (*(TDECLMODNEARMIDTYPEDEF*)0xF024)
typedef struct { unsigned __near char value; } TDECLMODNEARMIDMEMBER;
#define DECLMODNEARMIDMEMBER (((TDECLMODNEARMIDMEMBER*)0xF028)->value)
#define DECLMODNEARMIDCAST (*(unsigned __near char*)0xF02C)
typedef unsigned char __near TDECLMODNEARPOSTTYPEDEF;
#define DECLMODNEARPOSTTYPEDEF (*(TDECLMODNEARPOSTTYPEDEF*)0xF030)
typedef struct { unsigned char __near value; } TDECLMODNEARPOSTMEMBER;
#define DECLMODNEARPOSTMEMBER (((TDECLMODNEARPOSTMEMBER*)0xF034)->value)
#define DECLMODNEARPOSTCAST (*(unsigned char __near*)0xF038)
typedef __far unsigned char TDECLMODFARPRETYPEDEF;
#define DECLMODFARPRETYPEDEF (*(TDECLMODFARPRETYPEDEF*)0xF03C)
typedef struct { __far unsigned char value; } TDECLMODFARPREMEMBER;
#define DECLMODFARPREMEMBER (((TDECLMODFARPREMEMBER*)0xF040)->value)
#define DECLMODFARPRECAST (*(__far unsigned char*)0xF044)
typedef unsigned char __far TDECLMODFARPOSTTYPEDEF;
#define DECLMODFARPOSTTYPEDEF (*(TDECLMODFARPOSTTYPEDEF*)0xF048)
typedef struct { unsigned char __far value; } TDECLMODFARPOSTMEMBER;
#define DECLMODFARPOSTMEMBER (((TDECLMODFARPOSTMEMBER*)0xF04C)->value)
#define DECLMODFARPOSTCAST (*(unsigned char __far*)0xF050)
typedef __sreg unsigned char TDECLMODSREGPRETYPEDEF;
#define DECLMODSREGPRETYPEDEF (*(TDECLMODSREGPRETYPEDEF*)0xF054)
typedef struct { __sreg unsigned char value; } TDECLMODSREGPREMEMBER;
#define DECLMODSREGPREMEMBER (((TDECLMODSREGPREMEMBER*)0xF058)->value)
#define DECLMODSREGPRECAST (*(__sreg unsigned char*)0xF05C)
typedef unsigned __sreg char TDECLMODSREGMIDTYPEDEF;
#define DECLMODSREGMIDTYPEDEF (*(TDECLMODSREGMIDTYPEDEF*)0xF060)
typedef struct { unsigned __sreg char value; } TDECLMODSREGMIDMEMBER;
#define DECLMODSREGMIDMEMBER (((TDECLMODSREGMIDMEMBER*)0xF064)->value)
#define DECLMODSREGMIDCAST (*(unsigned __sreg char*)0xF068)
typedef unsigned __saddr char TDECLMODSADDRMIDTYPEDEF;
#define DECLMODSADDRMIDTYPEDEF (*(TDECLMODSADDRMIDTYPEDEF*)0xF06C)
typedef struct { unsigned __saddr char value; } TDECLMODSADDRMIDMEMBER;
#define DECLMODSADDRMIDMEMBER (((TDECLMODSADDRMIDMEMBER*)0xF070)->value)
#define DECLMODSADDRMIDCAST (*(unsigned __saddr char*)0xF074)
typedef unsigned char __saddr TDECLMODSADDRPOSTTYPEDEF;
#define DECLMODSADDRPOSTTYPEDEF (*(TDECLMODSADDRPOSTTYPEDEF*)0xF078)
typedef struct { unsigned char __saddr value; } TDECLMODSADDRPOSTMEMBER;
#define DECLMODSADDRPOSTMEMBER (((TDECLMODSADDRPOSTMEMBER*)0xF07C)->value)
#define DECLMODSADDRPOSTCAST (*(unsigned char __saddr*)0xF080)
typedef struct { struct { unsigned char a; }; union { unsigned char b; unsigned short w; } named; unsigned char tail; } OtherNested;
#define DECLOTHERNESTED (*(OtherNested*)0xF780)
typedef struct { unsigned short : 3; unsigned char x; } WordPadding;
typedef struct { unsigned char lead; WordPadding inner; unsigned char tail; } NestedPadding;
#define DECLWORDPADDING (*(NestedPadding*)0xF790)
