/*
 * probe.c - CA78K0R の lmf に入るデバッグ情報（型）を調べるための試験用ソース
 *
 * 製品のソースとは無関係の、形式調査だけを目的にしたファイルです。
 * CS+ で CA78K0R の新しいプロジェクトを作り、このファイル 1 つだけを追加してビルドしてください
 * （デバッグ情報つき。最適化の設定は製品のビルドと同じにしてください）。
 * できあがった DefaultBuild フォルダの .lmf / .map / .sym / .hex を送ってください。
 *
 * どのメンバがどこに置かれたかをメモリイメージから読み取れるよう、
 * 初期値はすべて違う値にしてあります。値を変えないでください。
 */

/* ---- 基本型 ---- */
char            g_c    = 0x11;
signed char     g_sc   = -0x12;
unsigned char   g_uc   = 0x13;
short           g_s    = 0x1415;
unsigned short  g_us   = 0x1617;
int             g_i    = 0x1819;
unsigned int    g_ui   = 0x1A1B;
long            g_l    = 0x1C1D1E1FL;
unsigned long   g_ul   = 0x20212223UL;
float           g_f    = 1.5f;
double          g_d    = 2.25;

/* ---- enum ---- */
enum small_e { SE_A, SE_B, SE_C = 100 };
enum neg_e   { NE_M = -3, NE_Z = 0, NE_P = 7 };
enum big_e   { BE_A, BE_B = 1000 };
enum small_e g_se = SE_C;
enum neg_e   g_ne = NE_M;
enum big_e   g_be = BE_B;

/* ---- 構造体、入れ子、配列 ---- */
struct inner {
    unsigned char  ia;      /* 0x31 */
    unsigned int   ib;      /* 0x3233 */
    unsigned char  ic;      /* 0x34 */
};

struct outer {
    unsigned char  oa;      /* 0x41 */
    struct inner   in;
    unsigned long  ol;      /* 0x42434445 */
    unsigned char  arr[3];  /* 0x46, 0x47, 0x48 */
    unsigned int   warr[2]; /* 0x494A, 0x4B4C */
    struct inner   ins[2];
    unsigned char  tail;    /* 0x4D */
};

struct outer g_outer = {
    0x41,
    { 0x31, 0x3233, 0x34 },
    0x42434445UL,
    { 0x46, 0x47, 0x48 },
    { 0x494A, 0x4B4C },
    { { 0x51, 0x5253, 0x54 }, { 0x55, 0x5657, 0x58 } },
    0x4D
};

struct outer g_outer_arr[2];

/* ---- 共用体 ---- */
union uni {
    unsigned long  ul;
    unsigned int   ui[2];
    unsigned char  uc[4];
    struct { unsigned char lo; unsigned char hi; unsigned int w; } parts;
};
union uni g_uni = { 0x61626364UL };

/* ---- ビットフィールド ---- */
struct bits8 {
    unsigned char b0 : 1;
    unsigned char b1 : 1;
    unsigned char n2 : 3;
    unsigned char n5 : 3;
};
struct bits16 {
    unsigned int f0  : 1;
    unsigned int f1  : 4;
    unsigned int f5  : 7;
    unsigned int f12 : 4;
    unsigned int g0  : 9;   /* 次の 16 ビットへ */
    unsigned int g9  : 3;
};
struct bits_mixed {
    unsigned char head;     /* 0x71 */
    unsigned char m0 : 2;
    unsigned char m2 : 6;
    unsigned int  w0 : 5;
    unsigned int  w5 : 11;
    signed int    s0 : 4;
    unsigned char tail;     /* 0x72 */
};
struct bits8      g_bits8  = { 1, 0, 5, 2 };
struct bits16     g_bits16 = { 1, 9, 0x55, 3, 0x1AA, 5 };
struct bits_mixed g_bitsm  = { 0x71, 2, 0x2A, 0x15, 0x3FF, -3, 0x72 };

/* ---- 多次元配列 ---- */
unsigned char g_mat2[2][3]     = { { 0x81, 0x82, 0x83 }, { 0x84, 0x85, 0x86 } };
unsigned int  g_mat3[2][3][4];
int           g_iarr[5]        = { 0x0101, 0x0202, 0x0303, 0x0404, 0x0505 };

/* ---- ポインタ ---- */
unsigned char         *g_p_uc    = &g_uc;
unsigned int          *g_p_ui    = &g_ui;
struct outer          *g_p_outer = &g_outer;
struct inner          *g_p_inner = &g_outer.in;
unsigned char        **g_pp_uc   = &g_p_uc;
const unsigned char   *g_p_const = &g_uc;
unsigned char __far   *g_fp_uc   = &g_uc;
struct outer  __far   *g_fp_outer = &g_outer;
unsigned char         *g_parr[3] = { &g_uc, &g_uc, &g_uc };
unsigned char        (*g_p_arr)[3] = &g_mat2[1];

/* ---- typedef ---- */
typedef unsigned char  U8;
typedef unsigned int   U16;
typedef unsigned long  U32;
typedef struct { U8 x; U16 y; U32 z; } POINT_T;
typedef POINT_T *POINT_P;
U8      g_td_u8  = 0x91;
U16     g_td_u16 = 0x9293;
U32     g_td_u32 = 0x94959697UL;
POINT_T g_td_pt  = { 0x98, 0x999A, 0x9B9C9D9EUL };
POINT_P g_td_pp  = &g_td_pt;

