/*
 * tasm.c - minimal RL78 assembler used by the tests.
 *
 * The encoding table below was transcribed from R01US0015 Rev.2.40,
 * Table 5-8 "List of Instruction Formats" (and cross-checked against the
 * instruction maps 5-9..5-12).  It is deliberately written in a different
 * form from the simulator's decoder so that the two can be checked against
 * each other.
 *
 * Pattern syntax: operands separated by ", ".  Kinds:
 *   #b #w      immediate byte / word          s S   saddr / saddrp
 *   f F        sfr / sfrp (names or 0FFFxxH)  !a !!A  addr16 / addr20
 *   $r $!R     8/16-bit relative              [HL+o] [DE+o] [SP+o]
 *   W[B] W[C] W[BC]  word-based               % = ES: allowed here
 * Expansion: {r} X..H (r=0..7)  {ra} without A  {rp} AX..HL (p=0..3)
 *   {rpa} without AX  {n} bit 0..7  {c7} 1..7  {c15} 1..15 (c)
 *   {b} bank 0..3     {alu} ADD..XOR (k=0..7)
 * Byte template: hex, or expression "61 08+k*10+r", or field names
 *   d dl dh s f al ah as j jl jh o wl wh
 */
#include "tasm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const table_src[] = {
    "NOP = 00",
    /* ---- 8-bit data transfer ---- */
    "MOV {r}, #b = 50+r d",
    "MOV PSW, #b = CE FA d",
    "MOV CS, #b = CE FC d",
    "MOV ES, #b = 41 d",
    "MOV %!a, #b = CF al ah d",
    "MOV s, #b = CD s d",
    "MOV f, #b = CE f d",
    "MOV %[DE+o], #b = CA o d",
    "MOV %[HL+o], #b = CC o d",
    "MOV [SP+o], #b = C8 o d",
    "MOV %W[B], #b = 19 wl wh d",
    "MOV %W[C], #b = 38 wl wh d",
    "MOV %W[BC], #b = 39 wl wh d",
    "MOV A, {ra} = 60+r",
    "MOV {ra}, A = 70+r",
    "MOV A, PSW = 8E FA",
    "MOV PSW, A = 9E FA",
    "MOV A, CS = 8E FC",
    "MOV CS, A = 9E FC",
    "MOV A, ES = 8E FD",
    "MOV ES, A = 9E FD",
    "MOV A, %!a = 8F al ah",
    "MOV %!a, A = 9F al ah",
    "MOV A, s = 8D s",
    "MOV s, A = 9D s",
    "MOV A, f = 8E f",
    "MOV f, A = 9E f",
    "MOV A, %[DE] = 89",
    "MOV %[DE], A = 99",
    "MOV A, %[HL] = 8B",
    "MOV %[HL], A = 9B",
    "MOV A, %[DE+o] = 8A o",
    "MOV %[DE+o], A = 9A o",
    "MOV A, %[HL+o] = 8C o",
    "MOV %[HL+o], A = 9C o",
    "MOV A, [SP+o] = 88 o",
    "MOV [SP+o], A = 98 o",
    "MOV A, %W[B] = 09 wl wh",
    "MOV %W[B], A = 18 wl wh",
    "MOV A, %W[C] = 29 wl wh",
    "MOV %W[C], A = 28 wl wh",
    "MOV A, %W[BC] = 49 wl wh",
    "MOV %W[BC], A = 48 wl wh",
    "MOV A, %[HL+B] = 61 C9",
    "MOV %[HL+B], A = 61 D9",
    "MOV A, %[HL+C] = 61 E9",
    "MOV %[HL+C], A = 61 F9",
    "MOV X, %!a = D9 al ah",
    "MOV X, s = D8 s",
    "MOV B, %!a = E9 al ah",
    "MOV B, s = E8 s",
    "MOV C, %!a = F9 al ah",
    "MOV C, s = F8 s",
    "MOV ES, s = 61 B8 s",
    "XCH A, X = 08",
    "XCH A, C = 61 8A",
    "XCH A, B = 61 8B",
    "XCH A, E = 61 8C",
    "XCH A, D = 61 8D",
    "XCH A, L = 61 8E",
    "XCH A, H = 61 8F",
    "XCH A, %!a = 61 AA al ah",
    "XCH A, s = 61 A8 s",
    "XCH A, f = 61 AB f",
    "XCH A, %[DE] = 61 AE",
    "XCH A, %[HL] = 61 AC",
    "XCH A, %[DE+o] = 61 AF o",
    "XCH A, %[HL+o] = 61 AD o",
    "XCH A, %[HL+B] = 61 B9",
    "XCH A, %[HL+C] = 61 A9",
    "ONEB A = E1", "ONEB X = E0", "ONEB B = E3", "ONEB C = E2",
    "ONEB %!a = E5 al ah", "ONEB s = E4 s",
    "CLRB A = F1", "CLRB X = F0", "CLRB B = F3", "CLRB C = F2",
    "CLRB %!a = F5 al ah", "CLRB s = F4 s",
    "MOVS %[HL+o], X = 61 CE o",
    /* ---- 16-bit data transfer ---- */
    "MOVW {rp}, #w = 30+p*2 dl dh",
    "MOVW S, #w = C9 s dl dh",
    "MOVW F, #w = CB f dl dh",
    "MOVW AX, {rpa} = 11+p*2",
    "MOVW {rpa}, AX = 10+p*2",
    "MOVW AX, %!a = AF al ah",
    "MOVW %!a, AX = BF al ah",
    "MOVW AX, S = AD s",
    "MOVW S, AX = BD s",
    "MOVW AX, F = AE f",
    "MOVW F, AX = BE f",
    "MOVW AX, %[DE] = A9",
    "MOVW %[DE], AX = B9",
    "MOVW AX, %[HL] = AB",
    "MOVW %[HL], AX = BB",
    "MOVW AX, %[DE+o] = AA o",
    "MOVW %[DE+o], AX = BA o",
    "MOVW AX, %[HL+o] = AC o",
    "MOVW %[HL+o], AX = BC o",
    "MOVW AX, [SP+o] = A8 o",
    "MOVW [SP+o], AX = B8 o",
    "MOVW AX, %W[B] = 59 wl wh",
    "MOVW %W[B], AX = 58 wl wh",
    "MOVW AX, %W[C] = 69 wl wh",
    "MOVW %W[C], AX = 68 wl wh",
    "MOVW AX, %W[BC] = 79 wl wh",
    "MOVW %W[BC], AX = 78 wl wh",
    "MOVW BC, %!a = DB al ah",
    "MOVW DE, %!a = EB al ah",
    "MOVW HL, %!a = FB al ah",
    "MOVW BC, S = DA s",
    "MOVW DE, S = EA s",
    "MOVW HL, S = FA s",
    "XCHW AX, {rpa} = 31+p*2",
    "ONEW AX = E6", "ONEW BC = E7", "CLRW AX = F6", "CLRW BC = F7",
    /* ---- 8-bit operation ---- */
    "{alu} A, #b = 0C+k*10 d",
    "{alu} s, #b = 0A+k*10 s d",
    "{alu} A, {ra} = 61 08+k*10+r",
    "{alu} {r}, A = 61 k*10+r",
    "{alu} A, %!a = 0F+k*10 al ah",
    "{alu} A, s = 0B+k*10 s",
    "{alu} A, %[HL] = 0D+k*10",
    "{alu} A, %[HL+o] = 0E+k*10 o",
    "{alu} A, %[HL+B] = 61 80+k*10",
    "{alu} A, %[HL+C] = 61 82+k*10",
    "CMP %!a, #b = 40 al ah d",
    "CMP0 A = D1", "CMP0 X = D0", "CMP0 B = D3", "CMP0 C = D2",
    "CMP0 %!a = D5 al ah", "CMP0 s = D4 s",
    "CMPS X, %[HL+o] = 61 DE o",
    /* ---- 16-bit operation ---- */
    "ADDW AX, #w = 04 dl dh", "ADDW AX, AX = 01", "ADDW AX, BC = 03", "ADDW AX, DE = 05",
    "ADDW AX, HL = 07", "ADDW AX, %!a = 02 al ah", "ADDW AX, S = 06 s",
    "ADDW AX, %[HL+o] = 61 09 o",
    "SUBW AX, #w = 24 dl dh", "SUBW AX, BC = 23", "SUBW AX, DE = 25", "SUBW AX, HL = 27",
    "SUBW AX, %!a = 22 al ah", "SUBW AX, S = 26 s", "SUBW AX, %[HL+o] = 61 29 o",
    "CMPW AX, #w = 44 dl dh", "CMPW AX, BC = 43", "CMPW AX, DE = 45", "CMPW AX, HL = 47",
    "CMPW AX, %!a = 42 al ah", "CMPW AX, S = 46 s", "CMPW AX, %[HL+o] = 61 49 o",
    /* ---- multiply / divide ---- */
    "MULU X = D6",
    "MULHU = CE FB 01", "MULH = CE FB 02", "DIVHU = CE FB 03", "DIVWU = CE FB 0B",
    "MACHU = CE FB 05", "MACH = CE FB 06",
    /* ---- increment / decrement ---- */
    "INC {r} = 80+r", "INC %!a = A0 al ah", "INC s = A4 s", "INC %[HL+o] = 61 59 o",
    "DEC {r} = 90+r", "DEC %!a = B0 al ah", "DEC s = B4 s", "DEC %[HL+o] = 61 69 o",
    "INCW {rp} = A1+p*2", "INCW %!a = A2 al ah", "INCW S = A6 s", "INCW %[HL+o] = 61 79 o",
    "DECW {rp} = B1+p*2", "DECW %!a = B2 al ah", "DECW S = B6 s", "DECW %[HL+o] = 61 89 o",
    /* ---- shift / rotate ---- */
    "SHR A, {c7} = 31 c*10+0A",
    "SHRW AX, {c15} = 31 c*10+0E",
    "SHL A, {c7} = 31 c*10+09",
    "SHL B, {c7} = 31 c*10+08",
    "SHL C, {c7} = 31 c*10+07",
    "SHLW AX, {c15} = 31 c*10+0D",
    "SHLW BC, {c15} = 31 c*10+0C",
    "SAR A, {c7} = 31 c*10+0B",
    "SARW AX, {c15} = 31 c*10+0F",
    "ROR A, 1 = 61 DB", "ROL A, 1 = 61 EB", "RORC A, 1 = 61 FB", "ROLC A, 1 = 61 DC",
    "ROLWC AX, 1 = 61 EE", "ROLWC BC, 1 = 61 FE",
    /* ---- bit manipulation ---- */
    "EI = 71 7A FA", "DI = 71 7B FA",
    "MOV1 CY, A.{n} = 71 8C+n*10", "MOV1 A.{n}, CY = 71 89+n*10",
    "MOV1 CY, s.{n} = 71 04+n*10 s", "MOV1 s.{n}, CY = 71 01+n*10 s",
    "MOV1 CY, f.{n} = 71 0C+n*10 f", "MOV1 f.{n}, CY = 71 09+n*10 f",
    "MOV1 CY, %[HL].{n} = 71 84+n*10", "MOV1 %[HL].{n}, CY = 71 81+n*10",
    "AND1 CY, A.{n} = 71 8D+n*10", "AND1 CY, s.{n} = 71 05+n*10 s",
    "AND1 CY, f.{n} = 71 0D+n*10 f", "AND1 CY, %[HL].{n} = 71 85+n*10",
    "OR1 CY, A.{n} = 71 8E+n*10", "OR1 CY, s.{n} = 71 06+n*10 s",
    "OR1 CY, f.{n} = 71 0E+n*10 f", "OR1 CY, %[HL].{n} = 71 86+n*10",
    "XOR1 CY, A.{n} = 71 8F+n*10", "XOR1 CY, s.{n} = 71 07+n*10 s",
    "XOR1 CY, f.{n} = 71 0F+n*10 f", "XOR1 CY, %[HL].{n} = 71 87+n*10",
    "SET1 A.{n} = 71 8A+n*10", "SET1 %!a.{n} = 71 00+n*10 al ah", "SET1 s.{n} = 71 02+n*10 s",
    "SET1 f.{n} = 71 0A+n*10 f", "SET1 %[HL].{n} = 71 82+n*10",
    "CLR1 A.{n} = 71 8B+n*10", "CLR1 %!a.{n} = 71 08+n*10 al ah", "CLR1 s.{n} = 71 03+n*10 s",
    "CLR1 f.{n} = 71 0B+n*10 f", "CLR1 %[HL].{n} = 71 83+n*10",
    "SET1 CY = 71 80", "CLR1 CY = 71 88", "NOT1 CY = 71 C0",
    /* ---- call / return ---- */
    "CALL AX = 61 CA", "CALL BC = 61 DA", "CALL DE = 61 EA", "CALL HL = 61 FA",
    "CALL $!R = FE jl jh", "CALL !a = FD al ah", "CALL !!A = FC al ah as",
    "BRK = 61 CC", "RET = D7", "RETI = 61 FC", "RETB = 61 EC",
    /* ---- stack ---- */
    "PUSH PSW = 61 DD", "PUSH {rp} = C1+p*2", "POP PSW = 61 CD", "POP {rp} = C0+p*2",
    "MOVW SP, #w = CB F8 dl dh", "MOVW SP, AX = BE F8", "MOVW AX, SP = AE F8",
    "MOVW BC, SP = DB F8 FF", "MOVW DE, SP = EB F8 FF", "MOVW HL, SP = FB F8 FF",
    "ADDW SP, #b = 10 d", "SUBW SP, #b = 20 d",
    /* ---- branches ---- */
    "BR AX = 61 CB", "BR $r = EF j", "BR $!R = EE jl jh", "BR !a = ED al ah",
    "BR !!A = EC al ah as",
    "BC $r = DC j", "BNC $r = DE j", "BZ $r = DD j", "BNZ $r = DF j",
    "BH $r = 61 C3 j", "BNH $r = 61 D3 j",
    "BT s.{n}, $r = 31 02+n*10 s j", "BT f.{n}, $r = 31 82+n*10 f j",
    "BT A.{n}, $r = 31 03+n*10 j", "BT %[HL].{n}, $r = 31 83+n*10 j",
    "BF s.{n}, $r = 31 04+n*10 s j", "BF f.{n}, $r = 31 84+n*10 f j",
    "BF A.{n}, $r = 31 05+n*10 j", "BF %[HL].{n}, $r = 31 85+n*10 j",
    "BTCLR s.{n}, $r = 31 00+n*10 s j", "BTCLR f.{n}, $r = 31 80+n*10 f j",
    "BTCLR A.{n}, $r = 31 01+n*10 j", "BTCLR %[HL].{n}, $r = 31 81+n*10 j",
    /* ---- skip / CPU control ---- */
    "SKC = 61 C8", "SKNC = 61 D8", "SKZ = 61 E8", "SKNZ = 61 F8", "SKH = 61 E3", "SKNH = 61 F3",
    "SEL RB{b} = 61 CF+b*10",
    "HALT = 61 ED", "STOP = 61 FD",
};

