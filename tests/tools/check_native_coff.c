/* check_native_coff.c - 手で組み立てた COFF の境界ケース */
#include "native_coff.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned char data[4096], strings[1024];
    size_t n, size;
} object;
typedef struct { const char *name; int defined, function; } expected;
typedef struct { const expected *items; int n, seen, failed; } visits;
static int cases, failed;

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v)
{
    put16(p, v & 65535u); put16(p + 2, v >> 16);
}
static void init(object *o)
{
    memset(o, 0, sizeof *o);
    put16(o->data, 0x8664);
    put32(o->data + 8, 20);
    o->size = 4;
}
static void symbol(object *o, const char *name, unsigned section, uint32_t value,
                   unsigned type, unsigned storage, unsigned aux)
{
    unsigned char *s = o->data + 20 + o->n * 18;
    size_t len = strlen(name);
    if (len <= 8) memcpy(s, name, len);
    else {
        put32(s + 4, (uint32_t)o->size);
        memcpy(o->strings + o->size, name, len + 1);
        o->size += len + 1;
    }
    put32(s + 8, value);
    put16(s + 12, section);
    put16(s + 14, type);
    s[16] = (unsigned char)storage;
    s[17] = (unsigned char)aux;
    o->n += (size_t)aux + 1;
}
static size_t finish(object *o)
{
    size_t start = 20 + o->n * 18;
    put32(o->data + 12, (uint32_t)o->n);
    put32(o->strings, (uint32_t)o->size);
    memcpy(o->data + start, o->strings, o->size);
    return start + o->size;
}
static void visit(void *ctx, const char *name, int defined, int function)
{
    visits *v = ctx;
    if (v->seen >= v->n) v->failed = 1;
    else {
        const expected *e = &v->items[v->seen];
        if (strcmp(name, e->name) || defined != e->defined || function != e->function) v->failed = 1;
    }
    v->seen++;
}
static void check(const char *name, const unsigned char *data, size_t len,
                  const expected *items, int n, int bad)
{
    visits v = {items, n, 0, 0};
    char err[200] = "";
    int rc = native_coff_symbols(data, len, visit, &v, err, sizeof err);
    cases++;
    if ((bad && (rc == 0 || !err[0])) || (!bad && (rc != 0 || v.failed || v.seen != n))) {
        fprintf(stderr, "失敗: %s\n", name);
        failed++;
    }
}
/* ---- LLVM IR の宣言 ---- */
typedef struct { const char *name; int function; unsigned long size; int ret; } ll_expected;
typedef struct { const ll_expected *items; int n, seen, failed; } ll_visits;
static void ll_visit(void *ctx, const char *name, int is_function, unsigned long size, int ret)
{
    ll_visits *v = ctx;
    if (v->seen >= v->n) {
        fprintf(stderr, "  余分: %s\n", name);
        v->failed = 1;
    } else {
        const ll_expected *e = &v->items[v->seen];
        if (strcmp(name, e->name) || is_function != e->function || size != e->size || ret != e->ret) {
            fprintf(stderr, "  違う: %s %d %lu %d（期待 %s %d %lu %d）\n", name, is_function, size, ret,
                    e->name, e->function, e->size, e->ret);
            v->failed = 1;
        }
    }
    v->seen++;
}
static void check_ll_text(const char *name, const char *text, const ll_expected *items, int n)
{
    ll_visits v = {items, n, 0, 0};
    native_ll_decls(text, strlen(text), ll_visit, &v);
    cases++;
    if (v.failed || v.seen != n) {
        fprintf(stderr, "失敗: %s\n", name);
        failed++;
    }
}
static void check_ll(void)
{
    static const char text[] =
        "; ModuleID = 'a.pp.c'\n"
        "target triple = \"x86_64-pc-windows-msvc\"\n"
        "@g = dso_local global i16 4660, align 2\n"
        "@str = private unnamed_addr constant [4 x i8] c\"abc\\00\", align 1\n"
        "@t = external dso_local global [8 x i8], align 1\n"
        "@w = external dso_local global i16, align 2\n"
        "@p = external global ptr\n"
        "@s = external global %struct.pt, align 4\n"
        "@u = external global [0 x i8]\n"
        "@m = external dso_local global [3 x [5 x i32]], align 16\n"
        "@asm_t = external dso_local global i8, align 1\n"
        "@asm_table = external dso_local global [16 x i8], align 1\n"
        "@\"q name\" = external global i32\r\n"
        "@\"esc\\41pe\" = extern_weak global float\n"
        "@k = external constant double\n"
        "@odd = external global i17\n"
        "@big = external global [4294967296 x [4294967296 x i8]]\n"
        "@tls = external thread_local global i64, align 8\n"
        "\n"
        "define dso_local i16 @user(i8 noundef %0) #0 {\n"
        "  %2 = call i16 @f(i8 noundef zeroext 1)\n"
        "  ret i16 0\n"
        "}\n"
        "declare dso_local signext i16 @f(i8 noundef zeroext) #1\n"
        "declare dso_local void @start_asm() #1\n"
        "declare dso_local i32 @asm_implicit(...) #1\n"
        "declare void @\"quoted fn\"(ptr)\n"
        "declare dso_local void @asm_t2()";      /* 最後の行に改行がない */
    static const ll_expected items[] = {
        {"t", 0, 8, NATIVE_RET_INT}, {"w", 0, 2, NATIVE_RET_INT}, {"p", 0, 8, NATIVE_RET_INT}, {"s", 0, 0, NATIVE_RET_INT}, {"u", 0, 0, NATIVE_RET_INT}, {"m", 0, 60, NATIVE_RET_INT},
        {"asm_t", 0, 1, NATIVE_RET_INT}, {"asm_table", 0, 16, NATIVE_RET_INT}, {"q name", 0, 4, NATIVE_RET_INT}, {"escApe", 0, 4, NATIVE_RET_INT}, {"k", 0, 8, NATIVE_RET_INT},
        {"odd", 0, 3, NATIVE_RET_INT}, {"big", 0, 0, NATIVE_RET_INT}, {"tls", 0, 8, NATIVE_RET_INT},
        {"f", 1, 0, NATIVE_RET_INT}, {"start_asm", 1, 0, NATIVE_RET_INT}, {"asm_implicit", 1, 0, NATIVE_RET_INT}, {"quoted fn", 1, 0, NATIVE_RET_INT}, {"asm_t2", 1, 0, NATIVE_RET_INT}
    };
    static const ll_expected none[] = {{"", 0, 0, NATIVE_RET_INT}};
    check_ll_text("LLVM IR の宣言", text, items, (int)(sizeof items / sizeof items[0]));
    check_ll_text("定義だけ", "@g = dso_local global i16 4660\ndefine void @h() {\n}\n", none, 0);
    check_ll_text("壊れた行", "@\"unterminated = external global i8\n@ = external global i8\n"
                             "@x external global i8\n@\"nul\\00\" = external global i8\ndeclare void @\n"
                             "@y = external global [3 x\n@z = external global [3 i8]\n",
                  (const ll_expected[]){{"y", 0, 0, NATIVE_RET_INT}, {"z", 0, 0, NATIVE_RET_INT}}, 2);
    check_ll_text("空", "", none, 0);
    {
        static const char rets[] =
            "declare dso_local double @d(double noundef)\n"
            "declare dso_local float @f(float noundef)\n"
            "declare dso_local signext i16 @s(i8 noundef zeroext) #1\n"
            "declare dso_local void @v()\n"
            "declare dso_local ptr @p(ptr noundef)\n"
            "declare dso_local i32 @implicit(...)\n"
            "declare dso_local x86_fp80 @ld()\n"
            "declare dso_local void @sr(ptr dead_on_unwind writable sret(%struct.pt) align 4)\n"
            "declare dso_local noundef i32 @n()\n"
            "declare dso_local double @\"q fp\"(i32)\n"
            "declare dllimport x86_64_win64cc noundef range(i8 0, 2) zeroext i8 @r()\n"
            "declare dso_local noundef nonnull align 8 dereferenceable(16) ptr @a()\n"
            "declare dso_local <4 x float> @vec()\n"
            "declare dso_local { i64, i64 } @pair()\n"
            "declare dso_local %struct.pt @byval_ret()\n"
            "declare dso_local half @h()\n"
            "declare dso_local void @nosret(ptr noundef) #2 ; mysret(\n";
        static const ll_expected items[] = {
            {"d", 1, 0, NATIVE_RET_F64}, {"f", 1, 0, NATIVE_RET_F32}, {"s", 1, 0, NATIVE_RET_INT},
            {"v", 1, 0, NATIVE_RET_INT}, {"p", 1, 0, NATIVE_RET_INT}, {"implicit", 1, 0, NATIVE_RET_INT},
            {"ld", 1, 0, NATIVE_RET_OTHER}, {"sr", 1, 0, NATIVE_RET_SRET}, {"n", 1, 0, NATIVE_RET_INT},
            {"q fp", 1, 0, NATIVE_RET_F64}, {"r", 1, 0, NATIVE_RET_INT}, {"a", 1, 0, NATIVE_RET_INT},
            {"vec", 1, 0, NATIVE_RET_OTHER}, {"pair", 1, 0, NATIVE_RET_OTHER},
            {"byval_ret", 1, 0, NATIVE_RET_OTHER}, {"h", 1, 0, NATIVE_RET_OTHER}, {"nosret", 1, 0, NATIVE_RET_INT}
        };
        check_ll_text("戻り値の種類", rets, items, (int)(sizeof items / sizeof items[0]));
    }
}

