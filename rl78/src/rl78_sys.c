/*
 * rl78_sys.c - memory system, SFRs, multiplier/divider unit, interrupts,
 * breakpoints, hooks, snapshots and the public run loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rl78_internal.h"

uint64_t rl78__exec(rl78_cpu *C, uint64_t lim, int *why);

#define NO_BP 0xFFFFFFFFu

/* G13 multiplier/divider SFRs (RL78/G13 hardware manual, chapter 14) */
#define MDAL 0xFFFF0u
#define MDAH 0xFFFF2u
#define MDBH 0xFFFF4u
#define MDBL 0xFFFF6u
#define MDCL 0xF00E0u
#define MDCH 0xF00E2u
#define MDUC 0xF00E8u

void rl78_default_config(rl78_config *cfg, rl78_core core)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->core = core;
    cfg->halt_policy = RL78_SLEEP_RETURN;
    cfg->stop_policy = RL78_SLEEP_RETURN;
    cfg->irq_clocks = 9;
}

/* ---- page tables -------------------------------------------------------- */

static int mirror_maa(const rl78_cpu *c)
{
    return c->cfg.core != RL78_CORE_S1 && (c->pmc & 1u);
}

static int in_mirror(const rl78_cpu *c, uint32_t a)
{
    return c->cfg.mirror_end != 0 && a >= c->cfg.mirror_start && a < c->cfg.mirror_end;
}

static uint32_t mirror_src(const rl78_cpu *c, uint32_t a)
{
    return (c->cfg.mirror_src + (a - c->cfg.mirror_start) + (mirror_maa(c) ? 0x10000u : 0u)) &
           0xFFFFFu;
}

static int is_rom(const rl78_cpu *c, uint32_t a)
{
    return c->cfg.rom_size != 0 && a < c->cfg.rom_size;
}

static int is_flash(const rl78_cpu *c, uint32_t a)
{
    if (in_mirror(c, a)) return 1;
    if (c->cfg.data_flash_size && a >= c->cfg.data_flash_start &&
        a < c->cfg.data_flash_start + c->cfg.data_flash_size)
        return 1;
    if (c->cfg.rom_size) return a < c->cfg.rom_size;
    return a < 0xF0000u;
}

static int hook_at(const rl78_cpu *c, uint32_t a)
{
    int i;
    for (i = 0; i < c->nhooks; i++)
        if (a >= c->hooks[i].start && a < c->hooks[i].end) return i;
    return -1;
}

static int bp_at(const rl78_cpu *c, uint32_t a)
{
    return c->bpbits && (c->bpbits[a >> 3] & (1u << (a & 7u)));
}

void rl78__rebuild_pages(rl78_cpu *c)
{
    uint32_t pg;
    for (pg = 0; pg < RL78_NPAGES; pg++) {
        uint32_t base = pg << 8, end = base + 256u;
        int slow = 0, i;
        const uint8_t *r = c->mem + base;
        uint8_t *w = c->mem + base;

        c->flashpen[pg] = is_flash(c, base) ? 3u : 0u;
        if (pg == 0xFFFu) slow = 1;                       /* fixed SFRs */
        if (c->cfg.mdu && pg == (MDCL >> 8)) slow = 1;    /* MDCL/MDCH/MDUC */
        for (i = 0; i < c->nhooks; i++)
            if (c->hooks[i].start < end && c->hooks[i].end > base) slow = 1;
        if (c->bppage[pg]) slow = 1;
        if (c->cfg.mirror_end != 0 && c->cfg.mirror_start < end && c->cfg.mirror_end > base) {
            if (c->cfg.mirror_start <= base && c->cfg.mirror_end >= end) {
                r = c->mem + mirror_src(c, base);
                w = c->sink;
            } else {
                slow = 1;
            }
        }
        if (c->cfg.rom_size != 0 && base < c->cfg.rom_size) {
            if (end <= c->cfg.rom_size) w = c->sink;
            else slow = 1;
        }
        c->rp[pg] = slow ? NULL : r;
        c->wp[pg] = slow ? NULL : w;
    }
}

/* ---- PSW / SFR ------------------------------------------------------------ */

