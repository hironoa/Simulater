/*
 * mkabs.c - テスト用のロードモジュール（ELF32 + DWARF）を作る
 *
 *   mkabs <種類> <出力.abs>
 *     dwarf2   DWARF バージョン 2（CC-RL と同じ）。ビットフィールドは DW_AT_bit_offset（MSB 側から数える）
 *     dwarf3   バージョン 3。メンバの位置は定数
 *     dwarf4   バージョン 4。名前は .debug_str、DW_AT_data_bit_offset、exprloc、flag_present、high_pc は長さ
 *     nodwarf  デバッグ情報なし（.symtab だけ）。同じ名前の .map（Symbol List つき）も書く
 *
 * 機械語は testprog_code.h（tests/tools/testprog.s を GNU as でアセンブルしたもの）。変数のアドレスは
 * testprog.s の .set と合わせてある。どの種類にも、処理系独自の属性（0x2001、0x3fe1）と知らないタグ（0x4081）、
 * 宣言だけの関数、ロケーションリストの局所変数、lexical_block の中の static を入れて、読み飛ばしを確かめる。
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testprog_code.h"

/* ================================================================ バッファ */

typedef struct { unsigned char *p; size_t n, cap; } buf;

static void put(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "mkabs: メモリが足りません\n"); exit(2); }
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(buf *b, unsigned v) { unsigned char c = (unsigned char)v; put(b, &c, 1); }
static void put16(buf *b, unsigned v) { put8(b, v & 0xff); put8(b, (v >> 8) & 0xff); }
static void put32(buf *b, unsigned long v) { put16(b, (unsigned)(v & 0xffff)); put16(b, (unsigned)((v >> 16) & 0xffff)); }
static void putuleb(buf *b, unsigned long v) { do { unsigned c = v & 0x7f; v >>= 7; put8(b, c | (v ? 0x80 : 0)); } while (v); }
static void putsleb(buf *b, long v)
{
    int more = 1;
    while (more) {
        unsigned c = (unsigned)(v & 0x7f);
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) more = 0;
        else c |= 0x80;
        put8(b, c);
    }
}
static void putstr(buf *b, const char *s) { put(b, s, strlen(s) + 1); }
static void set32(buf *b, size_t at, unsigned long v)
{
    b->p[at] = (unsigned char)v; b->p[at + 1] = (unsigned char)(v >> 8);
    b->p[at + 2] = (unsigned char)(v >> 16); b->p[at + 3] = (unsigned char)(v >> 24);
}

/* ================================================================ DWARF */

enum {
    TAG_array = 0x01, TAG_enum = 0x04, TAG_param = 0x05, TAG_label = 0x0a, TAG_lexblock = 0x0b, TAG_member = 0x0d,
    TAG_ptr = 0x0f, TAG_cu = 0x11, TAG_struct = 0x13, TAG_typedef = 0x16, TAG_union = 0x17, TAG_subrange = 0x21,
    TAG_base = 0x24, TAG_const = 0x26, TAG_enumerator = 0x28, TAG_subprogram = 0x2e, TAG_variable = 0x34,
    TAG_volatile = 0x35
};
enum {
    AT_location = 0x02, AT_name = 0x03, AT_byte_size = 0x0b, AT_bit_offset = 0x0c, AT_bit_size = 0x0d,
    AT_low_pc = 0x11, AT_high_pc = 0x12, AT_language = 0x13, AT_comp_dir = 0x1b, AT_const_value = 0x1c,
    AT_producer = 0x25, AT_upper_bound = 0x2f, AT_address_class = 0x33, AT_count = 0x37, AT_member_loc = 0x38,
    AT_declaration = 0x3c, AT_encoding = 0x3e, AT_external = 0x3f, AT_type = 0x49, AT_data_bit_offset = 0x6b
};
enum {
    F_addr = 0x01, F_data2 = 0x05, F_data4 = 0x06, F_string = 0x08, F_block1 = 0x0a, F_data1 = 0x0b, F_flag = 0x0c,
    F_sdata = 0x0d, F_strp = 0x0e, F_ref4 = 0x13, F_sec_offset = 0x17, F_exprloc = 0x18, F_flag_present = 0x19
};

static int V = 2;
static buf info, abbr, dstr;

typedef struct { buf enc; } abent;
static abent abtab[128];
static int nab;

/* pairs: at, form, ..., 0。同じ内容の略語は使い回す */
static int abbrev(int tag, int children, const int *pairs)
{
    buf e = {0};
    int i;
    putuleb(&e, (unsigned long)tag);
    put8(&e, (unsigned)children);
    for (i = 0; pairs[i]; i += 2) { putuleb(&e, (unsigned long)pairs[i]); putuleb(&e, (unsigned long)pairs[i + 1]); }
    put8(&e, 0);
    put8(&e, 0);
    for (i = 0; i < nab; i++)
        if (abtab[i].enc.n == e.n && memcmp(abtab[i].enc.p, e.p, e.n) == 0) { free(e.p); return i + 1; }
    abtab[nab].enc = e;
    nab++;
    putuleb(&abbr, (unsigned long)nab);
    put(&abbr, e.p, e.n);
    return nab;
}

/* CU 内のラベル（CU 先頭からの位置）と、ref4 の後埋め */
#define MAXL 128
static long lab[MAXL];
static struct { size_t pos; int l; } fx[4096];
static int nfx;
static size_t cu_start;

static void mark(int l) { lab[l] = (long)(info.n - cu_start); }
static void ref(int l) { fx[nfx].pos = info.n; fx[nfx].l = l; nfx++; put32(&info, 0); }

static int f_name(void) { return V >= 4 ? F_strp : F_string; }
static void p_name(const char *s)
{
    if (V >= 4) { put32(&info, (unsigned long)dstr.n); putstr(&dstr, s); }
    else putstr(&info, s);
}
static int f_flag(void) { return V >= 4 ? F_flag_present : F_flag; }
static void p_flag(void) { if (V < 4) put8(&info, 1); }
static int f_loc(void) { return V >= 4 ? F_exprloc : F_block1; }
static void p_loc(unsigned long addr)
{
    if (V >= 4) putuleb(&info, 5); else put8(&info, 5);
    put8(&info, 0x03); /* DW_OP_addr */
    put32(&info, addr);
}
static int f_mloc(void) { return V == 2 ? F_block1 : F_data1; }
static void p_mloc(unsigned off)
{
    if (V == 2) { put8(&info, 2); put8(&info, 0x23); put8(&info, off); } /* DW_OP_plus_uconst */
    else put8(&info, off);
}
static int f_hi(void) { return V >= 4 ? F_data4 : F_addr; }
static void p_hi(unsigned long lo, unsigned long hi) { if (V >= 4) put32(&info, hi - lo); else put32(&info, hi); }