/* ---- table expansion -------------------------------------------------------- */

typedef struct {
    char pat[48];
    char bytes[48];
} entry;

static entry *g_tab;
static int g_ntab;

static const char *const R8[8] = {"X", "A", "C", "B", "E", "D", "L", "H"};
static const char *const RP[4] = {"AX", "BC", "DE", "HL"};
static const char *const ALU[8] = {"ADD", "ADDC", "SUB", "SUBC", "CMP", "AND", "OR", "XOR"};

static void add_entry(const char *pat, const char *bytes)
{
    g_tab = (entry *)realloc(g_tab, sizeof(entry) * (size_t)(g_ntab + 1));
    snprintf(g_tab[g_ntab].pat, sizeof g_tab[g_ntab].pat, "%.*s", (int)sizeof g_tab[g_ntab].pat - 1, pat);
    snprintf(g_tab[g_ntab].bytes, sizeof g_tab[g_ntab].bytes, "%.*s", (int)sizeof g_tab[g_ntab].bytes - 1, bytes);
    g_ntab++;
}

/* simple tokenizer (C11 has no strtok_r) */
static char *tok_next(char **cur, char sep)
{
    char *s = *cur, *e;
    while (*s == sep) s++;
    if (!*s) return NULL;
    e = strchr(s, sep);
    if (e) { *e = 0; *cur = e + 1; }
    else *cur = s + strlen(s);
    return s;
}

