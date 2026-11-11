/*
 * t_regress.c - regression vectors produced by tests/difftest/difftest.py
 * from cases on which this core and the GNU RL78 simulator agreed (excluding
 * the known GNU deviations, see tests/difftest/README.md).  Runs without the
 * GNU tools: the initial state is re-derived from the seed with the same
 * xorshift32 generator and layout as difftest.py's init_state().
 */
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

typedef struct {
    int core;
    uint32_t seed;
    int kind, rp, ret_psw;
    uint32_t addr, stop, label;
    int len;
    const char *code;
    const char *diffs;
} rvec;

static const rvec RV[] = {
#include "difftest_vectors.inc"
};

static uint32_t xs_state;
static uint32_t xs_next(void)
{
    uint32_t s = xs_state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    xs_state = s;
    return s;
}
static unsigned xs_byte(void) { return xs_next() & 0xFF; }

static const struct { uint32_t base; int n; } REG[] = {
    {0xFFE20, 8}, {0xFFE70, 64}, {0xFFDF8, 16}, {0xFFEE0, 32}, {0xFFFF0, 8}};
#define NSTATE (13 + 8 + 64 + 16 + 32 + 8)

static uint32_t reg_addr(unsigned psw, int idx) /* idx: X A C B E D L H */
{
    unsigned bank = ((psw >> 3) & 1u) | ((psw >> 4) & 2u);
    return 0xFFEF8u - 8u * bank + (uint32_t)idx;
}

static void full_state(rl78_cpu *cpu, uint8_t *st)
{
    int k = 0, r, i;
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_A);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_PSW);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_X);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_C);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_B);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_E);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_D);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_L);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_H);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_ES);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_CS);
    st[k++] = (uint8_t)(rl78_get_reg(cpu, RL78_REG_SP) >> 8);
    st[k++] = (uint8_t)rl78_get_reg(cpu, RL78_REG_SP);
    for (r = 0; r < 5; r++)
        for (i = 0; i < REG[r].n; i++) st[k++] = rl78_peek8(cpu, REG[r].base + (uint32_t)i);
}

