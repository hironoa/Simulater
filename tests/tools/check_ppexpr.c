/* check_ppexpr.c - #if の条件式の評価器（runtime/src/sim_ppexpr.c）の境界ケース。
 * 失敗した件数を表示し、1 件でもあれば 1 で終わる。 */
#include "sim_ppexpr.h"

#include <stdio.h>
#include <string.h>

static const struct { const char *name, *body; int kind; } macros[] = {
    {"ONE", "1", SIM_PP_OBJECT}, {"TWO", "(ONE + ONE)", SIM_PP_OBJECT}, {"EMPTY", "", SIM_PP_OBJECT},
    {"OPEN", "(1 + 2", SIM_PP_OBJECT}, {"CLOSE", "1 + 2)", SIM_PP_OBJECT},
    {"SUM", "1 + 1", SIM_PP_OBJECT},            /* 括弧なし: トークン列として展開する */
    {"SELF", "SELF", SIM_PP_OBJECT},            /* 自己参照は展開せず 0 */
    {"PING", "PONG", SIM_PP_OBJECT}, {"PONG", "PING", SIM_PP_OBJECT},
    {"PRODUCT", "2", SIM_PP_OBJECT}, {"TYPE_B", "2", SIM_PP_OBJECT},
    {"ADDR", "0xFF91CUL", SIM_PP_OBJECT}, {"TEXT", "\"abc\"", SIM_PP_OBJECT},
    {"FN", NULL, SIM_PP_FUNCTION}, {"HALF", "1 +", SIM_PP_OBJECT},
    /* 関数形式マクロ: 本体は「(仮引数) 置き換えるトークン列」。FN は本体が分からないもの */
    {"ABC", "1000", SIM_PP_OBJECT}, {"DEF", "24", SIM_PP_OBJECT}, {"GHI", "2", SIM_PP_OBJECT},
    {"CNT", "(ch)                 (((ABC              +DEF)-1)/GHI)", SIM_PP_FUNCTION},
    {"DBL", "(x) ((x) * 2)", SIM_PP_FUNCTION}, {"ADD", "(a, b) ((a) + (b))", SIM_PP_FUNCTION},
    {"RAW", "(x) x * 2", SIM_PP_FUNCTION},              /* 括弧なし: トークン列として置き換える */
    {"NONE", "() 7", SIM_PP_FUNCTION}, {"HEX", "(F) (0x1F + F)", SIM_PP_FUNCTION},
    {"VAR", "(a, ...) (a)", SIM_PP_FUNCTION}, {"CAT", "(a, b) a ## b", SIM_PP_FUNCTION},
    {"STR", "(a) #a", SIM_PP_FUNCTION}, {"LOOP", "(x) LOOP(x)", SIM_PP_FUNCTION},
    {"DIV", "(a, b) ((a) / (b))", SIM_PP_FUNCTION}, {"CH", "(c) ((c) == ',')", SIM_PP_FUNCTION},
    {"D0", "D1", SIM_PP_OBJECT}, {"D1", "D2", SIM_PP_OBJECT}, {"D2", "D3", SIM_PP_OBJECT}, {"D3", "7", SIM_PP_OBJECT}
};
static int lookups;
static char ladder[36][64];

static int lookup(void *ctx, const char *name, size_t len, const char **body, int in_defined)
{
    size_t i;
    (void)ctx; (void)in_defined;
    lookups++;
    for (i=0;i<36;i++) {
        char key[16];
        snprintf(key,sizeof key,"LIMIT%zu",i);
        if (strlen(key)==len && !memcmp(key,name,len)) { *body=ladder[i]; return SIM_PP_OBJECT; }
    }
    for (i = 0; i < sizeof macros / sizeof macros[0]; i++)
        if (strlen(macros[i].name) == len && !memcmp(macros[i].name, name, len)) {
            *body = macros[i].body;
            return macros[i].kind;
        }
    return SIM_PP_UNDEF;
}

#define BAD (-1)
/* bits = 0: sim_pp_eval（32 / 64 の両方で真偽が同じときだけ決まる）。expect は 1 / 0 / BAD
 * bits = 32 / 64: sim_pp_eval_width。ok = 0 なら value と uns を比べる */