static void d_end(void) { put8(&info, 0); }

static void d_base(int l, const char *name, int size, int enc)
{
    int pr[] = {AT_name, f_name(), AT_encoding, F_data1, AT_byte_size, F_data1, 0};
    mark(l);
    putuleb(&info, (unsigned long)abbrev(TAG_base, 0, pr));
    p_name(name); put8(&info, (unsigned)enc); put8(&info, (unsigned)size);
}

static void d_typedef(int l, const char *name, int tl)
{
    int pr[] = {AT_name, f_name(), AT_type, F_ref4, 0};
    mark(l);
    putuleb(&info, (unsigned long)abbrev(TAG_typedef, 0, pr));
    p_name(name); ref(tl);
}

static void d_qual(int l, int tag, int tl)
{
    int pr[] = {AT_type, F_ref4, 0};
    mark(l);
    putuleb(&info, (unsigned long)abbrev(tag, 0, pr));
    ref(tl);
}

/* CC-RL の near データポインタ: DW_AT_address_class 4、DW_AT_byte_size なし */
static void d_ptr_near(int l, int tl)
{
    int pr[] = {AT_type, F_ref4, AT_address_class, F_data1, 0};
    mark(l);
    putuleb(&info, (unsigned long)abbrev(TAG_ptr, 0, pr));
    ref(tl); put8(&info, 4);
}

static void d_ptr_far(int l, int tl)
{
    int pr[] = {AT_type, F_ref4, AT_byte_size, F_data1, 0};
    mark(l);
    putuleb(&info, (unsigned long)abbrev(TAG_ptr, 0, pr));
    ref(tl); put8(&info, 4);
}

static void d_agg_begin(int l, int tag, const char *name, int size)
{
    mark(l);
    if (name) {
        int pr[] = {AT_name, f_name(), AT_byte_size, F_data1, 0};
        putuleb(&info, (unsigned long)abbrev(tag, 1, pr));
        p_name(name); put8(&info, (unsigned)size);
    } else {
        int pr[] = {AT_byte_size, F_data1, 0};
        putuleb(&info, (unsigned long)abbrev(tag, 1, pr));
        put8(&info, (unsigned)size);
    }
}

static void d_member(const char *name, int tl, unsigned off)
{
    if (name) {
        int pr[] = {AT_name, f_name(), AT_type, F_ref4, AT_member_loc, f_mloc(), 0};
        putuleb(&info, (unsigned long)abbrev(TAG_member, 0, pr));
        p_name(name); ref(tl); p_mloc(off);
    } else {
        int pr[] = {AT_type, F_ref4, AT_member_loc, f_mloc(), 0};
        putuleb(&info, (unsigned long)abbrev(TAG_member, 0, pr));
        ref(tl); p_mloc(off);
    }
}

/* ビットフィールド。unit_off/unit_size は記憶単位、bitpos は記憶単位の LSB からのビット位置 */
static void d_member_bits(const char *name, int tl, unsigned unit_off, unsigned unit_size, unsigned bitpos, unsigned bits)
{
    if (V >= 4) {
        int pr[] = {AT_name, f_name(), AT_type, F_ref4, AT_bit_size, F_data1, AT_data_bit_offset, F_data1, 0};
        putuleb(&info, (unsigned long)abbrev(TAG_member, 0, pr));
        p_name(name); ref(tl); put8(&info, bits); put8(&info, unit_off * 8 + bitpos);
    } else {
        int pr[] = {AT_name, f_name(), AT_type, F_ref4, AT_byte_size, F_data1, AT_bit_offset, F_data1,
                    AT_bit_size, F_data1, AT_member_loc, f_mloc(), 0};
        putuleb(&info, (unsigned long)abbrev(TAG_member, 0, pr));
        p_name(name); ref(tl); put8(&info, unit_size); put8(&info, unit_size * 8 - bitpos - bits); put8(&info, bits);
        p_mloc(unit_off);
    }
}

static void d_array(int l, int tl, int n1, int n2)
{
    int pr[] = {AT_type, F_ref4, 0};
    int ps[] = {V == 2 ? AT_upper_bound : AT_count, F_data1, 0};
    int k;
    mark(l);
    putuleb(&info, (unsigned long)abbrev(TAG_array, 1, pr));
    ref(tl);
    for (k = 0; k < (n2 ? 2 : 1); k++) {
        int n = k ? n2 : n1;
        putuleb(&info, (unsigned long)abbrev(TAG_subrange, 0, ps));
        put8(&info, (unsigned)(V == 2 ? n - 1 : n));
    }
    d_end();
}

static void d_enum_begin(int l, const char *name, int size)
{
    d_agg_begin(l, TAG_enum, name, size);
}

static void d_enumerator(const char *name, long v)
{
    int pr[] = {AT_name, f_name(), AT_const_value, v < 0 ? F_sdata : F_data1, 0};
    putuleb(&info, (unsigned long)abbrev(TAG_enumerator, 0, pr));
    p_name(name);
    if (v < 0) putsleb(&info, v); else put8(&info, (unsigned)v);
}

static int g_junk_done;

static void d_var(const char *name, int tl, unsigned long addr, int ext)
{
    if (!g_junk_done) {
        /* 処理系独自の属性（data2 と block1） */
        int pr[] = {AT_external, f_flag(), AT_name, f_name(), 0x2001, F_data2, AT_type, F_ref4, AT_location, f_loc(),
                    0x3fe1, F_block1, 0};
        putuleb(&info, (unsigned long)abbrev(TAG_variable, 0, pr));
        p_flag(); p_name(name); put16(&info, 0xBEEF); ref(tl); p_loc(addr);
        put8(&info, 3); put8(&info, 0xAA); put8(&info, 0xBB); put8(&info, 0xCC);
        g_junk_done = 1;
        return;
    }
    if (ext) {
        int pr[] = {AT_external, f_flag(), AT_name, f_name(), AT_type, F_ref4, AT_location, f_loc(), 0};
        putuleb(&info, (unsigned long)abbrev(TAG_variable, 0, pr));
        p_flag(); p_name(name); ref(tl); p_loc(addr);
    } else {
        int pr[] = {AT_name, f_name(), AT_type, F_ref4, AT_location, f_loc(), 0};
        putuleb(&info, (unsigned long)abbrev(TAG_variable, 0, pr));
        p_name(name); ref(tl); p_loc(addr);
    }
}