void rl78__set_psw(rl78_cpu *c, uint8_t v)
{
    unsigned bank;
    if (c->cfg.core == RL78_CORE_S1) v &= (uint8_t)~(RL78_PSW_RBS0 | RL78_PSW_RBS1);
    c->psw = v;
    bank = ((v >> 3) & 1u) | ((v >> 4) & 2u);
    c->rb = c->mem + 0xFFEF8u - 8u * bank;
}

static void mdu_update(rl78_cpu *c, uint32_t a)
{
    uint8_t *m = c->mem;
    unsigned mduc = m[MDUC];
    unsigned mdal = m[MDAL] | (m[MDAL + 1] << 8);
    unsigned mdah = m[MDAH] | (m[MDAH + 1] << 8);
    int sgn = (mduc & 0x08u) != 0;
    if (mduc & 0x80u) return; /* division mode: started by DIVST */
    if (a == MDAL || a == MDAL + 1 || a == MDAH || a == MDAH + 1) {
        uint32_t prod;
        if (sgn) prod = (uint32_t)((int32_t)(int16_t)mdal * (int32_t)(int16_t)mdah);
        else prod = (uint32_t)mdal * (uint32_t)mdah;
        m[MDBL] = (uint8_t)prod; m[MDBL + 1] = (uint8_t)(prod >> 8);
        m[MDBH] = (uint8_t)(prod >> 16); m[MDBH + 1] = (uint8_t)(prod >> 24);
        if ((mduc & 0x40u) && (a == MDAH || a == MDAH + 1)) { /* multiply-accumulate */
            uint32_t acc = m[MDCL] | (m[MDCL + 1] << 8) | ((uint32_t)m[MDCH] << 16) |
                           ((uint32_t)m[MDCH + 1] << 24);
            uint32_t res;
            unsigned of;
            if (sgn) {
                int64_t s = (int64_t)(int32_t)acc + (int64_t)(int32_t)prod;
                res = (uint32_t)s;
                of = (s > 0x7FFFFFFFLL || s < -0x80000000LL);
            } else {
                uint64_t s = (uint64_t)acc + prod;
                res = (uint32_t)s;
                of = (s >> 32) != 0;
            }
            m[MDCL] = (uint8_t)res; m[MDCL + 1] = (uint8_t)(res >> 8);
            m[MDCH] = (uint8_t)(res >> 16); m[MDCH + 1] = (uint8_t)(res >> 24);
            mduc &= ~0x06u;
            if (of) mduc |= 0x04u;
            if (sgn && (res & 0x80000000u)) mduc |= 0x02u;
            m[MDUC] = (uint8_t)mduc;
        }
    }
    if (sgn && (mduc & 0x40u) && (a == MDCH + 1 || a == MDCH)) {
        /* MACSF follows the sign of the initial accumulated value (14.4.4 <3>) */
        mduc = (mduc & ~0x02u) | ((m[MDCH + 1] & 0x80u) ? 0x02u : 0u);
        m[MDUC] = (uint8_t)mduc;
    }
}

static void mdu_divide(rl78_cpu *c)
{
    uint8_t *m = c->mem;
    uint32_t a = m[MDAL] | (m[MDAL + 1] << 8) | ((uint32_t)m[MDAH] << 16) | ((uint32_t)m[MDAH + 1] << 24);
    uint32_t b = m[MDBL] | (m[MDBL + 1] << 8) | ((uint32_t)m[MDBH] << 16) | ((uint32_t)m[MDBH + 1] << 24);
    uint32_t q, r;
    /* divisor 0: result undefined on hardware; we return quotient FFFFFFFFH,
       remainder = dividend (same convention as DIVWU) */
    if (b == 0) { q = 0xFFFFFFFFu; r = a; }
    else { q = a / b; r = a % b; }
    m[MDAL] = (uint8_t)q; m[MDAL + 1] = (uint8_t)(q >> 8);
    m[MDAH] = (uint8_t)(q >> 16); m[MDAH + 1] = (uint8_t)(q >> 24);
    m[MDCL] = (uint8_t)r; m[MDCL + 1] = (uint8_t)(r >> 8);
    m[MDCH] = (uint8_t)(r >> 16); m[MDCH + 1] = (uint8_t)(r >> 24);
    m[MDUC] = (uint8_t)(m[MDUC] & ~0x01u); /* DIVST cleared: operation done */
}

