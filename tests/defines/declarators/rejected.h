typedef unsigned char u1;
typedef unsigned char *PU8;
typedef u1 __far *FP;
typedef void (*FUNC)(void);
typedef unsigned char MultiU1, *MultiPU1, MultiA4[4];
typedef long double Bad_LONG_DOUBLE_TYPEDEF;
#define DECLBADLONGDOUBLETYPEDEF (*(Bad_LONG_DOUBLE_TYPEDEF*)0xF800)
typedef struct { unsigned char before; long double value; unsigned char after; } Bad_LONG_DOUBLE_MEMBER;
#define DECLBADLONGDOUBLEMEMBER (((Bad_LONG_DOUBLE_MEMBER*)0xF800)->before)
#define DECLBADLONGDOUBLECAST (*(long double*)0xF800)
typedef bool Bad_BOOL_TYPEDEF;
#define DECLBADBOOLTYPEDEF (*(Bad_BOOL_TYPEDEF*)0xF800)
typedef struct { unsigned char before; bool value; unsigned char after; } Bad_BOOL_MEMBER;
#define DECLBADBOOLMEMBER (((Bad_BOOL_MEMBER*)0xF800)->before)
#define DECLBADBOOLCAST (*(bool*)0xF800)
typedef _Bool Bad_CBOOL_TYPEDEF;
#define DECLBADCBOOLTYPEDEF (*(Bad_CBOOL_TYPEDEF*)0xF800)
typedef struct { unsigned char before; _Bool value; unsigned char after; } Bad_CBOOL_MEMBER;
#define DECLBADCBOOLMEMBER (((Bad_CBOOL_MEMBER*)0xF800)->before)
#define DECLBADCBOOLCAST (*(_Bool*)0xF800)
typedef void Bad_VOID_TYPEDEF;
#define DECLBADVOIDTYPEDEF (*(Bad_VOID_TYPEDEF*)0xF800)
typedef struct { unsigned char before; void value; unsigned char after; } Bad_VOID_MEMBER;
#define DECLBADVOIDMEMBER (((Bad_VOID_MEMBER*)0xF800)->before)
#define DECLBADVOIDCAST (*(void*)0xF800)
#define DECLBADPU8 (*(PU8*)0xF800)
#define DECLBADFP (*(FP*)0xF800)
#define DECLBADFUNC (*(FUNC*)0xF800)
#define DECLBADMULTIPTR (*(MultiPU1*)0xF800)
typedef unsigned char BadArrayEMPTY[];
#define DECLBADARRAYEMPTY (*(BadArrayEMPTY*)0xF800)
typedef unsigned char BadArrayUNKNOWN[UNKNOWN_SIZE];
#define DECLBADARRAYUNKNOWN (*(BadArrayUNKNOWN*)0xF800)
typedef unsigned char BadArrayZERO[0];
#define DECLBADARRAYZERO (*(BadArrayZERO*)0xF800)
typedef unsigned char BadArrayMULTI_UNKNOWN[2][UNKNOWN_SIZE];
#define DECLBADARRAYMULTIUNKNOWN (*(BadArrayMULTI_UNKNOWN*)0xF800)
typedef struct { unsigned char before; unsigned char *p; unsigned char after; } BadRecordPTR;
#define DECLBADRECORDPTRbefore (((BadRecordPTR*)0xF800)->before)
#define DECLBADRECORDPTRafter (((BadRecordPTR*)0xF800)->after)
#define DECLBADRECORDPTRb0 (((BadRecordPTR*)0xF800)->b0)
typedef struct { unsigned char before; unsigned char a, b[2], *p; unsigned char after; } BadRecordMIXED;
#define DECLBADRECORDMIXEDbefore (((BadRecordMIXED*)0xF800)->before)
#define DECLBADRECORDMIXEDafter (((BadRecordMIXED*)0xF800)->after)
#define DECLBADRECORDMIXEDb0 (((BadRecordMIXED*)0xF800)->b0)
typedef struct { unsigned char before; void (*f)(void); unsigned char after; } BadRecordFUNCTION;
#define DECLBADRECORDFUNCTIONbefore (((BadRecordFUNCTION*)0xF800)->before)
#define DECLBADRECORDFUNCTIONafter (((BadRecordFUNCTION*)0xF800)->after)
#define DECLBADRECORDFUNCTIONb0 (((BadRecordFUNCTION*)0xF800)->b0)
typedef struct { unsigned char before; enum E value; unsigned char after; } BadRecordENUM;
#define DECLBADRECORDENUMbefore (((BadRecordENUM*)0xF800)->before)
#define DECLBADRECORDENUMafter (((BadRecordENUM*)0xF800)->after)
#define DECLBADRECORDENUMb0 (((BadRecordENUM*)0xF800)->b0)
typedef struct { unsigned char before; unsigned char values[]; unsigned char after; } BadRecordBAD_ARRAY;
#define DECLBADRECORDBADARRAYbefore (((BadRecordBAD_ARRAY*)0xF800)->before)
#define DECLBADRECORDBADARRAYafter (((BadRecordBAD_ARRAY*)0xF800)->after)
#define DECLBADRECORDBADARRAYb0 (((BadRecordBAD_ARRAY*)0xF800)->b0)
typedef struct { unsigned char before; struct { unknown value; } nested; unsigned char after; } BadRecordNESTED;
#define DECLBADRECORDNESTEDbefore (((BadRecordNESTED*)0xF800)->before)
#define DECLBADRECORDNESTEDafter (((BadRecordNESTED*)0xF800)->after)
#define DECLBADRECORDNESTEDb0 (((BadRecordNESTED*)0xF800)->b0)
enum E { E0, E1=1000 };
#define DECLBADINDEX ((u1*)0xF800)[2]
#define DECLBADARRAYPTR (*(u1(*)[4])0xF800)
#define DECLBADDOUBLECAST (*(volatile u1*)(unsigned long)0xF800)
#define DECLBADNEGATIVE (*(u1*)-1)
#define DECLBADTOOHIGH (*(u1*)0x100000)
#define DECLBADEND (*(unsigned short*)0xFFFFF)
#define DECLBADDOUBLEPTR (*(u1**)0xF800)
#define DECLVOIDPTR ((void*)0xF800)
typedef u1 __near *NP;
#define DECLBADNEARPTR (*(NP*)0xF800)
typedef struct { unsigned char before; u1 __far *p; unsigned char after; } FarMember;
#define DECLBADFARMEMBER (((FarMember*)0xF800)->before)
typedef struct { unsigned char before; u1 __near *p; unsigned char after; } NearMember;
#define DECLBADNEARMEMBER (((NearMember*)0xF800)->after)
#define DECLBADENUMCAST (*(enum E*)0xF800)
#define DECLBADVOIDQUALCAST (*(void const*)0xF800)
#define DECLBADINLINE (((struct { unsigned char before; u1 *p; }*)0xF800)->b0)