/* ロケーションリストの局所変数（静的なアドレスではないので読み飛ばされるべきもの） */
static void d_local(const char *name, int tl)
{
    int pr[] = {AT_name, f_name(), AT_type, F_ref4, AT_location, V >= 4 ? F_sec_offset : F_data4, 0};
    putuleb(&info, (unsigned long)abbrev(TAG_variable, 0, pr));
    p_name(name); ref(tl); put32(&info, 0);
}

/* name に "(void)" を付けると CC-RL の static 関数の名前の付け方になる */
static void d_func_begin(const char *name, unsigned long lo, unsigned long hi, int ext, int ret_tl)
{
    int pr_e_r[] = {AT_external, f_flag(), AT_name, f_name(), AT_type, F_ref4, AT_low_pc, F_addr, AT_high_pc, f_hi(), 0};
    int pr_e[] = {AT_external, f_flag(), AT_name, f_name(), AT_low_pc, F_addr, AT_high_pc, f_hi(), 0};
    int pr_r[] = {AT_name, f_name(), AT_type, F_ref4, AT_low_pc, F_addr, AT_high_pc, f_hi(), 0};
    int pr[] = {AT_name, f_name(), AT_low_pc, F_addr, AT_high_pc, f_hi(), 0};
    if (ext && ret_tl >= 0) { putuleb(&info, (unsigned long)abbrev(TAG_subprogram, 1, pr_e_r)); p_flag(); p_name(name); ref(ret_tl); }
    else if (ext) { putuleb(&info, (unsigned long)abbrev(TAG_subprogram, 1, pr_e)); p_flag(); p_name(name); }
    else if (ret_tl >= 0) { putuleb(&info, (unsigned long)abbrev(TAG_subprogram, 1, pr_r)); p_name(name); ref(ret_tl); }
    else { putuleb(&info, (unsigned long)abbrev(TAG_subprogram, 1, pr)); p_name(name); }
    put32(&info, lo);
    p_hi(lo, hi);
}

static void d_param(const char *name, int tl)
{
    int pr[] = {AT_name, f_name(), AT_type, F_ref4, 0};
    putuleb(&info, (unsigned long)abbrev(TAG_param, 0, pr));
    p_name(name); ref(tl);
}

/* 宣言だけの関数（low_pc なし） */
static void d_func_decl(const char *name)
{
    int pr[] = {AT_external, f_flag(), AT_name, f_name(), AT_declaration, f_flag(), 0};
    putuleb(&info, (unsigned long)abbrev(TAG_subprogram, 0, pr));
    p_flag(); p_name(name); p_flag();
}

static void d_lexblock_begin(void)
{
    int pr[] = {0};
    putuleb(&info, (unsigned long)abbrev(TAG_lexblock, 1, pr));
}

static void d_label(const char *name, unsigned long addr)
{
    int pr[] = {AT_name, F_string, AT_low_pc, F_addr, AT_external, F_flag, 0};
    putuleb(&info, (unsigned long)abbrev(TAG_label, 0, pr));
    putstr(&info, name); put32(&info, addr); put8(&info, 1);
}

/* 知らないタグ（子つき）。読み手は中身ごと飛ばすべき */
static void d_junk_tag(void)
{
    int pr[] = {AT_name, F_string, 0x2005, F_data4, 0};
    int pc[] = {AT_byte_size, F_data1, 0};
    putuleb(&info, (unsigned long)abbrev(0x4081, 1, pr));
    putstr(&info, "renesas_vendor"); put32(&info, 0x12345678);
    putuleb(&info, (unsigned long)abbrev(0x4082, 0, pc));
    put8(&info, 9);
    d_end();
}

static void cu_begin(const char *name, unsigned long lo, unsigned long hi, int asm_cu)
{
    int pr[] = {AT_name, f_name(), AT_producer, f_name(), AT_language, F_data2, AT_comp_dir, f_name(), AT_low_pc, F_addr, AT_high_pc, f_hi(), 0};
    int pa[] = {AT_name, F_string, AT_language, F_data2, 0};
    int i;
    cu_start = info.n;
    nfx = 0;
    for (i = 0; i < MAXL; i++) lab[i] = -1;
    put32(&info, 0);                   /* unit_length（後で） */
    put16(&info, (unsigned)(asm_cu ? 2 : V));
    put32(&info, 0);                   /* abbrev_offset: 略語表は全 CU で共有 */
    put8(&info, 4);                    /* address_size */
    if (asm_cu) {
        int save = V;
        V = 2;
        putuleb(&info, (unsigned long)abbrev(TAG_cu, 1, pa));
        putstr(&info, name); put16(&info, 0x8000);
        V = save;
        return;
    }
    putuleb(&info, (unsigned long)abbrev(TAG_cu, 1, pr));
    p_name(name); p_name("mkabs test generator (not CC-RL)"); put16(&info, 0x0c);
    p_name("C:\\work\\testprog"); put32(&info, lo); p_hi(lo, hi);
}

static void cu_end(void)
{
    int i;
    d_end();
    set32(&info, cu_start, (unsigned long)(info.n - cu_start - 4));
    for (i = 0; i < nfx; i++) {
        if (lab[fx[i].l] < 0) { fprintf(stderr, "mkabs: ラベル %d が未定義です\n", fx[i].l); exit(2); }
        set32(&info, fx[i].pos, (unsigned long)lab[fx[i].l]);
    }
}

/* ---------------------------------------------------------------- 関数の範囲 */