static int is_mdu_reg(const rl78_cpu *c, uint32_t a)
{
    if (!c->cfg.mdu) return 0;
    return (a >= MDAL && a < MDAL + 8u) || (a >= MDCL && a < MDCL + 4u) || a == MDUC;
}

/* raw (no hook) read with SFR semantics */
static uint8_t sys_rd8(rl78_cpu *c, uint32_t a)
{
    switch (a) {
    case 0xFFFF8u: return (uint8_t)c->sp;
    case 0xFFFF9u: return (uint8_t)(c->sp >> 8);
    case 0xFFFFAu: return c->psw;
    case 0xFFFFCu: return c->cs;
    case 0xFFFFDu: return c->es;
    case 0xFFFFEu: return c->pmc;
    default: break;
    }
    if (bp_at(c, a)) return c->bporig[a];
    if (in_mirror(c, a)) {
        uint32_t s = mirror_src(c, a);
        return bp_at(c, s) ? c->bporig[s] : c->mem[s];
    }
    return c->mem[a];
}

static void sys_wr8(rl78_cpu *c, uint32_t a, uint8_t v, int mdu_trigger)
{
    switch (a) {
    case 0xFFFF8u: c->sp = (uint16_t)((c->sp & 0xFF00u) | (v & 0xFEu)); return;
    case 0xFFFF9u: c->sp = (uint16_t)((c->sp & 0x00FFu) | ((unsigned)v << 8)); return;
    case 0xFFFFAu:
        rl78__set_psw(c, v);
        c->hold_req = 1;
        c->exit_req = 1;
        return;
    case 0xFFFFCu: c->cs = (uint8_t)(v & 0x0Fu); return;
    case 0xFFFFDu: c->es = (uint8_t)(v & 0x0Fu); return;
    case 0xFFFFEu: {
        uint8_t nv = (uint8_t)(v & 0x01u);
        if (c->cfg.core == RL78_CORE_S1) nv = 0; /* only 00H may be written */
        if (nv != c->pmc) {
            c->pmc = nv;
            rl78__rebuild_pages(c);
        }
        return;
    }
    default: break;
    }
    if (is_rom(c, a) || in_mirror(c, a)) return; /* read-only */
    if (bp_at(c, a)) { c->bporig[a] = v; return; }
    if (is_mdu_reg(c, a)) {
        if (a == MDUC) {
            /* bits 1,2 (MACSF, MACOF) are read-only */
            uint8_t old = c->mem[MDUC];
            c->mem[MDUC] = (uint8_t)((v & ~0x06u) | (old & 0x06u));
            if ((v & 0x80u) && (v & 0x01u)) mdu_divide(c);
            return;
        }
        c->mem[a] = v;
        if (mdu_trigger) mdu_update(c, a);
        return;
    }
    c->mem[a] = v;
}

void rl78__write_macr(rl78_cpu *c, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4; i++) {
        uint32_t a = 0xFFFF0u + i;
        int h = hook_at(c, a);
        uint8_t b = (uint8_t)(v >> (8 * i));
        if (h >= 0 && c->hooks[h].wr) c->hooks[h].wr(c->hooks[h].ctx, c, a, b, 1);
        else c->mem[a] = b;
    }
}

uint8_t rl78__rd8_slow(rl78_cpu *c, uint32_t a)
{
    int h = hook_at(c, a);
    if (h >= 0 && c->hooks[h].rd) return (uint8_t)c->hooks[h].rd(c->hooks[h].ctx, c, a, 1);
    return sys_rd8(c, a);
}

void rl78__wr8_slow(rl78_cpu *c, uint32_t a, uint8_t v)
{
    int h = hook_at(c, a);
    if (h >= 0) {
        if (c->hooks[h].wr) c->hooks[h].wr(c->hooks[h].ctx, c, a, v, 1);
        else c->mem[a] = v;
        return;
    }
    sys_wr8(c, a, v, 1);
}