static int is_field(const char *t)
{
    static const char *const f[] = {"d", "dl", "dh", "s", "f", "al", "ah", "as", "j",
                                    "jl", "jh", "o", "wl", "wh", NULL};
    int i;
    for (i = 0; f[i]; i++)
        if (!strcmp(t, f[i])) return 1;
    return 0;
}

/* evaluate "08+k*10+r": hex constants, variables (lowercase letters) */
static int eval_byte(const char *e, const int *vars)
{
    int sum = 0;
    char buf[32];
    char *cur = buf, *tok;
    snprintf(buf, sizeof buf, "%s", e);
    while ((tok = tok_next(&cur, '+')) != NULL) {
        char *star = strchr(tok, '*');
        int v;
        if (star) *star = 0;
        if (islower((unsigned char)tok[0])) v = vars[(unsigned char)tok[0]];
        else v = (int)strtol(tok, NULL, 16);
        if (star) v *= (int)strtol(star + 1, NULL, 16);
        sum += v;
    }
    return sum & 0xFF;
}

static void expand_rec(const char *line, int *vars)
{
    const char *lb = strchr(line, '{');
    if (!lb) {
        const char *eq = strstr(line, " = ");
        char pat[64], tmp[96], out[96] = "";
        char *cur = tmp, *tok;
        snprintf(pat, sizeof pat, "%.*s", (int)(eq - line), line);
        snprintf(tmp, sizeof tmp, "%s", eq + 3);
        while ((tok = tok_next(&cur, ' ')) != NULL) {
            char b[8];
            if (is_field(tok)) snprintf(b, sizeof b, "%s", tok);
            else snprintf(b, sizeof b, "%02X", eval_byte(tok, vars));
            if (out[0]) strcat(out, " ");
            strcat(out, b);
        }
        add_entry(pat, out);
        return;
    }
    {
        const char *rb = strchr(lb, '}');
        char var[16];
        int i, lo = 0, hi = -1;
        char vname = 0;
        const char *const *names = NULL;
        int skip = -1;
        snprintf(var, sizeof var, "%.*s", (int)(rb - lb - 1), lb + 1);
        if (!strcmp(var, "r")) { names = R8; lo = 0; hi = 7; vname = 'r'; }
        else if (!strcmp(var, "ra")) { names = R8; lo = 0; hi = 7; skip = 1; vname = 'r'; }
        else if (!strcmp(var, "rp")) { names = RP; lo = 0; hi = 3; vname = 'p'; }
        else if (!strcmp(var, "rpa")) { names = RP; lo = 1; hi = 3; vname = 'p'; }
        else if (!strcmp(var, "alu")) { names = ALU; lo = 0; hi = 7; vname = 'k'; }
        else if (!strcmp(var, "n")) { lo = 0; hi = 7; vname = 'n'; }
        else if (!strcmp(var, "c7")) { lo = 1; hi = 7; vname = 'c'; }
        else if (!strcmp(var, "c15")) { lo = 1; hi = 15; vname = 'c'; }
        else if (!strcmp(var, "b")) { lo = 0; hi = 3; vname = 'b'; }
        for (i = lo; i <= hi; i++) {
            char line2[160], num[16];
            if (i == skip) continue;
            snprintf(num, sizeof num, "%d", i);
            snprintf(line2, sizeof line2, "%.*s%s%s", (int)(lb - line), line,
                     names ? names[i] : num, rb + 1);
            vars[(unsigned char)vname] = i;
            expand_rec(line2, vars);
        }
    }
}