typedef struct { const char *name; unsigned long addr; } fn_t;
static const fn_t FN[] = {
    {"start", TP_start}, {"exit", TP_exit}, {"main", TP_main}, {"f_mix", TP_f_mix}, {"f_long", TP_f_long},
    {"f_stack", TP_f_stack}, {"f_args8", TP_f_args8}, {"f_mix2", TP_f_mix2}, {"f_far_read", TP_f_far_read},
    {"f_far_write", TP_f_far_write}, {"f_far2", TP_f_far2}, {"f_far3", TP_f_far3}, {"f_sum", TP_f_sum},
    {"f_neg", TP_f_neg}, {"f_float", TP_f_float}, {"f_ll", TP_f_ll}, {"f_ptr_struct", TP_f_ptr_struct},
    {"f_bits", TP_f_bits}, {"f_mulhu", TP_f_mulhu}, {"f_mdu", TP_f_mdu}, {"read_io", TP_read_io},
    {"AMSTB_read_io", TP_AMSTB_read_io}, {"use_io", TP_use_io}, {"TEST_cycle", TP_TEST_cycle},
    {"intr_tm00", TP_intr_tm00}, {"cnt_call", TP_cnt_call}, {"engine_inc", TP_engine_inc},
    {"sensor_inc", TP_sensor_inc}, {"intr_reti", TP_intr_reti}, {"f_loop", TP_f_loop}, {"f_undef", TP_f_undef},
    {"f_halt", TP_f_halt},
};
#define NFN (sizeof FN / sizeof FN[0])

static unsigned long fend(unsigned long a)
{
    unsigned long e = TP_code_end;
    size_t i;
    for (i = 0; i < NFN; i++) if (FN[i].addr > a && FN[i].addr < e) e = FN[i].addr;
    return e;
}

static unsigned long faddr(const char *n)
{
    size_t i;
    for (i = 0; i < NFN; i++) if (strcmp(FN[i].name, n) == 0) return FN[i].addr;
    fprintf(stderr, "mkabs: 関数 %s がありません\n", n);
    exit(2);
}

/* ---------------------------------------------------------------- 型のラベル */

enum {
    L_uchar = 1, L_ushort, L_ulong, L_schar, L_short, L_long, L_float, L_llong, L_bool, L_char,
    L_u8, L_u16, L_u32, L_i8, L_i16,
    L_p_u8, L_cu8, L_p_cu8, L_fp_u8, L_fp_cu8,
    L_S, L_p_S, L_cS, L_p_cS, L_W, L_E, L_E2, L_U, L_C, L_Cu, L_Cs,
    L_a_u8_8, L_a_u8_3, L_a_u8_2_3, L_cu16, L_a_cu16_4, L_cchar, L_a_cchar_8, L_vu16, L_a_u8_2,
    L_const_mixed,
    L_last
};

static void common_types(void)
{
    d_base(L_uchar, "unsigned char", 1, 8);
    d_base(L_ushort, "unsigned short", 2, 7);
    d_base(L_ulong, "unsigned long", 4, 7);
    d_base(L_schar, "signed char", 1, 6);
    d_base(L_short, "short", 2, 5);
    d_base(L_long, "long", 4, 5);
    d_base(L_float, "float", 4, 4);
    d_base(L_llong, "long long", 8, 5);
    d_base(L_bool, "_Bool", 1, 2);
    d_base(L_char, "char", 1, 8);
    d_typedef(L_u8, "uint8_t", L_uchar);
    d_typedef(L_u16, "uint16_t", L_ushort);
    d_typedef(L_u32, "uint32_t", L_ulong);
    d_typedef(L_i8, "int8_t", L_schar);
    d_typedef(L_i16, "int16_t", L_short);
    d_ptr_near(L_p_u8, L_u8);
    d_qual(L_cu8, TAG_const, L_u8);
    d_ptr_near(L_p_cu8, L_cu8);
    d_ptr_far(L_fp_u8, L_u8);
    d_ptr_far(L_fp_cu8, L_cu8);
    /* struct S { uint8_t fa:3; uint8_t fb:4; uint8_t fc:1; uint8_t pad; uint16_t w; int16_t s; } */
    d_agg_begin(L_S, TAG_struct, "S", 6);
    d_member_bits("fa", L_u8, 0, 1, 0, 3);
    d_member_bits("fb", L_u8, 0, 1, 3, 4);
    d_member_bits("fc", L_u8, 0, 1, 7, 1);
    d_member("pad", L_u8, 1);
    d_member("w", L_u16, 2);
    d_member("s", L_i16, 4);
    d_end();
    d_ptr_near(L_p_S, L_S);
    d_qual(L_cS, TAG_const, L_S);
    d_ptr_near(L_p_cS, L_cS);
    /* struct W { uint16_t lo:5; uint16_t mid:6; uint16_t hi:5; }（16 ビットの記憶単位） */
    d_agg_begin(L_W, TAG_struct, "W", 2);
    d_member_bits("lo", L_ushort, 0, 2, 0, 5);
    d_member_bits("mid", L_ushort, 0, 2, 5, 6);
    d_member_bits("hi", L_ushort, 0, 2, 11, 5);
    d_end();
    d_enum_begin(L_E, "E", 1);
    d_enumerator("E_A", 0);
    d_enumerator("E_B", 1);
    d_enumerator("E_C", 200);
    d_end();
    d_enum_begin(L_E2, "E2", 1);
    d_enumerator("N1", -1);
    d_enumerator("P1", 1);
    d_end();
    d_array(L_a_u8_2, L_u8, 2, 0);
    /* union U { uint16_t w; uint8_t b[2]; } */
    d_agg_begin(L_U, TAG_union, "U", 2);
    d_member("w", L_u16, 0);
    d_member("b", L_a_u8_2, 0);
    d_end();
    /* struct C { union { uint16_t word; struct { uint8_t lo, hi; }; }; int16_t off; }（匿名メンバ） */
    d_agg_begin(L_Cs, TAG_struct, NULL, 2);
    d_member("lo", L_u8, 0);
    d_member("hi", L_u8, 1);
    d_end();
    d_agg_begin(L_Cu, TAG_union, NULL, 2);
    d_member("word", L_u16, 0);
    d_member(NULL, L_Cs, 0);
    d_end();
    d_agg_begin(L_C, TAG_struct, "C", 4);
    d_member(NULL, L_Cu, 0);
    d_member("off", L_i16, 2);
    d_end();
    d_array(L_a_u8_8, L_u8, 8, 0);
    d_array(L_a_u8_3, L_u8, 3, 0);
    d_array(L_a_u8_2_3, L_u8, 2, 3);
    d_qual(L_cu16, TAG_const, L_u16);
    d_array(L_a_cu16_4, L_cu16, 4, 0);
    d_qual(L_cchar, TAG_const, L_char);
    d_array(L_a_cchar_8, L_cchar, 8, 0);
    d_qual(L_vu16, TAG_volatile, L_u16);
    d_agg_begin(L_const_mixed, TAG_struct, "ConstMixed", 4);
    d_member("ro", L_cu8, 0);
    d_member("rw", L_u8, 1);
    d_member("data", L_a_u8_2, 2);
    d_end();
}