uint16_t rl78__rd16_slow(rl78_cpu *c, uint32_t a)
{
    int h = hook_at(c, a);
    if (h >= 0 && c->hooks[h].rd && a + 1u < c->hooks[h].end)
        return (uint16_t)c->hooks[h].rd(c->hooks[h].ctx, c, a, 2);
    return (uint16_t)(rl78__rd8_slow(c, a) | (rl78__rd8_slow(c, a + 1u) << 8));
}

void rl78__wr16_slow(rl78_cpu *c, uint32_t a, uint16_t v)
{
    int h = hook_at(c, a);
    if (h >= 0 && a + 1u < c->hooks[h].end) {
        if (c->hooks[h].wr) c->hooks[h].wr(c->hooks[h].ctx, c, a, v, 2);
        else { c->mem[a] = (uint8_t)v; c->mem[a + 1] = (uint8_t)(v >> 8); }
        return;
    }
    if (a == 0xFFFF8u) { c->sp = (uint16_t)(v & 0xFFFEu); return; }
    if (hook_at(c, a) < 0 && hook_at(c, a + 1u) < 0 && is_mdu_reg(c, a)) {
        sys_wr8(c, a, (uint8_t)v, 0);
        sys_wr8(c, a + 1u, (uint8_t)(v >> 8), 0);
        mdu_update(c, a);
        return;
    }
    rl78__wr8_slow(c, a, (uint8_t)v);
    rl78__wr8_slow(c, a + 1u, (uint8_t)(v >> 8));
}

/* ---- creation --------------------------------------------------------------- */

rl78_cpu *rl78_create(const rl78_config *cfg)
{
    rl78_cpu *c = (rl78_cpu *)calloc(1, sizeof(rl78_cpu));
    if (!c) return NULL;
    c->mem = (uint8_t *)calloc(1, RL78_MEM_SIZE + RL78_MEM_PAD);
    if (!c->mem) { free(c); return NULL; }
    memset(c->mem + RL78_MEM_SIZE, 0xFF, RL78_MEM_PAD); /* run-off traps as undefined */
    if (cfg) c->cfg = *cfg;
    else rl78_default_config(&c->cfg, RL78_CORE_S3);
    if (c->cfg.core < RL78_CORE_S1 || c->cfg.core > RL78_CORE_AUTO) c->cfg.core = RL78_CORE_S3;
    if (c->cfg.core == RL78_CORE_AUTO) c->cfg.mdu = 1;
    c->skip_bp_pc = NO_BP;
    rl78__rebuild_pages(c);
    rl78_reset(c);
    return c;
}

void rl78_destroy(rl78_cpu *c)
{
    if (!c) return;
    free(c->mem);
    free(c->bplist);
    free(c->bpbits);
    free(c->bporig);
    free(c);
}

const rl78_config *rl78_get_config(const rl78_cpu *c) { return &c->cfg; }
uint8_t *rl78_memory(rl78_cpu *c) { return c->mem; }

void rl78_reset(rl78_cpu *c)
{
    c->pmc = 0;
    c->es = 0x0F;
    c->cs = 0;
    c->sp = 0;
    rl78__set_psw(c, 0x06);
    c->halted = 0;
    c->nirqs = 0;
    c->hold_irq = 0;
    if (c->cfg.mdu) {
        memset(c->mem + MDAL, 0, 8);
        memset(c->mem + MDCL, 0, 4);
        c->mem[MDUC] = 0;
    }
    rl78__rebuild_pages(c);
    c->pc = (uint32_t)(rl78_peek8(c, 0) | (rl78_peek8(c, 1) << 8));
}

/* ---- memory API ---------------------------------------------------------------- */

void rl78_load(rl78_cpu *c, uint32_t addr, const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < len; i++) {
        uint32_t a = (addr + (uint32_t)i) & 0xFFFFFu;
        if (bp_at(c, a)) c->bporig[a] = d[i];
        else c->mem[a] = d[i];
    }
}

uint8_t rl78_peek8(rl78_cpu *c, uint32_t a)
{
    a &= 0xFFFFFu;
    return sys_rd8(c, a);
}

void rl78_peek(rl78_cpu *c, uint32_t addr, void *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) ((uint8_t *)out)[i] = rl78_peek8(c, addr + (uint32_t)i);
}

