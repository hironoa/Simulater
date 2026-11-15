/*
 * rl78_exec.c - RL78 instruction interpreter (hot loop).
 *
 * Opcode maps and semantics follow R01US0015 Rev.2.40, tables 5-5..5-12 and
 * section 6.  Clock counts follow tables 5-5 (S1), 5-6 (S2), 5-7 (S3):
 *   - "Note 1" clocks are counted by default,
 *   - "Note 2" clocks (data read from flash) are counted when the data address
 *     lies in a flash page (rl78_cpu.flashpen),
 *   - an ES: prefix adds one clock (uniform across the tables),
 *   - pipeline hazards (7.2.3) and RAM-fetch stalls (7.2.2) are not modelled.
 */
#include "rl78_exec_defs.h"

static int insn_len_bytes(const uint8_t *p);

int rl78__insn_len(rl78_cpu *c, uint32_t pc)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = rl78__rd8(c, (pc + (uint32_t)i) & 0xFFFFFu);
    /* The opcode trap mask is never applied to instruction-length reads. */
    return insn_len_bytes(b);
}

/* Instruction length from the opcode maps (used by the skip instructions). */
static int insn_len_bytes(const uint8_t *p)
{
    unsigned op = p[0];
    int pre = 0;
    if (op == 0x11) {
        pre = 1;
        p++;
        op = p[0];
    }
    switch (op) {
    case 0x61: {
        unsigned o = p[1];
        unsigned lo = o & 0xF, hi = o >> 4;
        if (hi <= 7) return pre + ((lo == 9) ? 3 : 2); /* ADDW AX,[HL+byte] etc */
        if (o == 0x89 || o == 0xA8 || o == 0xAB || o == 0xAD || o == 0xAF ||
            o == 0xB8 || o == 0xCE || o == 0xDE || o == 0xC3 || o == 0xD3)
            return pre + 3;
        if (o == 0xAA) return pre + 4;
        return pre + 2;
    }
    case 0x71: {
        unsigned o = p[1];
        unsigned lo = o & 0xF;
        if (o >= 0x80) {
            if (lo == 0 || lo == 8) return pre + 2;
            if (lo >= 1 && lo <= 7) return pre + 2;
            return pre + 2;
        }
        if (lo == 0 || lo == 8) return pre + 4;
        return pre + 3;
    }
    case 0x31: {
        unsigned o = p[1];
        unsigned lo = o & 0xF;
        if (lo <= 5) {
            if (o >= 0x80) return pre + ((lo & 1) ? 3 : 4);
            return pre + ((lo & 1) ? 3 : 4);
        }
        return pre + 2;
    }
    default: break;
    }
    {
        static const uint8_t len1[256] = {
            /*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
            /* 0 */ 1, 1, 3, 1, 3, 1, 2, 1, 1, 3, 3, 2, 2, 1, 2, 3,
            /* 1 */ 2, 1, 1, 1, 1, 1, 1, 1, 3, 4, 3, 2, 2, 1, 2, 3,
            /* 2 */ 2, 1, 3, 1, 3, 1, 2, 1, 3, 3, 3, 2, 2, 1, 2, 3,
            /* 3 */ 3, 2, 3, 1, 3, 1, 3, 1, 4, 4, 3, 2, 2, 1, 2, 3,
            /* 4 */ 4, 2, 3, 1, 3, 1, 2, 1, 3, 3, 3, 2, 2, 1, 2, 3,
            /* 5 */ 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 2, 2, 1, 2, 3,
            /* 6 */ 1, 2, 1, 1, 1, 1, 1, 1, 3, 3, 3, 2, 2, 1, 2, 3,
            /* 7 */ 1, 2, 1, 1, 1, 1, 1, 1, 3, 3, 3, 2, 2, 1, 2, 3,
            /* 8 */ 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 1, 2, 2, 2, 3,
            /* 9 */ 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 1, 2, 2, 2, 3,
            /* A */ 3, 1, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 2, 2, 3,
            /* B */ 3, 1, 3, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 2, 2, 3,
            /* C */ 1, 1, 1, 1, 1, 1, 1, 1, 3, 4, 3, 4, 3, 3, 3, 4,
            /* D */ 1, 1, 1, 1, 2, 3, 1, 1, 2, 3, 2, 3, 2, 2, 2, 2,
            /* E */ 1, 1, 1, 1, 2, 3, 1, 1, 2, 3, 2, 3, 4, 3, 3, 2,
            /* F */ 1, 1, 1, 1, 2, 3, 1, 1, 2, 3, 2, 3, 4, 3, 3, 1,
        };
        return pre + len1[op];
    }
}


/* Execute up to lim instructions.  Returns the number executed; *why is set
   to a stop reason, or -1 if it simply ran out / was asked to re-check. */
uint64_t rl78__exec(rl78_cpu *C, uint64_t lim, int *why);

uint64_t rl78__exec(rl78_cpu *C, uint64_t lim, int *why)
{
    uint8_t *const M = C->mem;
    uint8_t *R = C->rb;
    uint32_t pc = C->pc;
    uint32_t ipc = pc;
    uint64_t cyc = C->cycles;
    uint64_t n = 0;
    unsigned F = C->psw;
    const int s1 = (C->cfg.core == RL78_CORE_S1);
    const int s3 = (C->cfg.core == RL78_CORE_S3 || C->cfg.core == RL78_CORE_AUTO);
    uint32_t segv = 0xF0000u; /* segment of the general copy (see rl78_exec_ops.inc) */
    unsigned op;

    *why = -1;
    C->exit_req = 0;
    C->hold_req = 0;
    C->hold_n = 0;

    while (n < lim) {
        n++;
        ipc = pc;
        /* Only the first opcode sees traps; operands/data use unchanged M.
           No breakpoint branch in either the fetch or data-read fast path. */
        op = M[pc] | M[pc + RL78_TRAP_OFFSET];
#define seg 0xF0000u
#include "rl78_exec_ops.inc"
#undef seg
        continue;

    dispatch: /* general copy: op and segv are set by the fast copy */
#define seg segv
#include "rl78_exec_ops.inc"
#undef seg
        continue;

    undefined:
        pc = ipc;
        n--;
        *why = RL78_STOP_UNDEFINED;
        break;
    }

    C->pc = pc;
    C->psw = (uint8_t)F;
    C->cycles = cyc;
    C->rb = R;
    C->hold_irq = (n > 0 && C->hold_n == n);
    return n;
}