static void expand_line(const char *line)
{
    int vars[256] = {0};
    expand_rec(line, vars);
}

static void build_table(void)
{
    size_t i;
    if (g_tab) return;
    for (i = 0; i < sizeof table_src / sizeof table_src[0]; i++) expand_line(table_src[i]);
}

int tasm_table_size(void)
{
    build_table();
    return g_ntab;
}

void tasm_table_entry(int i, const char **pattern, const char **bytes)
{
    build_table();
    *pattern = g_tab[i].pat;
    *bytes = g_tab[i].bytes;
}

/* ---- labels ------------------------------------------------------------------ */

#define MAXLAB 1024
static char g_labname[MAXLAB][32];
static long g_labval[MAXLAB];
static int g_nlab;
static int g_pass2;

static void upcase(char *s);
long tasm_label(const char *name)
{
    int i;
    char u[32];
    snprintf(u, sizeof u, "%s", name);
    upcase(u);
    for (i = 0; i < g_nlab; i++)
        if (!strcmp(g_labname[i], u)) return g_labval[i];
    return -1;
}

static int is_regname(const char *s)
{
    static const char *const names[] = {"X", "A", "C", "B", "E", "D", "L", "H", "AX", "BC",
                                        "DE", "HL", "SP", "PSW", "CS", "ES", "CY", NULL};
    int i;
    for (i = 0; names[i]; i++)
        if (!strcmp(s, names[i])) return 1;
    return 0;
}