uint8_t rl78_read8(rl78_cpu *c, uint32_t a) { return rl78__rd8(c, a & 0xFFFFFu); }
uint16_t rl78_read16(rl78_cpu *c, uint32_t a) { return rl78__rd16(c, a & 0xFFFFFu); }

void rl78_write8(rl78_cpu *c, uint32_t a, uint8_t v)
{
    uint8_t *p;
    a &= 0xFFFFFu;
    p = c->wp[a >> 8];
    if (p) p[a & 0xFFu] = v;
    else rl78__wr8_slow(c, a, v);
}

void rl78_write16(rl78_cpu *c, uint32_t a, uint16_t v)
{
    uint8_t *p;
    a &= 0xFFFFEu;
    p = c->wp[a >> 8];
    if (p) { p[a & 0xFFu] = (uint8_t)v; p[(a & 0xFFu) + 1] = (uint8_t)(v >> 8); }
    else rl78__wr16_slow(c, a, v);
}

/* ---- registers ------------------------------------------------------------- */

static uint8_t *bank_ptr(rl78_cpu *c, int bank)
{
    if (c->cfg.core == RL78_CORE_S1) bank = 0;
    return c->mem + 0xFFEF8u - 8u * (unsigned)(bank & 3);
}

uint32_t rl78_get_bank_reg(rl78_cpu *c, int bank, rl78_reg r)
{
    uint8_t *p = bank_ptr(c, bank);
    if (r <= RL78_REG_H) return p[r];
    if (r <= RL78_REG_HL) {
        unsigned k = (unsigned)(r - RL78_REG_AX) * 2u;
        return (uint32_t)(p[k] | (p[k + 1] << 8));
    }
    return 0;
}

void rl78_set_bank_reg(rl78_cpu *c, int bank, rl78_reg r, uint32_t v)
{
    uint8_t *p = bank_ptr(c, bank);
    if (r <= RL78_REG_H) { p[r] = (uint8_t)v; return; }
    if (r <= RL78_REG_HL) {
        unsigned k = (unsigned)(r - RL78_REG_AX) * 2u;
        p[k] = (uint8_t)v;
        p[k + 1] = (uint8_t)(v >> 8);
    }
}

static int cur_bank(const rl78_cpu *c)
{
    return (int)(((c->psw >> 3) & 1u) | ((c->psw >> 4) & 2u));
}

uint32_t rl78_get_reg(rl78_cpu *c, rl78_reg r)
{
    switch (r) {
    case RL78_REG_PC: return c->pc;
    case RL78_REG_SP: return c->sp;
    case RL78_REG_PSW: return c->psw;
    case RL78_REG_ES: return c->es;
    case RL78_REG_CS: return c->cs;
    case RL78_REG_PMC: return c->pmc;
    default: return rl78_get_bank_reg(c, cur_bank(c), r);
    }
}

void rl78_set_reg(rl78_cpu *c, rl78_reg r, uint32_t v)
{
    switch (r) {
    case RL78_REG_PC: c->pc = v & 0xFFFFFu; c->halted = 0; c->hold_irq = 0; break; /* also leaves HALT/STOP */
    case RL78_REG_SP: c->sp = (uint16_t)(v & 0xFFFEu); break;
    case RL78_REG_PSW: rl78__set_psw(c, (uint8_t)v); break;
    case RL78_REG_ES: c->es = (uint8_t)(v & 0xFu); break;
    case RL78_REG_CS: c->cs = (uint8_t)(v & 0xFu); break;
    case RL78_REG_PMC:
        c->pmc = (uint8_t)(c->cfg.core == RL78_CORE_S1 ? 0 : (v & 1u));
        rl78__rebuild_pages(c);
        break;
    default: rl78_set_bank_reg(c, cur_bank(c), r, v); break;
    }
}

/* ---- interrupts ---------------------------------------------------------------- */