static void run_vector(const rvec *v, int idx)
{
    static uint8_t img[0x100000];
    rl78_config cfg;
    rl78_cpu *cpu;
    uint8_t *m;
    int r, i;
    unsigned psw;
    uint8_t exp[NSTATE], got[NSTATE];
    uint32_t a;
    static uint8_t have[0x100000];

    rl78_default_config(&cfg, (rl78_core)v->core);
    cfg.mdu = (v->core == 2);
    if (v->core == 1) { cfg.mirror_start = 0xF8000; cfg.mirror_end = 0xF9000; cfg.mirror_src = 0; }
    else { cfg.mirror_start = 0xF1000; cfg.mirror_end = 0xF8000; cfg.mirror_src = 0x1000; }
    cpu = rl78_create(&cfg);
    m = rl78_memory(cpu);
    memset(m, 0xFF, 0xF0000);
    memset(m + 0xF0000, 0x33, 0x10000);

    /* initial state from the seed (difftest.py: init_state) */
    xs_state = v->seed ? v->seed : 0x9E3779B9u;
    memset(have, 0, sizeof have);
    for (r = 0; r < 5; r++)
        for (i = 0; i < REG[r].n; i++) {
            img[REG[r].base + (uint32_t)i] = (uint8_t)xs_byte();
            have[REG[r].base + (uint32_t)i] = 1;
        }
    for (i = 0; i < 4; i++) {
        uint32_t b = 0xFFEF8u - 8u * (uint32_t)i;
        img[b + 2] = (uint8_t)(xs_byte() & 0x0F);
        img[b + 3] = (uint8_t)(xs_byte() & 0x0F);
        img[b + 4] = (uint8_t)(0x70 + (xs_byte() & 0x0E));
        img[b + 5] = 0xFE;
        img[b + 6] = (uint8_t)(0x70 + (xs_byte() & 0x0E));
        img[b + 7] = 0xFE;
    }
    psw = xs_byte();
    if (v->core == 1) psw &= ~0x28u;
    if (v->rp >= 0) {
        uint32_t lo = reg_addr(psw, v->rp * 2);
        img[lo] = (uint8_t)v->label;
        img[lo + 1] = (uint8_t)(v->label >> 8);
    }
    if (v->kind == 1) {
        img[0xFFE00] = (uint8_t)v->label;
        img[0xFFE01] = (uint8_t)(v->label >> 8);
        img[0xFFE02] = 0;
        img[0xFFE03] = (uint8_t)v->ret_psw;
    }
    /* write through the bus in ascending address order (as the generated
       initialisation code does), so that e.g. the S2 multiplier sees the
       same register writes */
    for (a = 0xFFDF8; a < 0x100000; a++)
        if (have[a]) rl78_write8(cpu, a, img[a]);
    rl78_load(cpu, v->addr, v->code, (size_t)v->len);
    rl78_set_reg(cpu, RL78_REG_ES, 0x0F);
    rl78_set_reg(cpu, RL78_REG_CS, 0);
    rl78_set_reg(cpu, RL78_REG_SP, 0xFE00);
    rl78_set_reg(cpu, RL78_REG_PSW, psw);
    /* note: initial register values for the diff baseline are the raw
       seed-derived bytes (before any multiplier side effects) */
    {
        uint8_t raw[NSTATE];
        int k = 0;
        unsigned bank_psw = psw;
        static const int order[9] = {1, -1, 0, 2, 3, 4, 5, 6, 7}; /* A PSW X C B E D L H */
        for (i = 0; i < 9; i++) raw[k++] = order[i] < 0 ? (uint8_t)psw : img[reg_addr(bank_psw, order[i])];
        raw[k++] = 0x0F; raw[k++] = 0; raw[k++] = 0xFE; raw[k++] = 0x00;
        for (r = 0; r < 5; r++)
            for (i = 0; i < REG[r].n; i++) raw[k++] = img[REG[r].base + (uint32_t)i];
        /* rebuild expected from the raw baseline */
        memcpy(exp, raw, sizeof exp);
        {
            const char *p = v->diffs;
            while (*p) {
                char *e;
                long kk = strtol(p, &e, 10);
                long val = strtol(e + 1, &e, 10);
                if (kk >= 0 && kk < NSTATE) exp[kk] = (uint8_t)val;
                p = (*e == ';') ? e + 1 : e;
            }
        }
    }
    rl78_set_reg(cpu, RL78_REG_PC, v->addr);
    for (i = 0; i < 400 && rl78_get_reg(cpu, RL78_REG_PC) != v->stop; i++) {
        rl78_stop_reason sr = rl78_step(cpu);
        if (sr != RL78_STOP_LIMIT) break;
    }
    full_state(cpu, got);
    {
        int bad = -1;
        for (i = 0; i < NSTATE; i++)
            if (got[i] != exp[i]) { bad = i; break; }
        CHECKF(bad < 0 && rl78_get_reg(cpu, RL78_REG_PC) == v->stop,
               "regress #%d (S%d seed %u, code at %05X): state byte %d got %02X expected %02X, pc %05X stop %05X",
               idx, v->core, (unsigned)v->seed, (unsigned)v->addr, bad, bad >= 0 ? got[bad] : 0,
               bad >= 0 ? exp[bad] : 0, (unsigned)rl78_get_reg(cpu, RL78_REG_PC), (unsigned)v->stop);
    }
    rl78_destroy(cpu);
}

void suite_regress(void)
{
    size_t i, n = sizeof RV / sizeof RV[0];
    for (i = 0; i < n; i++) run_vector(&RV[i], (int)i);
    printf("  difftest regression vectors: %zu\n", n);
    CHECK(n > 0);
}