/* parse a number or label; returns 0 on success */
static int parse_num(const char *s, long *v)
{
    size_t l = strlen(s);
    char *end;
    if (l == 0) return -1;
    if (isdigit((unsigned char)s[0])) {
        if (s[l - 1] == 'H' || s[l - 1] == 'h') {
            *v = strtol(s, &end, 16);
            return (end == s + l - 1) ? 0 : -1;
        }
        if (l > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            *v = strtol(s + 2, &end, 16);
            return *end ? -1 : 0;
        }
        *v = strtol(s, &end, 10);
        return *end ? -1 : 0;
    }
    if ((isalpha((unsigned char)s[0]) || s[0] == '_') && !is_regname(s)) {
        long x = tasm_label(s);
        if (x < 0) {
            if (g_pass2) return -1;
            x = 0;
        }
        *v = x;
        return 0;
    }
    return -1;
}

static const char *sfr_names[] = {"SPL", "SPH", "PSW", NULL, "CS", "ES", "PMC", "MEM"};

static int parse_sfr(const char *s, long *v)
{
    int i;
    for (i = 0; i < 8; i++)
        if (sfr_names[i] && !strcmp(s, sfr_names[i])) { *v = 0xF8 + i; return 0; }
    if (parse_num(s, v) == 0 && *v >= 0xFFF00 && *v <= 0xFFFFF) { *v -= 0xFFF00; return 0; }
    return -1;
}