int rl78_request_interrupt(rl78_cpu *c, uint16_t vector, int level)
{
    if (c->nirqs >= RL78_MAX_PENDING_IRQ) return -1;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    c->irqs[c->nirqs].vector = vector;
    c->irqs[c->nirqs].level = (uint8_t)level;
    c->irqs[c->nirqs].seq = c->irq_seq++;
    c->nirqs++;
    c->exit_req = 1; /* if called from a hook: re-check after this insn */
    return 0;
}

void rl78_cancel_interrupts(rl78_cpu *c) { c->nirqs = 0; }
int rl78_pending_interrupts(const rl78_cpu *c) { return c->nirqs; }
int rl78_is_halted(const rl78_cpu *c) { return c->halted; }

static void push_pc_psw(rl78_cpu *c, uint32_t ret)
{
    uint32_t sp = c->sp;
    rl78_write8(c, 0xF0000u | ((sp - 1u) & 0xFFFFu), c->psw);
    rl78_write8(c, 0xF0000u | ((sp - 2u) & 0xFFFFu), (uint8_t)(ret >> 16));
    rl78_write8(c, 0xF0000u | ((sp - 3u) & 0xFFFFu), (uint8_t)(ret >> 8));
    rl78_write8(c, 0xF0000u | ((sp - 4u) & 0xFFFFu), (uint8_t)ret);
    c->sp = (uint16_t)((sp - 4u) & 0xFFFFu);
}

static void vector_to(rl78_cpu *c, uint16_t vector)
{
    c->pc = (uint32_t)(rl78_peek8(c, vector) | (rl78_peek8(c, (uint32_t)vector + 1u) << 8));
    c->cycles += c->cfg.irq_clocks;
}

void rl78_force_interrupt(rl78_cpu *c, uint16_t vector)
{
    c->halted = 0;
    push_pc_psw(c, c->pc);
    rl78__set_psw(c, (uint8_t)(c->psw & ~RL78_PSW_IE));
    vector_to(c, vector);
}

/* Try to acknowledge one pending interrupt.  Returns 1 if acknowledged. */
static int try_accept(rl78_cpu *c)
{
    int i, best = -1;
    unsigned isp;
    if (c->nirqs == 0) return 0;
    for (i = 0; i < c->nirqs; i++) {
        if (best < 0 || c->irqs[i].level < c->irqs[best].level ||
            (c->irqs[i].level == c->irqs[best].level && c->irqs[i].seq < c->irqs[best].seq))
            best = i;
    }
    isp = (c->psw >> 1) & 3u;
    if (!(c->psw & RL78_PSW_IE) || c->irqs[best].level > isp) return 0;
    {
        rl78_irq q = c->irqs[best];
        unsigned nisp = q.level ? (unsigned)q.level - 1u : 0u;
        c->irqs[best] = c->irqs[c->nirqs - 1];
        c->nirqs--;
        c->halted = 0;
        push_pc_psw(c, c->pc);
        rl78__set_psw(c, (uint8_t)((c->psw & ~(RL78_PSW_IE | RL78_PSW_ISP0 | RL78_PSW_ISP1)) | (nisp << 1)));
        vector_to(c, q.vector);
    }
    return 1;
}

/* ---- run loop ---------------------------------------------------------------- */

static void fill_fault(rl78_cpu *c)
{
    char dis[64];
    int i;
    c->fault.pc = c->pc;
    for (i = 0; i < 6; i++) c->fault.bytes[i] = rl78_peek8(c, c->pc + (uint32_t)i);
    rl78_disasm(c, c->pc, dis, sizeof dis);
    snprintf(c->fault.text, sizeof c->fault.text,
             "undefined instruction at %05X: %02X %02X %02X %02X (%s)",
             (unsigned)c->pc, c->fault.bytes[0], c->fault.bytes[1], c->fault.bytes[2],
             c->fault.bytes[3], dis);
}