static const struct { const char *expr; int bits, ok; long long value; int uns; } cases[] = {
    /* 定数 */
    {"0", 0, 0, 0, 0}, {"1", 0, 1, 0, 0}, {"42", 64, 0, 42, 0}, {"0x10", 64, 0, 16, 0}, {"0X1f", 64, 0, 31, 0},
    {"010", 64, 0, 8, 0}, {"0b101", 64, 0, 5, 0}, {"0B11", 64, 0, 3, 0}, {"08", 64, BAD, 0, 0},
    {"10U", 64, 0, 10, 1}, {"10UL", 64, 0, 10, 1}, {"10L", 64, 0, 10, 0}, {"10lu", 64, 0, 10, 1},
    {"10uu", 64, BAD, 0, 0}, {"0x", 64, BAD, 0, 0}, {"1.5", 64, BAD, 0, 0}, {"1e5", 64, BAD, 0, 0},
    {"12abc", 64, BAD, 0, 0}, {"'A'", 64, 0, 65, 0}, {"'\\n'", 64, 0, 10, 0}, {"'\\0'", 64, 0, 0, 0},
    {"'\\x41'", 64, 0, 65, 0}, {"'\\101'", 64, 0, 65, 0}, {"'AB'", 64, BAD, 0, 0}, {"'\\xFF'", 64, BAD, 0, 0},
    {"\"str\"", 64, BAD, 0, 0}, {"", 0, BAD, 0, 0}, {"  ", 0, BAD, 0, 0},
    /* 優先順位と結合 */
    {"1 + 2 * 3", 64, 0, 7, 0}, {"(1 + 2) * 3", 64, 0, 9, 0}, {"10 - 4 - 3", 64, 0, 3, 0},
    {"100 / 10 / 5", 64, 0, 2, 0}, {"7 % 4 * 2", 64, 0, 6, 0}, {"1 << 2 + 1", 64, 0, 8, 0},
    {"1 + 2 << 3", 64, 0, 24, 0}, {"1 < 2 == 1", 64, 0, 1, 0}, {"6 & 3 == 3", 64, 0, 0, 0},
    {"(6 & 3) == 2", 64, 0, 1, 0}, {"1 | 2 ^ 3 & 4", 64, 0, 3, 0}, {"1 || 0 && 0", 64, 0, 1, 0},
    {"(1 || 0) && 0", 64, 0, 0, 0}, {"0 ? 1 : 2 ? 3 : 4", 64, 0, 3, 0}, {"1 ? 2 : 0 ? 3 : 4", 64, 0, 2, 0},
    {"0 || 0 ? 5 : 6", 64, 0, 6, 0}, {"2 > 1 ? 10 : 20", 64, 0, 10, 0},
    /* 単項 */
    {"-1", 64, 0, -1, 0}, {"- -1", 64, 0, 1, 0}, {"!0", 64, 0, 1, 0}, {"!5", 64, 0, 0, 0}, {"!!5", 64, 0, 1, 0},
    {"~0", 64, 0, -1, 0}, {"~0U", 32, 0, 0xFFFFFFFFLL, 1}, {"+3", 64, 0, 3, 0}, {"-(2 + 3)", 64, 0, -5, 0},
    {"!-1", 64, 0, 0, 0}, {"-!0", 64, 0, -1, 0},
    /* 符号: 符号なしが混ざると符号なしで比べる */
    {"-1 < 0", 0, 1, 0, 0}, {"-1 < 0U", 0, 0, 0, 0}, {"-1 > 0U", 0, 1, 0, 0}, {"-1 == 0xFFFFFFFFU", 32, 0, 1, 0},
    {"-1 == 0xFFFFFFFFU", 64, 0, 0, 0}, {"-1 == 0xFFFFFFFFU", 0, BAD, 0, 0},
    {"-7 / 2", 64, 0, -3, 0}, {"-7 % 2", 64, 0, -1, 0}, {"-8 >> 1", 64, 0, -4, 0}, {"-8 >> 1", 32, 0, -4, 0},
    {"0 ? -1 : 0U", 64, 0, 0, 1}, {"1 ? -1 : 0U", 32, 0, 0xFFFFFFFFLL, 1},
    /* 桁あふれと幅 */
    {"0x7FFFFFFF", 32, 0, 0x7FFFFFFFLL, 0}, {"0x80000000", 32, 0, 0x80000000LL, 1},
    {"0x80000000", 64, 0, 0x80000000LL, 0}, {"2147483648", 32, 0, 2147483648LL, 1},
    {"0x100000000", 32, BAD, 0, 0}, {"0x100000000", 64, 0, 0x100000000LL, 0},
    {"0xFFFFFFFFFFFFFFFF", 64, 0, -1, 1}, {"0x10000000000000000", 64, BAD, 0, 0},
    {"18446744073709551615", 64, 0, -1, 1}, {"18446744073709551616", 64, BAD, 0, 0},
    {"0x7FFFFFFF + 1", 32, BAD, 0, 0}, {"0x7FFFFFFF + 1", 64, 0, 0x80000000LL, 0},
    {"0x7FFFFFFF + 1", 0, BAD, 0, 0}, {"0xFFFFFFFFU + 1", 32, 0, 0, 1},
    {"0xFFFFFFFF + 1", 0, BAD, 0, 0},              /* 32 ビットでは 0、64 ビットでは真 */
    {"0x7FFFFFFFFFFFFFFF + 1", 64, BAD, 0, 0}, {"0x7FFFFFFFFFFFFFFF * 2", 64, BAD, 0, 0},
    {"-0x7FFFFFFFFFFFFFFF - 2", 64, BAD, 0, 0}, {"-0x7FFFFFFFFFFFFFFF - 1", 64, 0, (-0x7FFFFFFFFFFFFFFFLL - 1), 0},
    {"(-0x7FFFFFFFFFFFFFFF - 1) / -1", 64, BAD, 0, 0}, {"(-0x7FFFFFFFFFFFFFFF - 1) % -1", 64, BAD, 0, 0},
    {"-(-0x7FFFFFFFFFFFFFFF - 1)", 64, BAD, 0, 0}, {"(-0x7FFFFFFF - 1) / -1", 32, BAD, 0, 0},
    {"65536 * 65536", 32, BAD, 0, 0}, {"65536 * 65536", 64, 0, 4294967296LL, 0}, {"65536U * 65536U", 32, 0, 0, 1},
    {"0U - 1", 32, 0, 0xFFFFFFFFLL, 1}, {"0U - 1", 64, 0, -1, 1},
    {"-2147483648 < 0", 0, BAD, 0, 0},             /* C90 では 2147483648 が符号なし */
    {"1 << 31", 32, 0, -2147483648LL, 0}, {"1 << 31", 64, 0, 2147483648LL, 0}, {"1 << 31", 0, 1, 0, 0},
    {"(1 << 31) > 0", 0, BAD, 0, 0}, {"1 << 32", 32, BAD, 0, 0}, {"1 << 63", 64, 0, (-0x7FFFFFFFFFFFFFFFLL - 1), 0},
    {"1 << 64", 64, BAD, 0, 0}, {"1 << -1", 64, BAD, 0, 0}, {"1 >> 64", 64, BAD, 0, 0},
    {"0x80000000 >> 31", 32, 0, 1, 1}, {"0xF0 >> 4", 64, 0, 15, 0},
    /* 0 除算と短絡評価 */
    {"1 / 0", 0, BAD, 0, 0}, {"1 % 0", 0, BAD, 0, 0}, {"0 && (1 / 0)", 0, 0, 0, 0}, {"1 || (1 / 0)", 0, 1, 0, 0},
    {"1 && (1 / 0)", 0, BAD, 0, 0}, {"0 || (1 / 0)", 0, BAD, 0, 0}, {"(1 / 0) && 0", 0, BAD, 0, 0},
    {"(1 / 0) || 1", 0, BAD, 0, 0}, {"1 ? 2 : (1 / 0)", 0, 1, 0, 0}, {"0 ? (1 / 0) : 0", 0, 0, 0, 0},
    {"0 ? 2 : (1 / 0)", 0, BAD, 0, 0}, {"(1 / 0) ? 1 : 1", 0, BAD, 0, 0},
    {"0 && FN(1, (2))", 0, 0, 0, 0}, {"defined(FN) && FN(1)", 0, BAD, 0, 0}, {"1 || TEXT", 0, 1, 0, 0},
    {"0 && 0x100000000", 0, 0, 0, 0}, {"0 && (0x7FFFFFFF + 1)", 0, 0, 0, 0},
    /* 短絡評価でも構文の誤りは判定不能 */
    {"0 && (1 +)", 0, BAD, 0, 0}, {"1 || )", 0, BAD, 0, 0}, {"1 +", 0, BAD, 0, 0}, {"(1", 0, BAD, 0, 0},
    {"1)", 0, BAD, 0, 0}, {"1 2", 0, BAD, 0, 0}, {"1 ? 2", 0, BAD, 0, 0}, {"1 = 1", 0, BAD, 0, 0},
    {"1, 2", 0, BAD, 0, 0}, {"* 2", 0, BAD, 0, 0}, {"'a", 0, BAD, 0, 0},
    /* マクロ */
    {"ONE", 0, 1, 0, 0}, {"TWO == 2", 0, 1, 0, 0}, {"SUM * 2", 64, 0, 3, 0}, {"(SUM) * 2", 64, 0, 4, 0},
    {"UNDEFINED", 0, 0, 0, 0}, {"UNDEFINED == 0", 0, 1, 0, 0}, {"SELF", 0, 0, 0, 0}, {"PING", 0, 0, 0, 0},
    {"PING + 1", 64, 0, 1, 0}, {"D0", 64, 0, 7, 0}, {"EMPTY", 0, BAD, 0, 0}, {"EMPTY 1", 0, 1, 0, 0},
    {"HALF 2", 64, 0, 3, 0}, {"HALF", 0, BAD, 0, 0}, {"TEXT", 0, BAD, 0, 0}, {"FN", 0, 0, 0, 0},
    {"FN(1)", 0, BAD, 0, 0}, {"FN (1) + 1", 0, BAD, 0, 0}, {"FN(", 0, BAD, 0, 0},
    {"PRODUCT == TYPE_B", 0, 1, 0, 0}, {"PRODUCT == 1", 0, 0, 0, 0}, {"PRODUCT >= 2 && PRODUCT < 4", 0, 1, 0, 0},
    {"ADDR", 64, 0, 0xFF91CLL, 1}, {"ADDR > 0xFFFFF", 0, 0, 0, 0}, {"(ADDR & 0xF) == 0xC", 0, 1, 0, 0},
    {"SUM * 3 == 4", 0, 1, 0, 0}, {"HALF 2 * 3", 64, 0, 7, 0},
    {"SUM )", 0, BAD, 0, 0}, {"SUM 2", 0, BAD, 0, 0},
    {"OPEN) * 2 == 6", 0, 1, 0, 0}, {"(CLOSE * 2 == 6", 0, 1, 0, 0},
    {"LIMIT0", 0, BAD, 0, 0}, {"1 || LIMIT0", 0, BAD, 0, 0},
    /* defined */
    {"defined(ONE)", 0, 1, 0, 0}, {"defined ONE", 0, 1, 0, 0}, {"defined ( ONE )", 0, 1, 0, 0},
    {"defined(UNDEFINED)", 0, 0, 0, 0}, {"!defined(UNDEFINED)", 0, 1, 0, 0}, {"defined(EMPTY)", 0, 1, 0, 0},
    /* 関数形式マクロの呼び出し */
    {"( CNT(0) > 255 )", 0, 1, 0, 0}, {"CNT(0)", 64, 0, 511, 0}, {"CNT(0) > 600", 0, 0, 0, 0},
    {"DBL(3)", 64, 0, 6, 0}, {"DBL(1 + 2)", 64, 0, 6, 0}, {"DBL (4)", 64, 0, 8, 0}, {"DBL(ONE)", 64, 0, 2, 0},
    {"ADD(1, 2)", 64, 0, 3, 0}, {"ADD((1, 2), 3)", 64, BAD, 0, 0}, {"ADD(DBL(2), DBL(3))", 64, 0, 10, 0},
    {"ADD(ADD(1, 2), 3)", 64, BAD, 0, 0},        /* 引数の中で同じマクロを呼ぶ形は判定不能 */
    {"RAW(1 + 2)", 64, 0, 5, 0},                 /* 1 + 2 * 2 */
    {"NONE()", 64, 0, 7, 0}, {"NONE( )", 64, 0, 7, 0}, {"NONE(1)", 64, BAD, 0, 0},
    {"HEX(1)", 64, 0, 32, 0},                    /* 0x1F の F を仮引数と取り違えない */
    {"DBL()", 64, BAD, 0, 0}, {"DBL(1, 2)", 64, BAD, 0, 0}, {"ADD(1)", 64, BAD, 0, 0},
    {"VAR(1, 2)", 64, BAD, 0, 0}, {"CAT(1, 2)", 64, BAD, 0, 0}, {"STR(1)", 64, BAD, 0, 0},
    {"LOOP(1)", 64, BAD, 0, 0}, {"DIV(4, 0)", 64, BAD, 0, 0}, {"DIV(9, 3)", 64, 0, 3, 0},
    {"CH(',')", 64, 0, 1, 0}, {"CH('a')", 64, 0, 0, 0},
    {"DBL(1", 0, BAD, 0, 0}, {"DBL", 0, 0, 0, 0}, {"DBL + 1", 64, 0, 1, 0},
    {"0 && STR(1)", 0, 0, 0, 0}, {"0 && DBL()", 0, BAD, 0, 0},   /* 空の引数で式が壊れるのは構文の誤り */ {"1 || CAT(1, 2)", 0, 1, 0, 0}, {"defined(DBL) && DBL(1) == 2", 0, 1, 0, 0},
    {"DBL(UNDEFINED)", 64, 0, 0, 0}, {"DBL(DBL)", 64, 0, 0, 0},
    {"defined(FN)", 0, 1, 0, 0}, {"defined(ONE) && !defined(UNDEFINED)", 0, 1, 0, 0},
    {"defined(ONE) + defined(TWO)", 64, 0, 2, 0}, {"defined", 0, BAD, 0, 0}, {"defined(", 0, BAD, 0, 0},
    {"defined(ONE", 0, BAD, 0, 0}, {"defined(1)", 0, BAD, 0, 0}, {"definedONE", 0, 0, 0, 0},
    {"defined(SELF)", 0, 1, 0, 0}, {"defined ONE == 1", 0, 1, 0, 0}
};

