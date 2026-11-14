/*
 * t_decode.c - decode-table tests.
 *
 *  1. Sweep every opcode of the four maps (and every ES:-prefixed variant) on
 *     each core: the set of undefined opcodes must equal the grey cells of
 *     the instruction maps (Tables 5-9..5-12, transcribed below), the
 *     disassembler text must re-assemble (independent tasm table) to the
 *     same bytes, and executing the instruction must advance PC by the
 *     decoded length.
 *  2. Every entry of the tasm encoding table, instantiated with sample
 *     operands, must disassemble back to the same text.
 *  3. The set of ES:-capable opcodes must equal the set marked '%' in the
 *     tasm table (i.e. the ES: rows of Table 5-8).
 */
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

/* grey (undefined) cells transcribed from the instruction maps */
static int map1_undef(unsigned o) { return o == 0x21 || o == 0xFF; }
static int map2_undef(unsigned o, rl78_core core)
{
    unsigned lo = o & 0xF, hi = o >> 4;
    if (o == 0x19 || o == 0x39) return 1;
    if (hi >= 8 && lo == 1) return 1;
    if (o == 0x83 || o == 0x93 || o == 0xA3 || o == 0xB3) return 1;
    if (o == 0x88 || o == 0x98 || o == 0x99) return 1;
    if ((hi == 9 || hi == 0xB) && lo >= 0xA) return 1;
    if (core == RL78_CORE_S1 && (o == 0xCF || o == 0xDF || o == 0xEF || o == 0xFF)) return 1; /* SEL */
    return 0;
}
static int map3_undef(unsigned o)
{
    unsigned lo = o & 0xF, hi = o >> 4;
    if (hi >= 9 && lo == 0 && hi != 0xC) return 1;
    if (hi >= 9 && lo == 8) return 1;
    return 0;
}
static int map4_undef(unsigned o)
{
    unsigned lo = o & 0xF, hi = o >> 4;
    if (lo == 6) return 1;
    if (hi == 0 && lo >= 7) return 1;
    if (hi >= 8 && lo >= 7 && lo <= 0xB) return 1;
    return 0;
}

static int is_flow(const char *t)
{
    static const char *const m[] = {"BR ", "BC ", "BNC ", "BZ ", "BNZ ", "BH ", "BNH ", "BT ",
                                    "BF ", "BTCLR ", "CALL ", "CALLT ", "RET", "BRK", "SK", NULL};
    int i;
    for (i = 0; m[i]; i++)
        if (!strncmp(t, m[i], strlen(m[i]))) return 1;
    return 0;
}

static unsigned long n_defined, n_undefined, n_es_defined;

static void check_one(rl78_cpu *cpu, rl78_core core, const uint8_t *b, int expect_undef)
{
    char text[96], err[128];
    uint8_t re[8];
    int len = rl78_disasm_buf(core, b, 6, 0x200, text, sizeof text);
    rl78_stop_reason r;
    if (expect_undef >= 0) {
        CHECKF((len == 0) == (expect_undef != 0), "core S%d bytes %02X %02X %02X: undefined=%d expected %d (%s)",
               core, b[0], b[1], b[2], len == 0, expect_undef, text);
    }
    /* execute */
    rl78_load(cpu, 0x200, b, 6);
    rl78_set_reg(cpu, RL78_REG_PMC, 0);
    rl78_set_reg(cpu, RL78_REG_PSW, 0x06);
    rl78_set_reg(cpu, RL78_REG_SP, 0xFE00);
    rl78_set_reg(cpu, RL78_REG_ES, 0x0F);
    rl78_set_reg(cpu, RL78_REG_CS, 0);
    rl78_set_reg(cpu, RL78_REG_HL, 0xFE80);
    rl78_set_reg(cpu, RL78_REG_DE, 0xFE90);
    rl78_set_reg(cpu, RL78_REG_BC, 0x0004);
    rl78_set_reg(cpu, RL78_REG_AX, 0x1234);
    rl78_set_reg(cpu, RL78_REG_PC, 0x200);
    r = rl78_step(cpu);
    if (len == 0) {
        n_undefined++;
        CHECKF(r == RL78_STOP_UNDEFINED && rl78_get_reg(cpu, RL78_REG_PC) == 0x200,
               "core S%d bytes %02X %02X %02X: disasm undefined but exec stop=%d", core, b[0], b[1],
               b[2], (int)r);
        return;
    }
    n_defined++;
    if (b[0] == 0x11) n_es_defined++;
    CHECKF(r != RL78_STOP_UNDEFINED, "core S%d %s: exec says undefined", core, text);
    if (!is_flow(text) && r != RL78_STOP_UNDEFINED)
        CHECKF(rl78_get_reg(cpu, RL78_REG_PC) == 0x200u + (uint32_t)len,
               "core S%d %s: PC advanced to %X, length %d", core, text,
               (unsigned)rl78_get_reg(cpu, RL78_REG_PC), len);
    /* re-assemble with the independent table */
    {
        int l2 = tasm_one(text, 0x200, re, err, sizeof err);
        CHECKF(l2 == len && memcmp(re, b, (size_t)len) == 0,
               "core S%d %02X %02X %02X %02X: '%s' re-assembles to len %d (%s)", core, b[0], b[1],
               b[2], b[3], text, l2, l2 < 0 ? err : "");
    }
}