static int parse_saddr(const char *s, long *v)
{
    long x;
    if (parse_num(s, &x) != 0) return -1;
    if (x >= 0xFFE20 && x <= 0xFFEFF) x &= 0xFFFF;
    if (x < 0xFE20 || x > 0xFF1F) return -1;
    *v = (x - 0xFE00) & 0xFF;
    return 0;
}

typedef struct {
    long d, s, f, a, A, j, o, w;
    int es, has_j, j16;
} fields;

static void trim(char *s)
{
    size_t l;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
}

static void upcase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* match one pattern operand against actual operand */
static int match_opnd(const char *P, const char *T, fields *fl)
{
    char p[48], t[48];
    int esok = 0;
    long v;
    snprintf(p, sizeof p, "%s", P);
    snprintf(t, sizeof t, "%s", T);
    if (p[0] == '%') { esok = 1; memmove(p, p + 1, strlen(p)); }
    if (!strncmp(t, "ES:", 3)) {
        if (!esok) return 0;
        fl->es = 1;
        memmove(t, t + 3, strlen(t + 3) + 1);
    }
    /* bit suffix */
    {
        char *pd = strrchr(p, '.');
        if (pd && pd[1] >= '0' && pd[1] <= '7' && pd[2] == 0) {
            char *td = strrchr(t, '.');
            if (!td || strcmp(td, pd)) return 0;
            *pd = 0;
            *td = 0;
        }
    }
    if (!strcmp(p, "#b")) {
        if (t[0] != '#' || parse_num(t + 1, &v) || v < 0 || v > 0xFF) return 0;
        fl->d = v; return 1;
    }
    if (!strcmp(p, "#w")) {
        if (t[0] != '#' || parse_num(t + 1, &v) || v < 0 || v > 0xFFFF) return 0;
        fl->d = v; return 1;
    }
    if (!strcmp(p, "s") || !strcmp(p, "S")) {
        if (parse_saddr(t, &v)) return 0;
        if (p[0] == 'S' && (v & 1)) return 0;
        fl->s = v; return 1;
    }
    if (!strcmp(p, "f") || !strcmp(p, "F")) {
        if (p[0] == 'F' && !strcmp(t, "SP")) { fl->f = 0xF8; return 1; }
        if (parse_sfr(t, &v)) return 0;
        if (p[0] == 'F' && (v & 1)) return 0;
        fl->f = v; return 1;
    }
    if (!strcmp(p, "!a")) {
        if (t[0] != '!' || t[1] == '!' || parse_num(t + 1, &v) || v < 0 || v > 0xFFFF) return 0;
        fl->a = v; return 1;
    }
    if (!strcmp(p, "!!A")) {
        if (t[0] != '!' || t[1] != '!' || parse_num(t + 2, &v) || v < 0 || v > 0xFFFFF) return 0;
        fl->A = v; return 1;
    }
    if (!strcmp(p, "$r")) {
        if (t[0] != '$' || t[1] == '!' || parse_num(t + 1, &v)) return 0;
        fl->j = v; fl->has_j = 1; fl->j16 = 0; return 1;
    }
    if (!strcmp(p, "$!R")) {
        if (t[0] != '$' || t[1] != '!' || parse_num(t + 2, &v)) return 0;
        fl->j = v; fl->has_j = 1; fl->j16 = 1; return 1;
    }
    if (!strcmp(p, "[HL+o]") || !strcmp(p, "[DE+o]") || !strcmp(p, "[SP+o]")) {
        size_t l = strlen(t);
        char num[32];
        if (strncmp(t, p, 4) || t[l - 1] != ']' || l < 6) return 0;
        snprintf(num, sizeof num, "%.*s", (int)(l - 5), t + 4);
        if (!strcmp(num, "B") || !strcmp(num, "C")) return 0;
        if (parse_num(num, &v) || v < 0 || v > 0xFF) return 0;
        fl->o = v; return 1;
    }
    if (!strcmp(p, "W[B]") || !strcmp(p, "W[C]") || !strcmp(p, "W[BC]")) {
        const char *br = strchr(t, '[');
        char num[32];
        if (!br || strcmp(br, p + 1) || br == t) return 0;
        snprintf(num, sizeof num, "%.*s", (int)(br - t), t);
        if (parse_num(num, &v) || v < 0 || v > 0xFFFF) return 0;
        fl->w = v; return 1;
    }
    return strcmp(p, t) == 0;
}