#define FUNC(n, ext, ret) d_func_begin(n, faddr(n), fend(faddr(n)), ext, ret)

static void build_dwarf(void)
{
    /* ---- engine.c ---- */
    cu_begin("src\\engine.c", TP_f_mix, TP_code_end, 0);
    common_types();
    d_var("g_a", L_u8, 0xFE100, 1);           /* 最初の変数に独自属性が付く */
    d_junk_tag();
    d_var("g_const_mixed", L_const_mixed, 0xFE180, 1);
    d_var("g_const_elements", L_a_cu16_4, 0xFE184, 1);
    d_var("g_const_record", L_cS, 0xFE18C, 1);

    d_var("g_c", L_u8, 0xFE101, 1);
    d_var("g_b", L_u16, 0xFE102, 1);
    d_var("g_l", L_u32, 0xFE104, 1);
    d_var("g_w", L_u16, 0xFE108, 1);
    d_var("g_sa", L_u16, 0xFE10A, 1);
    d_var("g_sb", L_u16, 0xFE10C, 1);
    d_var("g_sc", L_u16, 0xFE10E, 1);
    d_var("g_sdd", L_u16, 0xFE110, 1);
    d_var("g_se", L_u8, 0xFE112, 1);
    d_var("g_out", L_u8, 0xFE113, 1);
    d_var("g_sf", L_u32, 0xFE114, 1);
    d_var("g_sum", L_u16, 0xFE118, 1);
    d_var("g_b8", L_a_u8_8, 0xFE11A, 1);
    d_var("g_m_a", L_u8, 0xFE122, 1);
    d_var("g_m_d", L_u8, 0xFE123, 1);
    d_var("g_m_b", L_u32, 0xFE124, 1);
    d_var("g_m_c", L_u16, 0xFE128, 1);
    d_var("g_fp", L_fp_u8, 0xFE12A, 1);
    d_var("g_stub_val", L_u16, 0xFE12E, 1);
    d_var("g_isr_cnt", L_u8, 0xFE13D, 1);
    d_var("g_st", L_S, 0xFE13E, 1);
    d_var("g_w16", L_W, 0xFE144, 1);
    d_var("s_cnt", L_u8, 0xFE146, 0);
    d_var("gp_st", L_p_S, 0xFE14A, 1);
    d_var("g_e", L_E, 0xFE14C, 1);
    d_var("g_e2", L_E2, 0xFE14D, 1);
    d_var("g_bool", L_bool, 0xFE14E, 1);
    d_var("g_mat", L_a_u8_2_3, 0xFE14F, 1);
    d_var("g_u", L_U, 0xFE156, 1);
    d_var("g_cal", L_C, 0xFE158, 1);
    d_var("g_vd", L_vu16, 0xFE15C, 1);
    d_var("g_init16", L_u16, 0xFE000, 1);
    d_var("g_init8", L_u8, 0xFE002, 1);
    d_var("g_initarr", L_a_u8_3, 0xFE003, 1);
    d_var("gp_init", L_p_u8, 0xFE006, 1);
    d_var("gfp_init", L_fp_cu8, 0xFE008, 1);
    d_var("g_sd", L_u16, 0xFFE40, 1);
    d_var("g_lut", L_a_cu16_4, 0x2000, 1);
    d_var("g_name", L_a_cchar_8, 0x2008, 1);
    d_func_decl("ext_only");
    FUNC("main", 1, -1); d_end();
    FUNC("f_mix", 1, L_u16); d_param("a", L_u8); d_param("b", L_u16); d_param("c", L_u8); d_local("tmp", L_u16); d_end();
    FUNC("f_long", 1, L_u32); d_param("x", L_u32); d_param("y", L_u16); d_end();
    FUNC("f_stack", 1, L_u16); d_param("a", L_u16); d_param("b", L_u16); d_param("c", L_u16); d_param("d", L_u16);
    d_param("e", L_u8); d_param("f", L_u32); d_end();
    FUNC("f_args8", 1, L_u8); d_param("a1", L_u8); d_param("a2", L_u8); d_param("a3", L_u8); d_param("a4", L_u8);
    d_param("a5", L_u8); d_param("a6", L_u8); d_param("a7", L_u8); d_param("a8", L_u8); d_end();
    FUNC("f_mix2", 1, -1); d_param("a", L_u8); d_param("b", L_u32); d_param("c", L_u16); d_param("d", L_u8); d_end();
    FUNC("f_far_read", 1, L_u8); d_param("p", L_fp_cu8); d_end();
    FUNC("f_far_write", 1, L_fp_u8); d_param("p", L_fp_u8); d_param("v", L_u8); d_end();
    FUNC("f_far2", 1, -1); d_param("a", L_u8); d_param("w", L_u16); d_param("p", L_fp_u8); d_end();
    FUNC("f_far3", 1, -1); d_param("w1", L_u16); d_param("w2", L_u16); d_param("p", L_fp_u8); d_end();
    FUNC("f_sum", 1, L_u16); d_param("p", L_p_u8); d_param("n", L_u8); d_end();
    FUNC("f_neg", 1, L_i8); d_end();
    FUNC("f_float", 1, L_float); d_end();
    FUNC("f_ll", 1, L_llong); d_end();
    FUNC("f_ptr_struct", 1, L_u16); d_param("p", L_p_S); d_end();
    FUNC("f_bits", 1, -1); d_end();
    FUNC("f_mulhu", 1, L_u32); d_param("a", L_u16); d_param("b", L_u16); d_end();
    FUNC("f_mdu", 1, L_u16); d_param("a", L_u16); d_param("b", L_u16); d_end();
    FUNC("read_io", 1, L_u16); d_end();
    FUNC("use_io", 1, L_u16); d_end();
    FUNC("cnt_call", 1, L_u16);
    d_lexblock_begin();
    d_var("calls", L_u16, 0xFE148, 0);       /* 関数内 static（lexical_block の中） */
    d_end();
    d_end();
    FUNC("engine_inc", 1, -1); d_end();
    FUNC("intr_reti", 1, -1); d_end();
    FUNC("f_loop", 1, -1); d_end();
    FUNC("f_undef", 1, -1); d_end();
    FUNC("f_halt", 1, -1); d_end();
    cu_end();

    /* ---- sensor.c ---- */
    cu_begin("src\\sensor.c", TP_sensor_inc, fend(TP_sensor_inc), 0);
    d_base(L_uchar, "unsigned char", 1, 8);
    d_typedef(L_u8, "uint8_t", L_uchar);
    d_var("s_cnt", L_u8, 0xFE147, 0);
    FUNC("sensor_inc", 1, -1); d_end();
    cu_end();

    /* ---- TEST_drv.c ---- */
    cu_begin("winams\\TEST_drv.c", TP_TEST_cycle, fend(TP_intr_tm00), 0);
    d_base(L_uchar, "unsigned char", 1, 8);
    d_base(L_ushort, "unsigned short", 2, 7);
    d_base(L_ulong, "unsigned long", 4, 7);
    d_typedef(L_u8, "uint8_t", L_uchar);
    d_typedef(L_u16, "uint16_t", L_ushort);
    d_typedef(L_u32, "uint32_t", L_ulong);
    d_var("TEST_time_us", L_u32, 0xFE130, 1);
    d_var("g_ms_tick", L_u32, 0xFE134, 1);
    d_var("TEST_cycle_cnt", L_u16, 0xFE138, 1);
    d_var("s_i", L_u16, 0xFE13A, 0);
    FUNC("TEST_cycle", 1, -1);
    d_var("sub", L_u8, 0xFE13C, 0);          /* 関数内 static */
    d_end();
    d_func_begin("intr_tm00(void)", TP_intr_tm00, fend(TP_intr_tm00), 0, -1); d_end(); /* static 関数（CC-RL 流の名前） */
    cu_end();

    /* ---- AMSTB_SrcFile.c ---- */
    cu_begin("winams\\AMSTB_SrcFile.c", TP_AMSTB_read_io, fend(TP_AMSTB_read_io), 0);
    d_base(L_ushort, "unsigned short", 2, 7);
    d_typedef(L_u16, "uint16_t", L_ushort);
    FUNC("AMSTB_read_io", 1, L_u16); d_end();
    cu_end();

    /* ---- cstart.asm（アセンブラの CU。常に DWARF2） ---- */
    cu_begin("src\\cstart.asm", 0, 0, 1);
    d_label("_start", TP_start);
    d_label("_exit", TP_exit);
    cu_end();
}