rl78_stop_reason rl78_run(rl78_cpu *c, uint64_t max_insns)
{
    uint64_t remaining = max_insns;
    c->stop_req = 0;
    for (;;) {
        uint64_t lim, n;
        int why;
        if (remaining == 0) return RL78_STOP_LIMIT;
        /* interrupts are acknowledged at an instruction boundary, before the
           next instruction (i.e. at the start of the next rl78_run call when
           the limit was reached exactly) */
        if (!c->hold_irq && c->nirqs) try_accept(c);
        if (c->halted) {
            if (c->nirqs) c->halted = 0; /* unmasked request releases HALT/STOP */
            else return c->halted == 1 ? RL78_STOP_HALT : RL78_STOP_STOPMODE;
        }
        if (c->stop_req) { c->stop_req = 0; return RL78_STOP_REQUEST; }
        lim = remaining;
        if (c->hold_irq && c->nirqs) lim = 1; /* run the instruction after a hold insn */
        c->hold_irq = 0;
        c->skip_bp_pc = (c->nbps && bp_at(c, c->pc)) ? c->pc : NO_BP;
        n = rl78__exec(c, lim, &why);
        c->skip_bp_pc = NO_BP;
        c->insns += n;
        remaining = (n >= remaining) ? 0 : remaining - n;
        if (why == RL78_STOP_UNDEFINED) { fill_fault(c); return RL78_STOP_UNDEFINED; }
        if (why == RL78_STOP_BREAKPOINT) return RL78_STOP_BREAKPOINT;
        if (why == RL78_STOP_HALT || why == RL78_STOP_STOPMODE) {
            if (c->nirqs) continue; /* wakes immediately */
            return (rl78_stop_reason)why;
        }
        if (c->stop_req) { c->stop_req = 0; return RL78_STOP_REQUEST; }
    }
}

rl78_stop_reason rl78_step(rl78_cpu *c) { return rl78_run(c, 1); }

void rl78_request_stop(rl78_cpu *c)
{
    c->stop_req = 1;
    c->exit_req = 1;
}

uint64_t rl78_insn_count(const rl78_cpu *c) { return c->insns; }
uint64_t rl78_clock_count(const rl78_cpu *c) { return c->cycles; }
void rl78_clear_counters(rl78_cpu *c) { c->insns = 0; c->cycles = 0; }
void rl78_add_clocks(rl78_cpu *c, uint64_t k) { c->cycles += k; }
const rl78_fault *rl78_last_fault(const rl78_cpu *c) { return &c->fault; }

/* ---- breakpoints ---------------------------------------------------------------- */

int rl78_add_breakpoint(rl78_cpu *c, uint32_t addr)
{
    addr &= 0xFFFFFu;
    if (!c->bpbits) {
        c->bpbits = (uint8_t *)calloc(RL78_MEM_SIZE / 8u, 1);
        c->bporig = (uint8_t *)malloc(RL78_MEM_SIZE);
        if (!c->bpbits || !c->bporig) {
            free(c->bpbits); free(c->bporig);
            c->bpbits = NULL; c->bporig = NULL;
            return -1;
        }
    }
    if (bp_at(c, addr)) return 0;
    if (c->nbps == c->bpcap) {
        int ncap = c->bpcap ? c->bpcap * 2 : 64;
        uint32_t *nl = (uint32_t *)realloc(c->bplist, sizeof(uint32_t) * (size_t)ncap);
        if (!nl) return -1;
        c->bplist = nl;
        c->bpcap = ncap;
    }
    c->bplist[c->nbps++] = addr;
    c->bporig[addr] = c->mem[addr];
    c->mem[addr] = BP_OPCODE;
    c->bpbits[addr >> 3] = (uint8_t)(c->bpbits[addr >> 3] | (1u << (addr & 7u)));
    if (c->bppage[addr >> 8]++ == 0) rl78__rebuild_pages(c);
    return 0;
}

int rl78_remove_breakpoint(rl78_cpu *c, uint32_t addr)
{
    int i;
    addr &= 0xFFFFFu;
    if (!bp_at(c, addr)) return -1;
    for (i = 0; i < c->nbps; i++)
        if (c->bplist[i] == addr) break;
    c->bplist[i] = c->bplist[--c->nbps];
    c->mem[addr] = c->bporig[addr];
    c->bpbits[addr >> 3] = (uint8_t)(c->bpbits[addr >> 3] & ~(1u << (addr & 7u)));
    if (--c->bppage[addr >> 8] == 0) rl78__rebuild_pages(c);
    return 0;
}