static void sweep(rl78_core core)
{
    rl78_cpu *cpu = t_cpu(core);
    unsigned o, o2, es;
    rl78_config cfg = *rl78_get_config(cpu);
    rl78_destroy(cpu);
    cfg.halt_policy = RL78_SLEEP_IGNORE;
    cfg.stop_policy = RL78_SLEEP_IGNORE;
    cpu = rl78_create(&cfg);
    for (es = 0; es < 2; es++) {
        for (o = 0; o < 256; o++) {
            uint8_t b[8];
            unsigned k = 0;
            if (es) b[k++] = 0x11;
            if (o == 0x11) continue;
            if (o == 0x61 || o == 0x71 || o == 0x31) {
                for (o2 = 0; o2 < 256; o2++) {
                    int u;
                    k = es ? 1u : 0u;
                    b[k] = (uint8_t)o; b[k + 1] = (uint8_t)o2; b[k + 2] = 0xFA;
                    b[k + 3] = 0x05; b[k + 4] = 0x06; b[k + 5] = 0x07;
                    if (o == 0x61) u = map2_undef(o2, core);
                    else if (o == 0x71) u = map3_undef(o2);
                    else u = map4_undef(o2);
                    if (o == 0x71 && (o2 & 0xF) >= 9 && o2 < 0x80) b[k + 2] = 0x10; /* sfr */
                    check_one(cpu, core, b, es ? -1 : u);
                    if (o == 0x71 && o2 < 0x80 && ((o2 & 0xF) >= 9)) {
                        b[k + 2] = 0xFA; /* PSW forms too */
                        check_one(cpu, core, b, es ? -1 : u);
                    }
                }
                continue;
            }
            b[k] = (uint8_t)o; b[k + 1] = 0x20; b[k + 2] = 0xFE; b[k + 3] = 0x05;
            b[k + 4] = 0x06; b[k + 5] = 0x07;
            check_one(cpu, core, b, es ? -1 : map1_undef(o));
            if (o == 0xCE) { /* sfr = FBH: S3 extension instructions */
                unsigned sub;
                for (sub = 0; sub < 16; sub++) {
                    b[k + 1] = 0xFB; b[k + 2] = (uint8_t)sub;
                    check_one(cpu, core, b, es ? -1 : 0);
                }
                b[k + 1] = 0xFA; check_one(cpu, core, b, es ? -1 : 0);
            }
        }
    }
    rl78_destroy(cpu);
}

/* ---- tasm table -> disasm --------------------------------------------------- */

static void hexh(char *out, size_t n, unsigned v, int digits)
{
    char t[16];
    snprintf(t, sizeof t, "%0*X", digits, v);
    snprintf(out, n, "%s%sH", (t[0] >= 'A' && t[0] <= 'F') ? "0" : "", t);
}