/* ================================================================ ELF */

typedef struct { const char *name; unsigned type, flags, addr, size, link, info, align, entsize; const unsigned char *data; size_t off; } sect_t;

static sect_t S[32];
static int NS;
static buf strtab, symtab, shstr;

static int add_sect(const char *name, unsigned type, unsigned flags, unsigned addr, const void *data, unsigned size)
{
    sect_t *s = &S[NS];
    memset(s, 0, sizeof *s);
    s->name = name;
    s->type = type;
    s->flags = flags;
    s->addr = addr;
    s->size = size;
    s->data = data;
    s->align = 1;
    return NS++;
}

static void sym(const char *name, unsigned long value, unsigned long size, int bind, int type, int shndx)
{
    put32(&symtab, name[0] ? (unsigned long)strtab.n : 0);
    if (name[0]) putstr(&strtab, name);
    put32(&symtab, value);
    put32(&symtab, size);
    put8(&symtab, (unsigned)((bind << 4) | type));
    put8(&symtab, 0);
    put16(&symtab, (unsigned)shndx);
}

typedef struct { const char *sym; unsigned long addr, size; int global; } dsym;
static const dsym DS[] = {
    {"_g_a", 0xFE100, 1, 1}, {"_g_c", 0xFE101, 1, 1}, {"_g_b", 0xFE102, 2, 1}, {"_g_l", 0xFE104, 4, 1},
    {"_g_w", 0xFE108, 2, 1}, {"_g_sa", 0xFE10A, 2, 1}, {"_g_sb", 0xFE10C, 2, 1}, {"_g_sc", 0xFE10E, 2, 1},
    {"_g_sdd", 0xFE110, 2, 1}, {"_g_se", 0xFE112, 1, 1}, {"_g_out", 0xFE113, 1, 1}, {"_g_sf", 0xFE114, 4, 1},
    {"_g_sum", 0xFE118, 2, 1}, {"_g_b8", 0xFE11A, 8, 1}, {"_g_m_a", 0xFE122, 1, 1}, {"_g_m_d", 0xFE123, 1, 1},
    {"_g_m_b", 0xFE124, 4, 1}, {"_g_m_c", 0xFE128, 2, 1}, {"_g_fp", 0xFE12A, 4, 1}, {"_g_stub_val", 0xFE12E, 2, 1},
    {"_TEST_time_us", 0xFE130, 4, 1}, {"_g_ms_tick", 0xFE134, 4, 1}, {"_TEST_cycle_cnt", 0xFE138, 2, 1},
    {"_s_i@1", 0xFE13A, 2, 0}, {"_sub@1@TEST_cycle", 0xFE13C, 1, 0}, {"_g_isr_cnt", 0xFE13D, 1, 1},
    {"_g_st", 0xFE13E, 6, 1}, {"_g_w16", 0xFE144, 2, 1}, {"_s_cnt@1", 0xFE146, 1, 0}, {"_s_cnt@2", 0xFE147, 1, 0},
    {"_calls@1@cnt_call", 0xFE148, 2, 0}, {"_gp_st", 0xFE14A, 2, 1}, {"_g_e", 0xFE14C, 1, 1}, {"_g_e2", 0xFE14D, 1, 1},
    {"_g_bool", 0xFE14E, 1, 1}, {"_g_mat", 0xFE14F, 6, 1}, {"_g_u", 0xFE156, 2, 1}, {"_g_cal", 0xFE158, 4, 1},
    {"_g_vd", 0xFE15C, 2, 1}, {"_g_init16", 0xFE000, 2, 1}, {"_g_init8", 0xFE002, 1, 1}, {"_g_initarr", 0xFE003, 3, 1},
    {"_gp_init", 0xFE006, 2, 1}, {"_gfp_init", 0xFE008, 4, 1}, {"_g_sd", 0xFFE40, 2, 1}, {"_g_lut", 0x2000, 8, 1},
    {"_g_name", 0x2008, 8, 1},
};
#define NDS (sizeof DS / sizeof DS[0])

