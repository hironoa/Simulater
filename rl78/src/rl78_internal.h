/* rl78_internal.h - private definitions shared by the simulator sources. */
#ifndef RL78_INTERNAL_H
#define RL78_INTERNAL_H

#include "rl78.h"

#define RL78_MEM_SIZE 0x100000u
#define RL78_MEM_PAD 16u /* operand fetch past FFFFFH stays in bounds */
#define RL78_NPAGES 4096u /* 256-byte pages */
#define RL78_TRAP_OFFSET (RL78_MEM_SIZE + RL78_MEM_PAD) /* bptrap = mem + this */

#define F_CY 0x01u
#define F_ISP0 0x02u
#define F_ISP1 0x04u
#define F_RBS0 0x08u
#define F_AC 0x10u
#define F_RBS1 0x20u
#define F_Z 0x40u
#define F_IE 0x80u

#define BP_OPCODE 0xFFu /* undefined in the 1st map; used as a breakpoint trap */

typedef struct {
    uint32_t start, end;
    rl78_read_hook rd;
    rl78_write_hook wr;
    void *ctx;
} rl78_hook;

typedef struct {
    uint32_t addr;
    uint8_t orig;
} rl78_bp;

typedef struct {
    uint16_t vector;
    uint8_t level;
    uint32_t seq;
} rl78_irq;

struct rl78_cpu {
    /* page tables for the data bus: NULL means "slow path" */
    const uint8_t *rp[RL78_NPAGES];
    uint8_t *wp[RL78_NPAGES];
    uint8_t flashpen[RL78_NPAGES]; /* extra clocks for data read from flash */

    uint8_t *mem;
    uint8_t *bptrap; /* opcode-only mask: FF at breakpoints, zero elsewhere (mem + RL78_TRAP_OFFSET) */
    uint8_t *rb; /* current register bank: mem + FFEF8H - 8*bank */

    uint32_t pc;
    uint16_t sp;
    uint8_t psw, es, cs, pmc;
    uint8_t halted; /* 0 run, 1 HALT, 2 STOP */

    uint64_t insns, cycles;

    rl78_config cfg;

    int exit_req;   /* set by slow paths: leave the inner loop after this insn */
    int stop_req;   /* rl78_request_stop */
    int hold_irq;   /* last insn was an interrupt-request-hold instruction */
    int hold_req;   /* slow path wrote PSW: treat current insn as hold insn */
    uint64_t hold_n;
    uint32_t skip_bp_pc; /* execute the original insn at this bp once */

    rl78_hook hooks[RL78_MAX_HOOKS];
    int nhooks;
    /* Breakpoints: address list and bitmap allocated on first use.
       bptrap is separate from the program-visible backing store. */
    uint32_t *bplist;
    int nbps, bpcap;
    uint8_t *bpbits;   /* 1 bit per address */
    rl78_irq irqs[RL78_MAX_PENDING_IRQ];
    int nirqs;
    uint32_t irq_seq;

    uint8_t sink[256]; /* writes to read-only pages land here */

    uint8_t *callprof;  /* call-target bitmap (1 bit per address), or NULL */

    /* translated code (rl78_xlate.c): block start -> leader number + 1 */
    uint32_t *xl_index;
    uint8_t *xl_inside;  /* bitmap: instructions inside a translated block of some chunk (not its start) */
    const struct rl78_xl_module *xl_mod;
    uint64_t xl_insns;  /* instructions executed by translated code */
    int xl_bp_inside;   /* breakpoints set inside translated blocks: translated code is not used while > 0 */
    unsigned xl_miss;   /* consecutive lookups that found no translated code (interpreter burst length) */
    rl78_fault fault;
};

/* slow-path bus access (hooks, SFRs, mirror edges) */
uint8_t rl78__rd8_slow(rl78_cpu *c, uint32_t a);
void rl78__wr8_slow(rl78_cpu *c, uint32_t a, uint8_t v);
uint16_t rl78__rd16_slow(rl78_cpu *c, uint32_t a);
void rl78__wr16_slow(rl78_cpu *c, uint32_t a, uint16_t v);
void rl78__rebuild_pages(rl78_cpu *c);
void rl78__set_psw(rl78_cpu *c, uint8_t v);
int rl78__insn_len(rl78_cpu *c, uint32_t pc);
void rl78__write_macr(rl78_cpu *c, uint32_t v);

/* translated code (rl78_xlate.c) */
uint64_t rl78__exec(rl78_cpu *C, uint64_t lim, int *why);
uint64_t rl78__xl_run(rl78_cpu *c, uint64_t lim, int *why);
void rl78__xl_breakpoint_changed(rl78_cpu *c, uint32_t addr, int added);

static inline uint8_t rl78__rd8(rl78_cpu *c, uint32_t a)
{
    const uint8_t *p = c->rp[a >> 8];
    if (p) return p[a & 0xFFu];
    return rl78__rd8_slow(c, a);
}

static inline uint16_t rl78__rd16(rl78_cpu *c, uint32_t a)
{
    const uint8_t *p;
    a &= 0xFFFFEu;
    p = c->rp[a >> 8];
    if (p) return (uint16_t)(p[a & 0xFFu] | (p[(a & 0xFFu) + 1u] << 8));
    return rl78__rd16_slow(c, a);
}

#endif