int main(void)
{
    object o;
    size_t len;
    static const expected items[] = {
        {"short", 1, 1}, {"exactly8", 1, 1}, {"long_function", 1, 1},
        {"undef_f", 0, 1}, {"undef_v", 0, 0}, {"common", 1, 0},
        {"absolute", 1, 0}, {"debug", 1, 0}, {"after", 1, 1}
    };
    init(&o);
    symbol(&o, "short", 1, 0, 0x20, 2, 0);
    symbol(&o, "exactly8", 1, 0, 0x20, 2, 0);
    symbol(&o, "long_function", 1, 0, 0x20, 2, 0);
    symbol(&o, "undef_f", 0, 0, 0x20, 2, 0);
    symbol(&o, "undef_v", 0, 0, 0, 2, 0);
    symbol(&o, "common", 0, 8, 0, 2, 0);
    symbol(&o, "static", 1, 0, 0x20, 3, 0);
    symbol(&o, "weak", 0, 0, 0, 105, 1);
    symbol(&o, "absolute", 65535, 0, 0, 2, 0);
    symbol(&o, "debug", 65534, 0, 0, 2, 0);
    symbol(&o, "after", 1, 0, 0x20, 2, 2);
    /* 補助シンボルが外部に見えても通知しない。 */
    o.data[20 + (o.n - 1) * 18 + 16] = 2;
    len = finish(&o);
    check("外部の種類と名前、補助シンボル", o.data, len, items, 9, 0);
    {
        size_t i;
        for (i = 0; i < len; i++) check("切り詰め", o.data, i, NULL, 0, 1);
    }
    put32(o.data + 8, UINT32_MAX);
    check("シンボル表の位置", o.data, len, NULL, 0, 1);
    put32(o.data + 8, 20);
    put32(o.data + 12, UINT32_MAX);
    check("シンボル数のあふれ", o.data, len, NULL, 0, 1);
    put32(o.data + 12, (uint32_t)o.n);
    put32(o.data + 20 + 2 * 18 + 4, UINT32_MAX);
    check("文字列の位置", o.data, len, NULL, 0, 1);
    put32(o.data + 20 + 2 * 18 + 4, 3);
    check("文字列の先頭より前", o.data, len, NULL, 0, 1);
    put32(o.data + 20 + 2 * 18 + 4, 4);
    o.data[len - 1] = 'x';
    check("文字列の終端なし", o.data, len, NULL, 0, 1);
    o.data[len - 1] = 0;
    o.data[20 + (o.n - 3) * 18 + 17] = 3;
    check("補助シンボルが表の外", o.data, len, NULL, 0, 1);
    put16(o.data, 0x14c);
    check("別の機械", o.data, len, NULL, 0, 1);
    put16(o.data, 0); put16(o.data + 2, 65535);
    check("bigobj", o.data, len, NULL, 0, 1);
    init(&o);
    len = finish(&o);
    check("空の表", o.data, len, NULL, 0, 0);
    put32(o.data + 8, 0);
    check("表なし", o.data, 20, NULL, 0, 0);
    check("NULL", NULL, 20, NULL, 0, 1);
    check_ll();
    printf("native coff: %d cases, %d failed\n", cases, failed);
    return failed ? 1 : 0;
}