static void write_map(const char *path)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) { fprintf(stderr, "mkabs: %s を書けません\n", path); exit(2); }
    fprintf(f, "Renesas Optimizing Linker (mkabs test generator)\n\n*** Options ***\n\n-nologo\n\n*** Error information ***\n\n");
    fprintf(f, "*** Mapping List ***\n\nSECTION                            START      END         SIZE   ALIGN\n");
    for (i = 1; i < (size_t)NS; i++) {
        if (!(S[i].flags & 2)) continue;
        fprintf(f, "%s\n                                  %08x  %08x  %8x   %u\n", S[i].name, S[i].addr,
                S[i].addr + (S[i].size ? S[i].size - 1 : 0), S[i].size, S[i].align);
    }
    fprintf(f, "\n*** Symbol List ***\n\nSECTION=\nFILE=                               START        END    SIZE\n");
    fprintf(f, "  SYMBOL                            ADDR     SIZE    INFO      COUNTS  OPT\n\n");
    fprintf(f, "SECTION=.bss\nFILE=DefaultBuild\\engine.obj\n                                  000fe100  000fe160        60\n");
    for (i = 0; i < NDS; i++) {
        if (!strcmp(DS[i].sym, "_s_cnt@2")) continue;
        fprintf(f, "  %s\n                                  %08lx  %8lx   data ,%c         0\n", DS[i].sym, DS[i].addr, DS[i].size, DS[i].global ? 'g' : 'l');
    }
    fprintf(f, "FILE=DefaultBuild\\sensor.obj\n                                  000fe147  000fe147         1\n");
    fprintf(f, "  _s_cnt@2\n                                  000fe147         1   data ,l         0\n");
    fprintf(f, "FILE=DefaultBuild\\engine.obj\n");
    /* .symtab にない、マップだけのシンボル（1 行に収まる形） */
    fprintf(f, "  _g_maponly                      000fe15e         2   data ,g         0\n");
    fprintf(f, "SECTION=.text\nFILE=DefaultBuild\\engine.obj\n");
    for (i = 0; i < NFN; i++)
        fprintf(f, "  _%s\n                                  %08lx  %8lx   func ,g         0\n", FN[i].name, FN[i].addr, fend(FN[i].addr) - FN[i].addr);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *kind, *out;
    int startup = 0, checksum = 0, boundary = 0;
    unsigned char vector[2];
    int nodw = 0, i, sh_symtab, sh_strtab;
    FILE *f;
    buf file = {0};
    size_t phoff, shoff;
    int nload = 0;
    if (argc != 3) { fprintf(stderr, "使い方: mkabs dwarf2|dwarf3|dwarf4|nodwarf 出力.abs\n"); return 2; }
    kind = argv[1];
    out = argv[2];
    if (!strcmp(kind, "startup")) { V = 2; startup = 1; }
    else if (!strcmp(kind, "boundary")) { V = 2; boundary = 1; }
    else if (!strcmp(kind, "checksum")) { V = 2; checksum = 1; }
    else if (!strcmp(kind, "dwarf2")) V = 2;
    else if (!strcmp(kind, "dwarf3")) V = 3;
    else if (!strcmp(kind, "dwarf4")) V = 4;
    else if (!strcmp(kind, "nodwarf")) nodw = 1;
    else { fprintf(stderr, "mkabs: 種類 %s は分かりません\n", kind); return 2; }

    add_sect("", 0, 0, 0, NULL, 0);
    vector[0] = (unsigned char)TP_trace_boot; vector[1] = (unsigned char)(TP_trace_boot >> 8);
    if (checksum) { vector[0] = (unsigned char)TP_rom_check; vector[1] = (unsigned char)(TP_rom_check >> 8); }
    if (boundary) { vector[0] = (unsigned char)TP_boundary_boot; vector[1] = (unsigned char)(TP_boundary_boot >> 8); }
    add_sect(".vect", 1, 2, 0, (startup || checksum || boundary) ? vector : TP_IMAGE, 2);
    add_sect(".text", 1, 6, 0x100, TP_IMAGE + 0x100, (unsigned)(TP_code_end - 0x100));
    add_sect(".data", 1, 2, 0x1800, TP_IMAGE + 0x1800, (unsigned)(TP_data_rom_end - TP_data_rom));
    add_sect(".sdata", 1, 2, 0x1840, TP_IMAGE + 0x1840, (unsigned)(TP_sdata_rom_end - TP_sdata_rom));
    add_sect(".const", 1, 2, 0x2000, TP_IMAGE + 0x2000, (unsigned)(TP_const_end - TP_const_rom));
    add_sect(".dataR", 8, 3, 0xFE000, NULL, 12);
    add_sect(".bss", 8, 3, 0xFE100, NULL, 0x60);
    add_sect(".sdataR", 8, 3, 0xFFE40, NULL, 2);
    for (i = 1; i < NS; i++) S[i].align = S[i].type == 8 || i >= 3 ? 2 : 1;

    /* .symtab */
    putstr(&strtab, "");
    sym("", 0, 0, 0, 0, 0);
    if (startup) { sym("_trace_boot", TP_trace_boot, TP_wait_sfr - TP_trace_boot, 1, 2, 2); sym("_wait_sfr", TP_wait_sfr, TP_code_end - TP_wait_sfr, 1, 2, 2); }
    if (boundary) { sym("_boundary_end", TP_boundary_end, 1, 1, 2, 2); sym("_boundary_short", TP_boundary_short, 1, 1, 2, 2); }
    sym("testprog.s", 0, 0, 0, 4, 0xfff1);                 /* STT_FILE */
    sym("_intr_tm00@1", TP_intr_tm00, fend(TP_intr_tm00) - TP_intr_tm00, 0, 2, 2);
    sym("_s_i@1", 0xFE13A, 2, 0, 1, 7);
    sym("_sub@1@TEST_cycle", 0xFE13C, 1, 0, 1, 7);
    sym("_s_cnt@1", 0xFE146, 1, 0, 1, 7);
    sym("_s_cnt@2", 0xFE147, 1, 0, 1, 7);
    sym("_calls@1@cnt_call", 0xFE148, 2, 0, 1, 7);
    sym(".Lbss", TP_start + 7, 0, 0, 0, 2);                  /* 局所ラベル（無視されるべき） */
    for (i = 0; i < (int)NFN; i++) {
        char nm[64];
        if (!strcmp(FN[i].name, "intr_tm00")) continue;
        snprintf(nm, sizeof nm, "_%s", FN[i].name);
        sym(nm, FN[i].addr, (!strcmp(FN[i].name, "start") || !strcmp(FN[i].name, "exit")) ? 0 : fend(FN[i].addr) - FN[i].addr,
            1, (!strcmp(FN[i].name, "start") || !strcmp(FN[i].name, "exit")) ? 0 : 2, 2);
    }
    for (i = 0; i < (int)NDS; i++) {
        if (!DS[i].global) continue;
        sym(DS[i].sym, DS[i].addr, DS[i].size, 1, 1, DS[i].addr < 0x1800 ? 2 : DS[i].addr < 0x2000 ? 3 : DS[i].addr < 0xF0000 ? 5 : DS[i].addr < 0xFE100 ? 6 : DS[i].addr < 0xFFE00 ? 7 : 8);
    }
    sym("__RAM_ADDR_START", 0xFDF00, 0, 1, 0, 0xfff1);
    sym("__RAM_ADDR_END", 0xFFEE0, 0, 1, 0, 0xfff1);
    sym("__STACK_ADDR_START", 0xFFE20, 0, 1, 0, 0xfff1);
    sym("__STACK_ADDR_END", 0xFF000, 0, 1, 0, 0xfff1);
    sh_symtab = add_sect(".symtab", 2, 0, 0, NULL, 0);
    sh_strtab = add_sect(".strtab", 3, 0, 0, NULL, 0);
    S[sh_symtab].data = symtab.p;
    S[sh_symtab].size = (unsigned)symtab.n;
    S[sh_symtab].link = (unsigned)sh_strtab;
    S[sh_symtab].info = 9;      /* 最初のグローバルの添字（おおよそ。読み手は使わない） */
    S[sh_symtab].entsize = 16;
    S[sh_symtab].align = 4;
    S[sh_strtab].data = strtab.p;
    S[sh_strtab].size = (unsigned)strtab.n;
    if (!nodw) {
        build_dwarf();
        add_sect(".debug_info", 1, 0, 0, info.p, (unsigned)info.n);
        put8(&abbr, 0);
        add_sect(".debug_abbrev", 1, 0, 0, abbr.p, (unsigned)abbr.n);
        if (dstr.n) add_sect(".debug_str", 1, 0x30, 0, dstr.p, (unsigned)dstr.n);
    }
    i = add_sect(".shstrtab", 3, 0, 0, NULL, 0);
    putstr(&shstr, "");
    {
        int k;
        static unsigned nameoff[32];
        for (k = 1; k < NS; k++) { nameoff[k] = (unsigned)shstr.n; putstr(&shstr, S[k].name); }
        S[i].data = shstr.p;
        S[i].size = (unsigned)shstr.n;
        /* ELF を組み立てる */
        for (k = 1; k < NS; k++) if ((S[k].flags & 2)) nload++;
        phoff = 52;
        {
            size_t pos = phoff + (size_t)nload * 32;
            unsigned char hdr[52];
            memset(hdr, 0, sizeof hdr);
            for (k = 1; k < NS; k++) {
                if (S[k].type == 8) { S[k].off = pos; continue; }
                if (S[k].align > 1) pos = (pos + S[k].align - 1) & ~(size_t)(S[k].align - 1);
                S[k].off = pos;
                pos += S[k].size;
            }
            shoff = (pos + 3) & ~(size_t)3;
            memcpy(hdr, "\x7f" "ELF", 4);
            hdr[4] = 1; hdr[5] = 1; hdr[6] = 1;
            put(&file, hdr, 16);
            put16(&file, 2);            /* ET_EXEC */
            put16(&file, 197);          /* EM_RL78 */
            put32(&file, 1);
            put32(&file, 0);            /* e_entry */
            put32(&file, (unsigned long)phoff);
            put32(&file, (unsigned long)shoff);
            put32(&file, 0);            /* e_flags */
            put16(&file, 52);
            put16(&file, 32);
            put16(&file, (unsigned)nload);
            put16(&file, 40);
            put16(&file, (unsigned)NS);
            put16(&file, (unsigned)i);
            for (k = 1; k < NS; k++) {
                if (!(S[k].flags & 2)) continue;
                put32(&file, 1);                                     /* PT_LOAD */
                put32(&file, S[k].type == 8 ? 0 : (unsigned long)S[k].off);
                put32(&file, S[k].addr);
                put32(&file, S[k].addr);
                put32(&file, S[k].type == 8 ? 0 : S[k].size);
                put32(&file, S[k].size);
                put32(&file, (S[k].flags & 4) ? 5 : (S[k].flags & 1) ? 6 : 4);
                put32(&file, S[k].align);
            }
            for (k = 1; k < NS; k++) {
                if (S[k].type == 8) continue;
                while (file.n < S[k].off) put8(&file, 0);
                put(&file, S[k].data, S[k].size);
            }
            while (file.n < shoff) put8(&file, 0);
            for (k = 0; k < NS; k++) {
                put32(&file, k ? nameoff[k] : 0);
                put32(&file, S[k].type);
                put32(&file, S[k].flags);
                put32(&file, S[k].addr);
                put32(&file, (unsigned long)S[k].off);
                put32(&file, S[k].size);
                put32(&file, S[k].link);
                put32(&file, S[k].info);
                put32(&file, S[k].align);
                put32(&file, S[k].entsize);
            }
        }
    }
    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "mkabs: %s を書けません\n", out); return 2; }
    fwrite(file.p, 1, file.n, f);
    fclose(f);
    if (nodw) {
        char mp[4096];
        size_t n = strlen(out);
        const char *dot = strrchr(out, '.');
        if (dot && (size_t)(dot - out) < sizeof mp - 5) n = (size_t)(dot - out);
        snprintf(mp, sizeof mp, "%.*s.map", (int)n, out);
        write_map(mp);
    }
    return 0;
}
