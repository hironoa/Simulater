/*
 * t_manual.c - every row of the manual's operation lists (Tables 5-5, 5-6,
 * 5-7; generated into manual_ops.inc) is instantiated with concrete operands,
 * assembled with the test assembler and checked for:
 *   - instruction length == "Bytes" column,
 *   - disassembler text == instantiated text,
 *   - executed clock count == "Clock cycles Note 1" (data in RAM) and
 *     "Note 2" (data read from flash) where given; for conditional branches
 *     both the not-taken and the taken counts.
 */
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

typedef struct {
    int core;
    const char *mn, *ops;
    int bytes;
    const char *clk1, *clk2;
} oprow;

static const oprow rows[] = {
#include "manual_ops.inc"
};

static void hexh(char *out, size_t n, unsigned v, int digits)
{
    char t[16];
    snprintf(t, sizeof t, "%0*X", digits, v);
    snprintf(out, n, "%s%sH", (t[0] >= 'A' && t[0] <= 'F') ? "0" : "", t);
}

#define CODE 0x0200u
#define TARGET 0x0300u

static int isalnum_c(char c);

/* replace whole-word token `from` in s by `to` */
static void subst(char *s, size_t n, const char *from, const char *to)
{
    char out[160];
    size_t o = 0, fl = strlen(from), tl = strlen(to);
    const char *p = s;
    while (*p && o + tl + 1 < sizeof out) {
        int boundary_l = (p == s) || !(isalnum_c(p[-1]));
        if (boundary_l && !strncmp(p, from, fl) && !isalnum_c(p[fl])) {
            memcpy(out + o, to, tl);
            o += tl;
            p += fl;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    snprintf(s, n, "%s", out);
}

static int isalnum_c(char c)
{
    /* used for whole-word matching */
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

typedef struct {
    unsigned base;  /* data address (16-bit) */
    int flash;
} memcase;

static void build_text(const oprow *r, const char *rsub, const char *rpsub, int cnt, int bank,
                       int bit, memcase mc, char *out, size_t n)
{
    char ops[128], t[32];
    snprintf(ops, sizeof ops, "%s", r->ops);
    /* bit operands first (".bit" suffix) */
    {
        char b[8];
        snprintf(b, sizeof b, ".%d", bit);
        while (strstr(ops, ".bit")) {
            char *q = strstr(ops, ".bit");
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(q - ops), ops, b, q + 4);
            snprintf(ops, sizeof ops, "%s", tmp);
        }
    }
    hexh(t, sizeof t, mc.base, 4);
    {
        char a16[32], w[32], wc[32];
        snprintf(a16, sizeof a16, "!%s", t);
        hexh(w, sizeof w, mc.base, 4);
        hexh(wc, sizeof wc, (mc.base - 0x10u) & 0xFFFFu, 4);
        if (!strcmp(r->mn, "CALL") || !strcmp(r->mn, "BR")) {
            char tg[32];
            hexh(tg, sizeof tg, TARGET, 4);
            snprintf(a16, sizeof a16, "!%s", tg);
        }
        /* order matters: longer tokens first */
        {
            char *q;
            while ((q = strstr(ops, "!!addr20")) != NULL) {
                char tmp[128], tg[32];
                hexh(tg, sizeof tg, TARGET, 5);
                snprintf(tmp, sizeof tmp, "%.*s!!%s%s", (int)(q - ops), ops, tg, q + 8);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "$!addr20")) != NULL) {
                char tmp[128], tg[32];
                hexh(tg, sizeof tg, TARGET, 5);
                snprintf(tmp, sizeof tmp, "%.*s$!%s%s", (int)(q - ops), ops, tg, q + 8);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "$addr20")) != NULL) {
                char tmp[128], tg[32];
                hexh(tg, sizeof tg, CODE + 0x20u, 5);
                snprintf(tmp, sizeof tmp, "%.*s$%s%s", (int)(q - ops), ops, tg, q + 7);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "!addr16")) != NULL) {
                char tmp[128];
                snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(q - ops), ops, a16, q + 7);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "word[BC]")) != NULL) {
                char tmp[128];
                snprintf(tmp, sizeof tmp, "%.*s%s[BC]%s", (int)(q - ops), ops, wc, q + 8);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "word[C]")) != NULL) {
                char tmp[128];
                snprintf(tmp, sizeof tmp, "%.*s%s[C]%s", (int)(q - ops), ops, wc, q + 7);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "word[B]")) != NULL) {
                char tmp[128];
                snprintf(tmp, sizeof tmp, "%.*s%s[B]%s", (int)(q - ops), ops, w, q + 7);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
            while ((q = strstr(ops, "+byte]")) != NULL) {
                char tmp[128];
                snprintf(tmp, sizeof tmp, "%.*s+10H]%s", (int)(q - ops), ops, q + 6);
                snprintf(ops, sizeof ops, "%s", tmp);
            }
        }
    }
    subst(ops, sizeof ops, "#byte", "#12H");
    subst(ops, sizeof ops, "#word", "#1234H");
    subst(ops, sizeof ops, "saddrp", "0FE20H");
    subst(ops, sizeof ops, "saddr", "0FE20H");
    subst(ops, sizeof ops, "sfrp", "0FFF10H");
    subst(ops, sizeof ops, "sfr", "0FFF10H");
    subst(ops, sizeof ops, "[addr5]", "[0080H]");
    {
        char c[8];
        snprintf(c, sizeof c, "%d", cnt);
        subst(ops, sizeof ops, "cnt", c);
        snprintf(c, sizeof c, "RB%d", bank);
        subst(ops, sizeof ops, "RBn", c);
    }
    if (rsub) subst(ops, sizeof ops, "r", rsub);
    if (rpsub) subst(ops, sizeof ops, "rp", rpsub);
    if (ops[0]) snprintf(out, n, "%s %s", r->mn, ops);
    else snprintf(out, n, "%s", r->mn);
}

