/* check_native_rewrite.c - 製品ソースの書き直しの境界ケース */
#include "native_rewrite.h"
#include <stdio.h>
#include <string.h>

static const struct { const char *in, *out; } cases[] = {
    {"int a;", "short a;"},
    {"unsigned int a;", "unsigned short a;"},
    {"long int a;", "long a;"},
    {"unsigned long int a;", "unsigned long a;"},
    {"int long a;", "long a;"},
    {"short int a;", "short a;"},
    {"signed short int a;", "signed short a;"},
    {"const int a;", "const short a;"},
    {"static int f(int x)", "static short f(short x)"},
    {"unsigned x;", "unsigned short x;"},
    {"signed y;", "signed short y;"},
    {"unsigned char c;", "unsigned char c;"},
    {"long l;", "long l;"},
    {"unsigned long ul;", "unsigned long ul;"},
    {"sizeof(int)", "sizeof(short)"},
    {"(int)x", "(short)x"},
    {"(unsigned)x", "(unsigned short)x"},
    {"char *s = \"int long int\"; 'i';", "char *s = \"int long int\"; 'i';"},
    {"L\"int\"; u8\"int\"; U'int';", "L\"int\"; u8\"int\"; U'int';"},
    {"int interval; print(); mint;", "short interval; print(); mint;"},
    {"0x1int 1int .1int 1e+int 0x1p-int", "0x1int 1int .1int 1e+int 0x1p-int"},
    {"__asm(\"di\");", ";"},
    {"__asm(\"extrn _foo\"); int x;", "; short x;"},
    {"__asm(\"mov a, #')'\");", ";"},
    {"__asm(\"x(\\\"y\\\")\");", ";"},
    {"__asm(\n\"di\"\n);\nint x;", "\n\n;\nshort x;"},
    {"__asm\r\n(\"di\");", "\r\n;"},
    {"__asm((\"(\"), ')');", ";"},
    {"__asm x;", "__asm x;"},
    {"__asm(\"di\";", "__asm(\"di\";"},
    {"# 1 \"C:\\clang\\lib\\clang\\18\\include\\stdint.h\" 1 3\ntypedef int x;\n# 5 \"a.c\" 2\nint y;",
     "# 1 \"C:\\clang\\lib\\clang\\18\\include\\stdint.h\" 1 3\ntypedef int x;\n# 5 \"a.c\" 2\nshort y;"},
    {"  #pragma int __asm(\"di\")\nint x;", "  #pragma int __asm(\"di\")\nshort x;"},
    {"long\nint x; unsigned\nint y;", "long\nx; unsigned\nshort y;"},
    {"__signed__ x; __const int y; __volatile__ unsigned z;", "__signed__ short x; __const short y; __volatile__ unsigned short z;"},
    {"static x; f() {} const x;", "static x; f() {} const x;"},
    {"# 1 \"sys.h\" 3\n#pragma int\nint x; __asm(\"di\");\n# 2 \"a.c\"\nint y;",
     "# 1 \"sys.h\" 3\n#pragma int\nint x; __asm(\"di\");\n# 2 \"a.c\"\nshort y;"},
    /* 型の語の並びの判定 */
    {"long long int a; long long b;", "long long a; long long b;"},
    {"long double d; short s; signed char c; unsigned char u;", "long double d; short s; signed char c; unsigned char u;"},
    {"void f(unsigned, int);", "void f(unsigned short, short);"},
    {"sizeof(unsigned) + sizeof(signed)", "sizeof(unsigned short) + sizeof(signed short)"},
    {"(unsigned long)x; (unsigned char)y; (const unsigned *)p;", "(unsigned long)x; (unsigned char)y; (const unsigned short *)p;"},
    {"typedef unsigned int UINT; extern volatile unsigned v;", "typedef unsigned short UINT; extern volatile unsigned short v;"},
    {"unsigned const x; int unsigned y; char signed z;", "unsigned short const x; short unsigned y; char signed z;"},
    {"struct s { unsigned a:3; int b:2; };", "struct s { unsigned short a:3; short b:2; };"},
    {"unsigned\t \n  int x; long  \n\n int y;", "unsigned\t \n  short x; long  \n\n y;"},
    /* clang -E は空行が続くと宣言の途中にも行の情報を出す */
    {"unsigned\n# 14 \"a.c\"\n int x;", "unsigned\n# 14 \"a.c\"\n short x;"},
    {"long\n# 14 \"a.c\"\n int x;", "long\n# 14 \"a.c\"\n x;"},
    {"__asm(\n# 9 \"a.c\"\n\"di\");", "\n# 9 \"a.c\"\n;"},
    {"__asm\n# 14 \"a.c\"\n(\"di\");", "\n# 14 \"a.c\"\n;"},
    /* GNU の修飾子の別名と、別の型の語 */
    {"unsigned __const__ char v;", "unsigned __const__ char v;"},
    {"unsigned __volatile char v;", "unsigned __volatile char v;"},
    {"__const__ int a;", "__const__ short a;"},
    {"unsigned __volatile__ x;", "unsigned short __volatile__ x;"},
    {"__extension__ long long int z;", "__extension__ long long z;"},
    {"unsigned __int64 q;", "unsigned __int64 q;"},
    {"long double d;", "long double d;"},
    {"const void *p;", "const void *p;"},
    {"__signed__ char c;", "__signed__ char c;"},
    {"__signed__ w;", "__signed__ short w;"},
    {"__signed w; signed __int8 b; unsigned _Bool f; long long int g; unsigned long long int h;",
     "__signed short w; signed __int8 b; unsigned _Bool f; long long g; unsigned long long h;"},
    {"unsigned * __restrict__ p; int * restrict q; static __inline__ unsigned f(void);",
     "unsigned short * __restrict__ p; short * restrict q; static __inline__ unsigned short f(void);"},
    /* 型の語の途中にある属性などの拡張の語は読み飛ばす（出力からは消さない） */
    {"unsigned __attribute__((aligned(1))) char value;", "unsigned __attribute__((aligned(1))) char value;"},
    {"unsigned __attribute__((aligned(1))) value;", "unsigned short __attribute__((aligned(1))) value;"},
    {"__attribute__((unused)) int a;", "__attribute__((unused)) short a;"},
    {"int __attribute__((unused)) a;", "short __attribute__((unused)) a;"},
    {"long __attribute__((unused)) int b;", "long __attribute__((unused)) b;"},
    {"unsigned __declspec(align(2)) char c;", "unsigned __declspec(align(2)) char c;"},
    {"unsigned __cdecl f(int a);", "unsigned short __cdecl f(short a);"},
    {"unsigned __tmp;", "unsigned short __tmp;"},
    {"unsigned __f(int a) { return a; }", "unsigned short __f(short a) { return a; }"},
    {"int __x = (int)__y;", "short __x = (short)__y;"},
    {"unsigned __int128 q;", "unsigned __int128 q;"},
    {"unsigned __asm(\"x\") y;", "unsigned short y;"},
    {"unsigned __attribute__\n# 5 \"a.c\"\n((aligned(1))) char v;", "unsigned __attribute__\n# 5 \"a.c\"\n((aligned(1))) char v;"},
    {"unsigned __attribute__((", "unsigned short __attribute__(("},
    {"unsigned __attribute__((x(\")\", ')'))) char v;", "unsigned __attribute__((x(\")\", ')'))) char v;"},
    {"unsigned _Alignas(4) char a; unsigned __attribute__((a)) __attribute__((b)) char b;",
     "unsigned _Alignas(4) char a; unsigned __attribute__((a)) __attribute__((b)) char b;"},
    {"unsigned __attribute__((aligned(sizeof(int)))) x;", "unsigned short __attribute__((aligned(sizeof(short)))) x;"},
    /* 以前のマクロ（#define int short）と比べる形 */
    {"typeof(int) a; __typeof__(unsigned) b; _Atomic(unsigned) c; _Atomic unsigned d; unsigned _Atomic e;",
     "typeof(short) a; __typeof__(unsigned short) b; _Atomic(unsigned short) c; _Atomic unsigned short d; unsigned short _Atomic e;"},
    {"x = (unsigned){1} + (int){2};", "x = (unsigned short){1} + (short){2};"},
    {"f(a, b) unsigned a; int b; { }", "f(a, b) unsigned short a; short b; { }"},
    {"enum e { A } unsigned_e; enum e x; unsigned enum_cnt; struct s { unsigned :3; int; } v;",
     "enum e { A } unsigned_e; enum e x; unsigned short enum_cnt; struct s { unsigned short :3; short; } v;"},
    {"unsigned\n#pragma pack(1)\nchar c; unsigned\n# 3 \"a.c\"\n\n x;", "unsigned\n#pragma pack(1)\nchar c; unsigned short\n# 3 \"a.c\"\n\n x;"},
    {"register unsigned r __asm__(\"r1\"); int g(void) __asm__(\"_g\");", "register unsigned short r __asm__(\"r1\"); short g(void) __asm__(\"_g\");"},
    {"_Static_assert(sizeof(int) == 2, \"x\"); unsigned __near *p; unsigned _Noreturn f(void);",
     "_Static_assert(sizeof(short) == 2, \"x\"); unsigned short __near *p; unsigned short _Noreturn f(void);"},
    {"int restrict; unsigned restrict2; short int unsigned u;", "short restrict; unsigned short restrict2; short unsigned u;"},
    {"__builtin_va_arg(ap, int); (unsigned)__builtin_expect(x, 1);", "__builtin_va_arg(ap, short); (unsigned short)__builtin_expect(x, 1);"},
    /* 入力の終わりと閉じていないもの */
    {"", ""},
    {"int", "short"},
    {"unsigned", "unsigned short"},
    {"L", "L"},
    {"u8", "u8"},
    {"\"abc", "\"abc"},
    {"\"abc\\", "\"abc\\"},
    {"'", "'"},
    {"#", "#"},
    {"# 1 \"a.h", "# 1 \"a.h"},
    {"__asm", "__asm"},
    {"__asm(", "__asm("},
    {"__asm((\"x\")", "__asm((\"x\")"},
    {"char c = 'x\nint y;", "char c = 'x\nshort y;"},
    {"__asm__(\"di\"); asm(\"di\"); __asm_x(1);", "__asm__(\"di\"); asm(\"di\"); __asm_x(1);"}
};