/* replace pattern kinds by sample values; es: 1 to insert ES: at '%' */
static void instantiate(const char *pat, int es, uint32_t pc, char *out, size_t n)
{
    size_t o = 0;
    const char *p = pat;
    char tmp[32];
    while (*p && o + 20 < n) {
        const char *rep = NULL;
        size_t skip = 0;
        if (*p == '%') { if (es) { memcpy(out + o, "ES:", 3); o += 3; } p++; continue; }
        if (!strncmp(p, "#b", 2)) { rep = "#12H"; skip = 2; }
        else if (!strncmp(p, "#w", 2)) { rep = "#1234H"; skip = 2; }
        else if (!strncmp(p, "!!A", 3)) { rep = "!!12345H"; skip = 3; }
        else if (!strncmp(p, "!a", 2)) { rep = "!0FE80H"; skip = 2; }
        else if (!strncmp(p, "$!R", 3)) { hexh(tmp, sizeof tmp, (pc + 0x1000u) & 0xFFFFFu, 5); out[o++] = '$'; out[o++] = '!'; rep = tmp; skip = 3; }
        else if (!strncmp(p, "$r", 2)) { hexh(tmp, sizeof tmp, (pc + 0x10u) & 0xFFFFFu, 5); out[o++] = '$'; rep = tmp; skip = 2; }
        else if (!strncmp(p, "[HL+o]", 6)) { rep = "[HL+10H]"; skip = 6; }
        else if (!strncmp(p, "[DE+o]", 6)) { rep = "[DE+10H]"; skip = 6; }
        else if (!strncmp(p, "[SP+o]", 6)) { rep = "[SP+10H]"; skip = 6; }
        else if (!strncmp(p, "W[", 2)) { rep = "0FE70H"; skip = 1; }
        else if ((*p == 's' || *p == 'S') && (p[1] == 0 || p[1] == ',' || p[1] == '.') && (p == pat || p[-1] == ' ')) { rep = "0FE20H"; skip = 1; }
        else if ((*p == 'f' || *p == 'F') && (p[1] == 0 || p[1] == ',' || p[1] == '.') && (p == pat || p[-1] == ' ')) { rep = "0FFF10H"; skip = 1; }
        if (rep) {
            size_t l = strlen(rep);
            memcpy(out + o, rep, l);
            o += l;
            p += skip;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

static void table_roundtrip(void)
{
    int i, n = tasm_table_size(), aliases = 0;
    for (i = 0; i < n; i++) {
        const char *pat, *bytes;
        int es;
        tasm_table_entry(i, &pat, &bytes);
        for (es = 0; es < 2; es++) {
            char text[96], dis[96], err[128];
            uint8_t b[8], b2[8];
            int len, dl;
            rl78_core core = strstr(pat, "MULH") || strstr(pat, "DIV") || strstr(pat, "MACH")
                                 ? RL78_CORE_S3 : RL78_CORE_S2;
            if (es && !strchr(pat, '%')) continue;
            instantiate(pat, es, 0x1000, text, sizeof text);
            len = tasm_one(text, 0x1000, b, err, sizeof err);
            CHECKF(len > 0, "tasm cannot assemble instantiated '%s' (%s)", text, err);
            if (len <= 0) continue;
            dl = rl78_disasm_buf(core, b, (size_t)len, 0x1000, dis, sizeof dis);
            CHECKF(dl == len, "'%s': disasm length %d, asm length %d", text, dl, len);
            if (strcmp(dis, text)) {
                /* alias forms (e.g. MOVW BC, SP == MOVW BC, !0FFF8H) must encode identically */
                int l2 = tasm_one(dis, 0x1000, b2, err, sizeof err);
                aliases++;
                if (getenv("RL78_TEST_VERBOSE")) printf("  alias: '%s' -> '%s'\n", text, dis);
                CHECKF(l2 == len && !memcmp(b, b2, (size_t)len), "'%s' disassembles to '%s'", text, dis);
            }
        }
    }
    CHECKF(aliases == 3, "expected 3 alias forms (MOVW rp, SP), got %d", aliases);
}

/* ES:-capable opcode keys: exec/disasm vs tasm '%' patterns */
static void es_sets(void)
{
    static unsigned char from_tasm[4][256], from_dis[4][256]; /* [map][op]: 0=map1,1=61,2=71,3=31 */
    int i, n = tasm_table_size();
    unsigned o, o2;
    memset(from_tasm, 0, sizeof from_tasm);
    memset(from_dis, 0, sizeof from_dis);
    for (i = 0; i < n; i++) {
        const char *pat, *bytes;
        unsigned b0, b1 = 0;
        tasm_table_entry(i, &pat, &bytes);
        if (!strchr(pat, '%')) continue;
        b0 = (unsigned)strtoul(bytes, NULL, 16);
        if (b0 == 0x61 || b0 == 0x71 || b0 == 0x31) b1 = (unsigned)strtoul(bytes + 3, NULL, 16);
        if (b0 == 0x61) from_tasm[1][b1] = 1;
        else if (b0 == 0x71) from_tasm[2][b1] = 1;
        else if (b0 == 0x31) from_tasm[3][b1] = 1;
        else from_tasm[0][b0] = 1;
    }
    for (o = 0; o < 256; o++) {
        uint8_t b[8] = {0x11, 0, 0x10, 0xFE, 0, 0, 0, 0};
        char t[96];
        if (o == 0x61 || o == 0x71 || o == 0x31) {
            for (o2 = 0; o2 < 256; o2++) {
                b[1] = (uint8_t)o; b[2] = (uint8_t)o2; b[3] = 0x10;
                if (rl78_disasm_buf(RL78_CORE_S3, b, 8, 0, t, sizeof t)) {
                    if (o == 0x61) from_dis[1][o2] = 1;
                    else from_dis[o == 0x71 ? 2 : 3][o2] = 1;
                }
            }
            continue;
        }
        b[1] = (uint8_t)o;
        if (rl78_disasm_buf(RL78_CORE_S3, b, 8, 0, t, sizeof t)) from_dis[0][o] = 1;
    }
    for (i = 0; i < 4; i++)
        for (o = 0; o < 256; o++)
            CHECKF(from_tasm[i][o] == from_dis[i][o], "ES: capability mismatch map%d op %02X: table %d disasm %d",
                   i, o, from_tasm[i][o], from_dis[i][o]);
}

void suite_decode(void)
{
    rl78_core c;
    for (c = RL78_CORE_S1; c <= RL78_CORE_AUTO; c = (rl78_core)(c + 1)) sweep(c);
    printf("  decode sweep: %lu defined encodings (%lu with ES:), %lu undefined\n", n_defined,
           n_es_defined, n_undefined);
    table_roundtrip();
    es_sets();
}