void rl78_clear_breakpoints(rl78_cpu *c)
{
    int i;
    for (i = 0; i < c->nbps; i++) {
        uint32_t a = c->bplist[i];
        c->mem[a] = c->bporig[a];
        c->bpbits[a >> 3] = 0;
    }
    c->nbps = 0;
    memset(c->bppage, 0, sizeof c->bppage);
    rl78__rebuild_pages(c);
}

int rl78_breakpoint_count(const rl78_cpu *c) { return c->nbps; }

/* ---- hooks ------------------------------------------------------------------ */

int rl78_add_hook(rl78_cpu *c, uint32_t start, uint32_t end, rl78_read_hook rd,
                  rl78_write_hook wr, void *ctx)
{
    if (c->nhooks >= RL78_MAX_HOOKS || end <= start) return -1;
    c->hooks[c->nhooks].start = start;
    c->hooks[c->nhooks].end = end;
    c->hooks[c->nhooks].rd = rd;
    c->hooks[c->nhooks].wr = wr;
    c->hooks[c->nhooks].ctx = ctx;
    c->nhooks++;
    rl78__rebuild_pages(c);
    return 0;
}

void rl78_clear_hooks(rl78_cpu *c)
{
    c->nhooks = 0;
    rl78__rebuild_pages(c);
}

/* ---- snapshots ------------------------------------------------------------- */

struct rl78_snapshot {
    uint8_t *mem;
    uint32_t pc;
    uint16_t sp;
    uint8_t psw, es, cs, pmc, halted;
    uint64_t insns, cycles;
    rl78_irq irqs[RL78_MAX_PENDING_IRQ];
    int nirqs;
    int hold_irq;
};

void rl78_snapshot_update(rl78_cpu *c, rl78_snapshot *s)
{
    int i;
    memcpy(s->mem, c->mem, RL78_MEM_SIZE);
    for (i = 0; i < c->nbps; i++) s->mem[c->bplist[i]] = c->bporig[c->bplist[i]];
    s->pc = c->pc;
    s->sp = c->sp;
    s->psw = c->psw;
    s->es = c->es;
    s->cs = c->cs;
    s->pmc = c->pmc;
    s->halted = c->halted;
    s->insns = c->insns;
    s->cycles = c->cycles;
    memcpy(s->irqs, c->irqs, sizeof s->irqs);
    s->nirqs = c->nirqs;
    s->hold_irq = c->hold_irq;
}

rl78_snapshot *rl78_snapshot_create(rl78_cpu *c)
{
    rl78_snapshot *s = (rl78_snapshot *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->mem = (uint8_t *)malloc(RL78_MEM_SIZE);
    if (!s->mem) { free(s); return NULL; }
    rl78_snapshot_update(c, s);
    return s;
}

void rl78_snapshot_restore(rl78_cpu *c, const rl78_snapshot *s)
{
    int i;
    memcpy(c->mem, s->mem, RL78_MEM_SIZE);
    for (i = 0; i < c->nbps; i++) {
        c->bporig[c->bplist[i]] = c->mem[c->bplist[i]];
        c->mem[c->bplist[i]] = BP_OPCODE;
    }
    c->pc = s->pc;
    c->sp = s->sp;
    c->es = s->es;
    c->cs = s->cs;
    c->pmc = s->pmc;
    c->halted = s->halted;
    c->insns = s->insns;
    c->cycles = s->cycles;
    memcpy(c->irqs, s->irqs, sizeof c->irqs);
    c->nirqs = s->nirqs;
    c->hold_irq = s->hold_irq;
    rl78__set_psw(c, s->psw);
    rl78__rebuild_pages(c);
}

void rl78_snapshot_free(rl78_snapshot *s)
{
    if (!s) return;
    free(s->mem);
    free(s);
}

const char *rl78_stop_reason_name(rl78_stop_reason r)
{
    switch (r) {
    case RL78_STOP_LIMIT: return "limit";
    case RL78_STOP_BREAKPOINT: return "breakpoint";
    case RL78_STOP_HALT: return "halt";
    case RL78_STOP_STOPMODE: return "stop";
    case RL78_STOP_UNDEFINED: return "undefined";
    case RL78_STOP_REQUEST: return "request";
    default: return "?";
    }
}