/* subst() uses isalnum_c before its definition above */

static int parse_clk(const char *s, int *a, int *b)
{
    if (!s || !*s) return 0;
    *a = atoi(s);
    *b = *a;
    if (strchr(s, '/')) *b = atoi(strchr(s, '/') + 1);
    return 1;
}

static unsigned long n_rows, n_inst, n_clk;

static rl78_cpu *g_cpu[4];

static void setup(rl78_cpu *cpu, const memcase *mc, unsigned psw, int bitval, int bit,
                  const char *text)
{
    uint8_t stack[4] = {(uint8_t)(TARGET & 0xFF), (uint8_t)(TARGET >> 8), 0x00, 0x06};
    uint8_t vec[2] = {(uint8_t)(TARGET & 0xFF), (uint8_t)(TARGET >> 8)};
    uint8_t m;
    rl78_set_reg(cpu, RL78_REG_PMC, 0);
    rl78_set_reg(cpu, RL78_REG_PSW, psw);
    rl78_set_reg(cpu, RL78_REG_SP, 0xFE00);
    rl78_set_reg(cpu, RL78_REG_ES, 0x0F);
    rl78_set_reg(cpu, RL78_REG_CS, 0);
    rl78_set_reg(cpu, RL78_REG_HL, mc->base);
    rl78_set_reg(cpu, RL78_REG_DE, mc->base);
    rl78_set_reg(cpu, RL78_REG_BC, 0x0010); /* B = 0, C = 10H */
    rl78_set_reg(cpu, RL78_REG_AX, 0x0102);
    if (strstr(text, "CALL AX") || strstr(text, "BR AX")) rl78_set_reg(cpu, RL78_REG_AX, TARGET);
    if (strstr(text, "CALL BC")) rl78_set_reg(cpu, RL78_REG_BC, TARGET);
    if (strstr(text, "CALL DE")) rl78_set_reg(cpu, RL78_REG_DE, TARGET);
    if (strstr(text, "CALL HL")) rl78_set_reg(cpu, RL78_REG_HL, TARGET);
    rl78_load(cpu, 0xFFE00, stack, 4);
    rl78_load(cpu, 0x7E, vec, 2);
    rl78_load(cpu, 0x80, vec, 2);
    /* condition bit for BT/BF/BTCLR */
    m = (uint8_t)(1u << bit);
    {
        uint8_t v = bitval ? 0xFF : 0x00;
        (void)m;
        rl78_load(cpu, 0xFFE20, &v, 1);
        rl78_load(cpu, 0xFFF10, &v, 1);
        rl78_load(cpu, mc->flash ? (0x00000u | mc->base) : (0xF0000u | mc->base), &v, 1);
        if (strstr(text, " A.")) rl78_set_reg(cpu, RL78_REG_AX, bitval ? 0xFF02u : 0x0002u);
    }
}