int main(void)
{
    sim_pp_env env;
    size_t i, n = sizeof cases / sizeof cases[0];
    int failed = 0;
    env.lookup = lookup; env.ctx = NULL;
    for (i=0;i<35;i++) snprintf(ladder[i],sizeof ladder[i],"LIMIT%zu",i+1);
    strcpy(ladder[35],"1");
    for (i = 0; i < n; i++) {
        int bad = 0;
        if (!cases[i].bits) {
            int r = sim_pp_eval(cases[i].expr, &env);
            bad = r != cases[i].ok;
            if (bad) printf("FAIL  #if %s -> %d (expected %d)\n", cases[i].expr, r, cases[i].ok);
        } else {
            sim_pp_value v = {0, 0};
            int r = sim_pp_eval_width(cases[i].expr, &env, cases[i].bits, &v);
            bad = r != cases[i].ok || (!r && (v.value != cases[i].value || v.is_unsigned != cases[i].uns));
            if (bad) printf("FAIL  %d-bit %s -> status %d value %lld unsigned %d (expected status %d value %lld unsigned %d)\n",
                            cases[i].bits, cases[i].expr, r, (long long)v.value, v.is_unsigned,
                            cases[i].ok, cases[i].value, cases[i].uns);
        }
        failed += bad;
    }
    /* 深い入れ子と長い式は上限で判定不能になり、落ちない */
    {
        static char deep[4096];
        size_t k;
        for (k = 0; k < 1000; k++) deep[k] = '(';
        deep[1000] = '1';
        for (k = 0; k < 1000; k++) deep[1001 + k] = ')';
        deep[2001] = 0;
        if (sim_pp_eval(deep, &env) != -1) { printf("FAIL  deep nesting\n"); failed++; }
        for (k = 0; k < 2000; k++) { deep[2 * k] = '1'; deep[2 * k + 1] = '+'; }
        deep[4000] = '1'; deep[4001] = 0;
        if (sim_pp_eval(deep, &env) != -1) { printf("FAIL  long expression\n"); failed++; }
        if (sim_pp_eval("((((1))))", &env) != 1) { printf("FAIL  shallow nesting\n"); failed++; }
        if (sim_pp_eval("UNDEFINED", NULL) != 0) { printf("FAIL  no environment\n"); failed++; }
        if (sim_pp_eval_width("1", &env, 16, NULL) != -1) { printf("FAIL  bad width\n"); failed++; }
    }
    n += 5;
    {
        static const struct { const char *expr; int ok; uint64_t value; } strict[] = {
            {"SUM*3",0,4}, {"OPEN)*2",0,6}, {"(CLOSE*2",0,6},
            {"SELF",-1,0}, {"PING",-1,0}, {"FN(1)",-1,0}, {"UNDEFINED",-1,0},
            {"0 && SELF",-1,0}, {"HALF",-1,0}, {"ONE TWO",-1,0}, {"OPEN",-1,0}
        };
        size_t k;
        for (k=0;k<sizeof strict/sizeof strict[0];k++) {
            uint64_t v=0;
            int r=sim_pp_eval_constant(strict[k].expr,&env,&v);
            if (r!=strict[k].ok || (!r && v!=strict[k].value)) {
                printf("FAIL strict %s\n",strict[k].expr); failed++;
            }
            n++;
        }
    }
    printf("ppexpr: %zu cases, %d failed, %d lookups\n", n, failed, lookups);
    return failed || !lookups;
}