/* ---- const（ROM）と volatile ---- */
const unsigned char  c_tbl[4]  = { 0xA1, 0xA2, 0xA3, 0xA4 };
const unsigned int   c_wtbl[2] = { 0xA5A6, 0xA7A8 };
const struct inner   c_inner   = { 0xA9, 0xAAAB, 0xAC };
volatile unsigned char g_vol   = 0xAD;

/* ---- 初期値なし（bss） ---- */
unsigned char g_bss_uc;
unsigned int  g_bss_ui;
struct outer  g_bss_outer;

/* ---- ファイル内 static ---- */
static unsigned char  s_file_uc  = 0xB1;
static unsigned int   s_file_ui  = 0xB2B3;
static struct inner   s_file_in  = { 0xB4, 0xB5B6, 0xB7 };
static unsigned char  s_file_bss;

/* ---- 関数ポインタ ---- */
unsigned int add2(unsigned int a, unsigned int b);
unsigned int (*g_fn)(unsigned int, unsigned int) = add2;
void (*g_fn_tbl[2])(void);

/* ---- 関数: 引数と戻り値のいろいろ ---- */
void f_void(void)
{
    g_bss_uc++;
}

unsigned char f_u8(unsigned char a)
{
    return (unsigned char)(a + 1);
}

unsigned int add2(unsigned int a, unsigned int b)
{
    return a + b;
}

unsigned long f_u32(unsigned long a)
{
    return a + 0x01020304UL;
}

unsigned long f_mix(unsigned char a, unsigned long b, unsigned int c, unsigned char d)
{
    return (unsigned long)a + b + c + d;
}

long f_long2(long a, long b)
{
    return a - b;
}

float f_float(float a, float b)
{
    return a * b;
}

unsigned int f_ptr(unsigned char *p, unsigned int n)
{
    unsigned int i;
    unsigned int sum = 0;
    for (i = 0; i < n; i++) {
        sum += p[i];
    }
    return sum;
}

unsigned char f_farptr(unsigned char __far *p)
{
    return *p;
}

unsigned int f_struct_ptr(struct outer *p)
{
    return (unsigned int)(p->oa + p->in.ib + p->tail);
}

struct inner f_ret_struct(unsigned char a)
{
    struct inner r;
    r.ia = a;
    r.ib = 0x1234;
    r.ic = (unsigned char)(a + 1);
    return r;
}

enum small_e f_enum(enum small_e e)
{
    return (e == SE_A) ? SE_B : SE_A;
}

/* 関数内 static（同じ名前を 2 つの関数に置く） */
unsigned int f_static1(void)
{
    static unsigned int  cnt = 0xC1C2;
    static unsigned char buf[4];
    buf[0]++;
    return ++cnt;
}

unsigned int f_static2(void)
{
    static unsigned int cnt = 0xC3C4;
    static struct inner keep = { 0xC5, 0xC6C7, 0xC8 };
    keep.ia++;
    {
        static unsigned char nested = 0xC9;   /* ブロックの中の static */
        nested++;
    }
    return ++cnt;
}

static unsigned char f_file_static(unsigned char a)
{
    return (unsigned char)(a + s_file_uc + s_file_bss);
}

/* ローカル変数とレジスタ変数 */
unsigned int f_locals(unsigned int a)
{
    unsigned char  l8  = 1;
    unsigned int   l16 = 2;
    unsigned long  l32 = 3;
    struct inner   ls;
    register unsigned int r = a;
    ls.ia = l8;
    ls.ib = l16;
    ls.ic = (unsigned char)l32;
    return (unsigned int)(r + ls.ia + ls.ib + ls.ic);
}

void main(void)
{
    struct inner t;

    f_void();
    g_uc   = f_u8(g_uc);
    g_ui   = add2(g_ui, 0x0102);
    g_ul   = f_u32(g_ul);
    g_ul   = f_mix(0x01, 0x02030405UL, 0x0607, 0x08);
    g_l    = f_long2(0x11121314L, 0x01020304L);
    g_f    = f_float(g_f, 2.0f);
    g_d    = (double)g_f;
    g_ui   = f_ptr(g_outer.arr, 3);
    g_uc   = f_farptr(g_fp_uc);
    g_ui   = f_struct_ptr(g_p_outer);
    t      = f_ret_struct(0x21);
    g_outer.in = t;
    g_se   = f_enum(g_se);
    g_ui   = f_static1() + f_static2();
    g_uc   = f_file_static(g_uc);
    g_ui   = f_locals(g_ui);
    g_ui   = g_fn(1, 2);
    g_fn_tbl[0] = f_void;
    g_bits8.n2  = 3;
    g_bits16.f5 = 0x2A;
    g_bitsm.s0  = -1;
    g_uni.uc[1] = 0x77;
    g_mat2[1][2]    = 0x88;
    g_mat3[1][2][3] = 0x8999;
    g_outer_arr[1].ins[1].ib = 0x8A8B;
    g_bss_outer = g_outer;
    g_bss_ui    = (unsigned int)(c_tbl[1] + c_wtbl[1] + c_inner.ib + g_vol + s_file_ui + s_file_in.ib);
    g_td_pp->y  = g_td_u16;
    g_td_pt.z   = g_td_u32 + g_td_u8;
    **g_pp_uc   = *g_p_const;
    *g_p_ui     = (*g_p_arr)[0];
    g_p_inner->ic = *g_parr[1];
    g_fp_outer->tail = g_ne + g_be + g_c + g_sc + g_s + g_us + g_i;

    for (;;) {
        ;
    }
}