static int split_ops(char *s, char **ops, int max)
{
    int n = 0;
    char *p = s;
    if (!*s) return 0;
    while (n < max) {
        char *c = strchr(p, ',');
        ops[n++] = p;
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    {
        int i;
        for (i = 0; i < n; i++) trim(ops[i]);
    }
    return n;
}

int tasm_one(const char *text, uint32_t pc, uint8_t *out, char *err, size_t errn)
{
    char line[128], mn[16];
    char *ops[4];
    int nops, i;
    char *sp;
    build_table();
    snprintf(line, sizeof line, "%s", text);
    trim(line);
    upcase(line);
    sp = strchr(line, ' ');
    if (sp) { *sp = 0; snprintf(mn, sizeof mn, "%.*s", (int)sizeof mn - 1, line); nops = split_ops(sp + 1, ops, 4); }
    else { snprintf(mn, sizeof mn, "%.*s", (int)sizeof mn - 1, line); nops = 0; }

    if (!strcmp(mn, "CALLT")) {
        long v;
        char num[32];
        size_t l;
        if (nops != 1 || ops[0][0] != '[') goto bad;
        l = strlen(ops[0]);
        snprintf(num, sizeof num, "%.*s", (int)(l - 2), ops[0] + 1);
        if (parse_num(num, &v) || v < 0x80 || v > 0xBE || (v & 1)) goto bad;
        out[0] = 0x61;
        out[1] = (uint8_t)(((8 + ((v & 0xF) >> 1)) << 4) | (4 + ((v - 0x80) >> 4)));
        return 2;
    }

    for (i = 0; i < g_ntab; i++) {
        char pat[64], pmn[16];
        char *pops[4];
        int pn, k, ok = 1;
        fields fl;
        char *psp;
        memset(&fl, 0, sizeof fl);
        snprintf(pat, sizeof pat, "%s", g_tab[i].pat);
        psp = strchr(pat, ' ');
        if (psp) { *psp = 0; pn = split_ops(psp + 1, pops, 4); }
        else pn = 0;
        snprintf(pmn, sizeof pmn, "%.*s", (int)sizeof pmn - 1, pat);
        if (strcmp(pmn, mn) || pn != nops) continue;
        for (k = 0; k < pn && ok; k++) ok = match_opnd(pops[k], ops[k], &fl);
        if (!ok) continue;
        {
            /* emit */
            char bt[64];
            char *cur = bt, *tok;
            int len = 0, jpos = -1, j16 = 0;
            if (fl.es) out[len++] = 0x11;
            snprintf(bt, sizeof bt, "%s", g_tab[i].bytes);
            while ((tok = tok_next(&cur, ' ')) != NULL) {
                if (!strcmp(tok, "d") || !strcmp(tok, "dl")) out[len++] = (uint8_t)fl.d;
                else if (!strcmp(tok, "dh")) out[len++] = (uint8_t)(fl.d >> 8);
                else if (!strcmp(tok, "s")) out[len++] = (uint8_t)fl.s;
                else if (!strcmp(tok, "f")) out[len++] = (uint8_t)fl.f;
                else if (!strcmp(tok, "al")) out[len++] = (uint8_t)(fl.a ? fl.a : fl.A);
                else if (!strcmp(tok, "ah")) out[len++] = (uint8_t)((fl.a ? fl.a : fl.A) >> 8);
                else if (!strcmp(tok, "as")) out[len++] = (uint8_t)(fl.A >> 16);
                else if (!strcmp(tok, "o")) out[len++] = (uint8_t)fl.o;
                else if (!strcmp(tok, "wl")) out[len++] = (uint8_t)fl.w;
                else if (!strcmp(tok, "wh")) out[len++] = (uint8_t)(fl.w >> 8);
                else if (!strcmp(tok, "j")) { jpos = len; out[len++] = 0; }
                else if (!strcmp(tok, "jl")) { jpos = len; j16 = 1; out[len++] = 0; out[len++] = 0; }
                else if (!strcmp(tok, "jh")) { /* consumed with jl */ }
                else out[len++] = (uint8_t)strtol(tok, NULL, 16);
            }
            if (jpos >= 0) {
                long rel = fl.j - (long)((pc + (uint32_t)len) & 0xFFFFF);
                if (rel > 0x7FFFF) rel -= 0x100000;
                if (rel < -0x80000) rel += 0x100000;
                if (!j16 && (rel < -128 || rel > 127)) {
                    if (g_pass2) { snprintf(err, errn, "branch out of range: %s", text); return -1; }
                    rel = 0;
                }
                out[jpos] = (uint8_t)rel;
                if (j16) out[jpos + 1] = (uint8_t)(rel >> 8);
            }
            return len;
        }
    }
bad:
    snprintf(err, errn, "cannot assemble: %s", text);
    return -1;
}

int tasm_program(const char *src, uint8_t *mem, uint32_t *end, char *err, size_t errn)
{
    int pass;
    uint32_t pc = 0;
    g_nlab = 0;
    for (pass = 0; pass < 2; pass++) {
        const char *p = src;
        int lineno = 0;
        g_pass2 = pass;
        pc = 0;
        while (*p) {
            char line[160];
            const char *nl = strchr(p, '\n');
            size_t l = nl ? (size_t)(nl - p) : strlen(p);
            char *c, *colon;
            lineno++;
            snprintf(line, sizeof line, "%.*s", (int)l, p);
            p += l + (nl ? 1 : 0);
            c = strchr(line, ';');
            if (c) *c = 0;
            trim(line);
            colon = strchr(line, ':');
            if (colon && !strchr(line, ' ') && colon[1] == 0) {
                *colon = 0;
                if (pass == 0) {
                    if (g_nlab >= MAXLAB) { snprintf(err, errn, "too many labels"); return -1; }
                    snprintf(g_labname[g_nlab], sizeof g_labname[0], "%.*s", (int)sizeof g_labname[0] - 1, line);
                    upcase(g_labname[g_nlab]);
                    g_labval[g_nlab++] = (long)pc;
                }
                continue;
            }
            if (colon && colon > line && !strchr(line, ' ') ) { /* label: insn without space? */ }
            {
                /* "label: insn" form */
                char *sp = strchr(line, ' ');
                if (colon && sp && colon < sp && colon[1] == ' ') {
                    *colon = 0;
                    if (pass == 0) {
                        snprintf(g_labname[g_nlab], sizeof g_labname[0], "%.*s", (int)sizeof g_labname[0] - 1, line);
                        upcase(g_labname[g_nlab]);
                        g_labval[g_nlab++] = (long)pc;
                    }
                    memmove(line, colon + 1, strlen(colon + 1) + 1);
                    trim(line);
                }
            }
            if (!line[0]) continue;
            if (!strncmp(line, ".org", 4)) {
                long v;
                char t[32];
                snprintf(t, sizeof t, "%.*s", (int)sizeof t - 1, line + 4);
                trim(t);
                upcase(t);
                if (parse_num(t, &v)) { snprintf(err, errn, "line %d: bad .org", lineno); return -1; }
                pc = (uint32_t)v;
                continue;
            }
            if (!strncmp(line, ".db", 3) || !strncmp(line, ".dw", 3)) {
                int w = line[2] == 'w';
                char t[160];
                char *ops[64];
                int n, i;
                snprintf(t, sizeof t, "%s", line + 3);
                upcase(t);
                n = split_ops(t, ops, 64);
                for (i = 0; i < n; i++) {
                    long v;
                    if (parse_num(ops[i], &v)) { snprintf(err, errn, "line %d: bad data", lineno); return -1; }
                    if (mem) mem[pc & 0xFFFFF] = (uint8_t)v;
                    pc++;
                    if (w) { if (mem) mem[pc & 0xFFFFF] = (uint8_t)(v >> 8); pc++; }
                }
                continue;
            }
            {
                uint8_t b[8];
                int n = tasm_one(line, pc, b, err, errn), i;
                if (n < 0) {
                    char e2[200];
                    snprintf(e2, sizeof e2, "line %d: %s", lineno, err);
                    snprintf(err, errn, "%s", e2);
                    return -1;
                }
                if (pass == 1 && mem)
                    for (i = 0; i < n; i++) mem[(pc + (uint32_t)i) & 0xFFFFF] = b[i];
                pc += (uint32_t)n;
            }
        }
    }
    if (end) *end = pc;
    return 0;
}