/* 空白の位置は比べず、識別子が接着してしまう誤りは検出する。引用符の中はそのまま比べる。 */
static void normalize(const char *s, sim_buf *b)
{
    int space = 0, word = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        int next_word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = 1; continue; }
        if (space && word && next_word) sim_buf_addc(b, ' ');
        space = 0;
        sim_buf_addc(b, (char)c);
        if (c == '"' || c == '\'') {
            char quote = (char)c;
            while (*s) {
                char x = *s++;
                sim_buf_addc(b, x);
                if (x == '\\' && *s) sim_buf_addc(b, *s++);
                else if (x == quote) break;
            }
        }
        word = next_word;
    }
}
static size_t newlines(const char *s)
{
    size_t n = 0;
    while (*s) if (*s++ == '\n') n++;
    return n;
}
int main(void)
{
    size_t i;
    int failed = 0;
    for (i = 0; i < sizeof cases / sizeof *cases; i++) {
        sim_buf out, a, b;
        native_rewrite_stats st;
        sim_buf_init(&out); sim_buf_init(&a); sim_buf_init(&b);
        native_rewrite(cases[i].in, strlen(cases[i].in), &out, &st);
        normalize(sim_buf_cstr(&out), &a); normalize(cases[i].out, &b);
        if (strcmp(sim_buf_cstr(&a), sim_buf_cstr(&b)) || newlines(cases[i].in) != newlines(sim_buf_cstr(&out))) {
            printf("ケース %zu: 出力が違います: %s\n", i + 1, sim_buf_cstr(&out));
            failed++;
        }
        sim_buf_free(&out); sim_buf_free(&a); sim_buf_free(&b);
    }
    {
        const char *in = "int x; long int y; unsigned z; __asm(\"di\"); __asm(\"ei\"); __asm\n# 14 \"a.c\"\n(\"nop\");";
        native_rewrite_stats st;
        sim_buf out;
        sim_buf_init(&out);
        native_rewrite(in, strlen(in), &out, &st);
        if (st.int_replaced != 1 || st.int_dropped != 1 || st.short_added != 1 || st.asm_removed != 3) {
            puts("統計の数が違います"); failed++;
        }
        sim_buf_free(&out);
    }
    printf("native rewrite: %zu cases, %d failed\n", sizeof cases / sizeof *cases + 1, failed);
    return failed ? 1 : 0;
}
