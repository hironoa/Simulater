/*
 * rl78_xlate_rt.h - runtime of translated code.
 *
 * rl78_xlate.c turns RL78 machine code into C: one call of rl78_xl_exec1() per
 * instruction, with the address and the instruction bytes as constants.
 * rl78_xl_exec1() is the opcode switch of the interpreter (rl78_exec_ops.inc),
 * so translated code and the interpreter cannot drift apart; the C compiler
 * folds the switch down to the one handler and optimises across instructions.
 *
 * Compiled freestanding (no C library): only <stdint.h>/<stddef.h>.
 * clang only: gcc does not prune the switch while inlining and never finishes.
 */
#ifndef RL78_XLATE_RT_H
#define RL78_XLATE_RT_H

#include "rl78_exec_defs.h"

#define RL78_XL_ABI 1u

/* Return codes of a translated function (chunk) and of rl78_xl_exec1(). */
#define RL78_XL_CONT 0   /* left the chunk: look the new pc up again */
#define RL78_XL_MISS 98  /* cannot go on here (near the limit, breakpoint, unknown pc): let the interpreter step */
#define RL78_XL_STOP 100 /* RL78_XL_STOP + why: stop request of the instruction (why = -1: just re-check) */

typedef int (*rl78_xl_fn)(rl78_cpu *C, uint64_t *np, uint64_t lim);

typedef struct { uint32_t pc; rl78_xl_fn fn; } rl78_xl_leader;

/* The slow-path functions of the simulator, for translated code in a DLL. */
struct rl78_xl_host {
    uint8_t (*rd8_slow)(rl78_cpu *c, uint32_t a);
    void (*wr8_slow)(rl78_cpu *c, uint32_t a, uint8_t v);
    uint16_t (*rd16_slow)(rl78_cpu *c, uint32_t a);
    void (*wr16_slow)(rl78_cpu *c, uint32_t a, uint16_t v);
    void (*write_macr)(rl78_cpu *c, uint32_t v);
};

struct rl78_xl_module {
    uint32_t abi;        /* RL78_XL_ABI */
    uint32_t cpu_size;   /* sizeof(rl78_cpu): layout check */
    uint32_t core_s1, core_s3; /* what the code was translated for */
    uint32_t nleaders;
    const rl78_xl_leader *leaders;  /* block starts, sorted by pc */
    uint32_t ninside;
    const uint32_t *inside;         /* translated instructions that are not block starts */
};

#ifdef RL78_XL_UNIT
/* ---- one translated instruction ------------------------------------------- */

#undef B1
#undef B2
#undef B3
#undef W1
#undef W2
/* instruction bytes are constants (pc moves by one after an ES: prefix) */
#define B1 ((unsigned)ib[pc - pc0 + 1u])
#define B2 ((unsigned)ib[pc - pc0 + 2u])
#define B3 ((unsigned)ib[pc - pc0 + 3u])
#define W1 (B1 | (B2 << 8))
#define W2 (B2 | (B3 << 8))
/* the skip instructions ask for the length of the next instruction */
#define rl78__insn_len(C_, a_) ((int)nextlen_)
#define RL78_XL_NOLIM (~(uint64_t)0)

static inline __attribute__((always_inline)) int rl78_xl_exec1(
    rl78_cpu *C, uint8_t *const M, uint8_t **Rp, unsigned *Fp, uint64_t *cycp, uint32_t *pcp, uint64_t *np,
    const uint32_t pc0, const unsigned i0, const unsigned i1, const unsigned i2, const unsigned i3,
    const unsigned i4, const unsigned i5, const int nextlen_)
{
    const uint8_t ib[8] = {(uint8_t)i0, (uint8_t)i1, (uint8_t)i2, (uint8_t)i3, (uint8_t)i4, (uint8_t)i5, 0, 0};
    uint8_t *R = *Rp;
    unsigned F = *Fp;
    uint64_t cyc = *cycp;
    uint32_t pc = pc0, ipc = pc0;
    uint64_t n = *np + 1, lim = RL78_XL_NOLIM;
    int why_ = -1, *why = &why_;
    unsigned op = i0;
    uint32_t segv = 0xF0000u;
    const int s1 = RL78_XL_S1, s3 = RL78_XL_S3;
    int st;
    (void)s3;
    (void)segv;
#define seg 0xF0000u
#include "rl78_exec_ops.inc"
#undef seg
    goto done;
dispatch: /* general copy: after an ES: prefix */
#define seg segv
#include "rl78_exec_ops.inc"
#undef seg
    goto done;
undefined:
    pc = ipc;
    n--;
    why_ = RL78_STOP_UNDEFINED;
    lim = n;
done:
    st = (lim != RL78_XL_NOLIM) ? RL78_XL_STOP + 1 + why_ : 0; /* why_ is -1 or a stop reason */
    *Rp = R;
    *Fp = F;
    *cycp = cyc;
    *pcp = pc;
    *np = n;
    return st;
}
#endif /* RL78_XL_UNIT */

#endif