static void check_row(const oprow *r)
{
    rl78_cpu *cpu = g_cpu[r->core];
    const char *regs8[8] = {"X", "A", "C", "B", "E", "D", "L", "H"};
    const char *regs16[4] = {"AX", "BC", "DE", "HL"};
    int has_r = 0, has_rp = 0, has_cnt = 0, has_bank = 0;
    int ri, pi, ci, bi, instances = 0;
    int c1a, c1b, c2a = -1, c2b = -1;
    int maxcnt = 7;
    char ops[128];
    snprintf(ops, sizeof ops, " %s ", r->ops);
    {
        /* detect generic operands */
        const char *p;
        for (p = ops; *p; p++) {
            if (p[0] == 'r' && !isalnum_c(p[-1]) && !isalnum_c(p[1])) has_r = 1;
            if (p[0] == 'r' && p[1] == 'p' && !isalnum_c(p[-1]) && !isalnum_c(p[2])) has_rp = 1;
        }
        if (has_rp) has_r = 0;
        if (strstr(ops, "cnt")) has_cnt = 1;
        if (strstr(ops, "RBn")) has_bank = 1;
        if (strstr(ops, "AX, cnt") || strstr(ops, "BC, cnt")) maxcnt = 15;
    }
    parse_clk(r->clk1, &c1a, &c1b);
    parse_clk(r->clk2, &c2a, &c2b);
    n_rows++;
    for (ri = 0; ri < (has_r ? 8 : 1); ri++)
        for (pi = 0; pi < (has_rp ? 4 : 1); pi++)
            for (ci = 1; ci <= (has_cnt ? maxcnt : 1); ci++)
                for (bi = 0; bi < (has_bank ? 4 : 1); bi++) {
                    int mcase;
                    for (mcase = 0; mcase < (r->clk2[0] ? 2 : 1); mcase++) {
                        memcase mc;
                        char text[128], dis[128], err[128];
                        uint8_t b[8];
                        int len, bit, cond;
                        int seen_nt = 0, seen_t = 0;
                        mc.flash = mcase;
                        mc.base = mcase ? 0x2400u : 0xFE80u;
                        bit = strstr(r->ops, "PSW.bit") ? 0 : 3;
                        build_text(r, has_r ? regs8[ri] : NULL, has_rp ? regs16[pi] : NULL, ci, bi,
                                   bit, mc, text, sizeof text);
                        len = tasm_one(text, CODE, b, err, sizeof err);
                        if (len < 0) continue; /* e.g. MOV A, A does not exist */
                        instances++;
                        n_inst++;
                        {
                            int expect = r->bytes;
                            if (!strcmp(r->mn, "XCH") && !strcmp(r->ops, "A, r")) expect = (ri == 0) ? 1 : 2;
                            CHECKF(len == expect, "S%d %s: length %d, manual %d", r->core, text, len, expect);
                        }
                        rl78_disasm_buf((rl78_core)r->core, b, (size_t)len, CODE, dis, sizeof dis);
                        {
                            /* MOVW rp, SP and similar are aliases of !addr16 forms */
                            uint8_t b2[8];
                            int l2 = tasm_one(dis, CODE, b2, err, sizeof err);
                            CHECKF(!strcmp(dis, text) || (l2 == len && !memcmp(b, b2, (size_t)len)),
                                   "S%d disasm '%s' != '%s'", r->core, dis, text);
                        }
                        /* clocks: several condition variants (flags / bit values) */
                        for (cond = 0; cond < 4; cond++) {
                            static const unsigned psws[4] = {0x06, 0x47, 0x06, 0x47};
                            uint64_t c0;
                            uint32_t pc_after;
                            int got, want;
                            rl78_load(cpu, CODE, b, (size_t)len);
                            setup(cpu, &mc, psws[cond], cond >= 2, bit, text);
                            rl78_set_reg(cpu, RL78_REG_PC, CODE);
                            c0 = rl78_clock_count(cpu);
                            rl78_step(cpu);
                            got = (int)(rl78_clock_count(cpu) - c0);
                            pc_after = rl78_get_reg(cpu, RL78_REG_PC);
                            if (!strncmp(r->mn, "SK", 2) && pc_after != CODE + (uint32_t)len) continue; /* skipped */
                            {
                                int taken = strchr(r->clk1, '/') && pc_after != CODE + (uint32_t)len;
                                if (taken) seen_t = 1; else seen_nt = 1;
                                if (mcase) want = taken ? c2b : c2a;
                                else want = taken ? c1b : c1a;
                            }
                            n_clk++;
                            CHECKF(got == want, "S%d %s (%s, cond %d): %d clocks, manual %d", r->core,
                                   text, mcase ? "flash" : "RAM", cond, got, want);
                        }
                        if (strchr(r->clk1, '/'))
                            CHECKF(seen_t && seen_nt, "S%d %s: taken/not-taken not both exercised", r->core, text);
                    }
                }
    CHECKF(instances > 0, "S%d manual row '%s %s' could not be instantiated/assembled", r->core,
           r->mn, r->ops);
}

void suite_manual(void)
{
    size_t i;
    int c;
    for (c = 1; c <= 3; c++) {
        rl78_config cfg;
        rl78_default_config(&cfg, (rl78_core)c);
        cfg.rom_size = 0x10000;
        cfg.mirror_start = 0xF2000;
        cfg.mirror_end = 0xFC000;
        cfg.mirror_src = 0x02000;
        cfg.halt_policy = RL78_SLEEP_IGNORE;
        cfg.stop_policy = RL78_SLEEP_IGNORE;
        g_cpu[c] = rl78_create(&cfg);
    }
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) check_row(&rows[i]);
    printf("  manual rows: %lu, instances: %lu, clock measurements: %lu\n", n_rows, n_inst, n_clk);
    CHECK(n_rows > 1400);
    for (c = 1; c <= 3; c++) rl78_destroy(g_cpu[c]);
}
