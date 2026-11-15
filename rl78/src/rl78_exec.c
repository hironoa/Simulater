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
#include <string.h>

#include "rl78_internal.h"

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

enum { OP_ADD, OP_ADDC, OP_SUB, OP_SUBC, OP_CMP, OP_AND, OP_OR, OP_XOR };

/* 8-bit ALU; returns the result, updates flags in *f. */
static inline unsigned alu8(unsigned op, unsigned a, unsigned b, unsigned *f)
{
    unsigned r;
    switch (op) {
    case OP_ADD: r = a + b; break;
    case OP_ADDC: r = a + b + (*f & F_CY); break;
    case OP_SUB:
    case OP_CMP: r = a - b; break;
    case OP_SUBC: r = a - b - (*f & F_CY); break;
    case OP_AND:
        r = a & b;
        *f = (*f & ~F_Z) | (r ? 0u : F_Z);
        return r;
    case OP_OR:
        r = a | b;
        *f = (*f & ~F_Z) | (r ? 0u : F_Z);
        return r;
    default:
        r = a ^ b;
        *f = (*f & ~F_Z) | (r ? 0u : F_Z);
        return r;
    }
    *f = (*f & ~(F_Z | F_AC | F_CY)) | ((r >> 8) & 1u) | ((a ^ b ^ r) & 0x10u) |
         ((r & 0xFFu) ? 0u : F_Z);
    return r & 0xFFu;
}

/* 16-bit add/sub (ADDW/SUBW/CMPW).  AC is "undefined" in the manual; we set
   it to the carry/borrow from bit 3 to bit 4 (same rule as 8-bit). */
static inline unsigned addw(unsigned a, unsigned b, unsigned *f)
{
    unsigned r = a + b;
    *f = (*f & ~(F_Z | F_AC | F_CY)) | ((r >> 16) & 1u) | ((a ^ b ^ r) & 0x10u) |
         ((r & 0xFFFFu) ? 0u : F_Z);
    return r & 0xFFFFu;
}
static inline unsigned subw(unsigned a, unsigned b, unsigned *f)
{
    unsigned r = a - b;
    *f = (*f & ~(F_Z | F_AC | F_CY)) | ((r >> 16) & 1u) | ((a ^ b ^ r) & 0x10u) |
         ((r & 0xFFFFu) ? 0u : F_Z);
    return r & 0xFFFFu;
}
static inline unsigned inc8(unsigned a, unsigned *f)
{
    unsigned r = (a + 1u) & 0xFFu;
    *f = (*f & ~(F_Z | F_AC)) | ((a & 0xFu) == 0xFu ? F_AC : 0u) | (r ? 0u : F_Z);
    return r;
}
/* DEC: the manual text says "carry from bit 3 to bit 4"; for a decrement we
   interpret AC as the borrow from bit 4 to bit 3, i.e. low nibble was 0. */
static inline unsigned dec8(unsigned a, unsigned *f)
{
    unsigned r = (a - 1u) & 0xFFu;
    *f = (*f & ~(F_Z | F_AC)) | ((a & 0xFu) == 0u ? F_AC : 0u) | (r ? 0u : F_Z);
    return r;
}

#define G16(p) ((unsigned)((p)[0] | ((unsigned)(p)[1] << 8)))
#define S16(p, v) ((p)[0] = (uint8_t)(v), (p)[1] = (uint8_t)((unsigned)(v) >> 8))

/* register file (current bank) */
#define rX R[0]
#define rA R[1]
#define rC R[2]
#define rB R[3]
#define rE R[4]
#define rD R[5]
#define rL R[6]
#define rH R[7]
#define AX G16(R + 0)
#define BC G16(R + 2)
#define DE G16(R + 4)
#define HL G16(R + 6)

#define B1 ((unsigned)M[pc + 1])
#define B2 ((unsigned)M[pc + 2])
#define B3 ((unsigned)M[pc + 3])
#define W1 ((unsigned)M[pc + 1] | ((unsigned)M[pc + 2] << 8))
#define W2 ((unsigned)M[pc + 2] | ((unsigned)M[pc + 3] << 8))
#define SADDR(s) (0xFFE00u + (s) + ((s) < 0x20u ? 0x100u : 0u))
#define SFRA(s) (0xFFF00u + (s))
#define ADDR16(w) (seg | ((w) & 0xFFFFu))
#define CK(n) (cyc += (n))
#define CKS1(n23, n1) (cyc += s1 ? (unsigned)(n1) : (unsigned)(n23))

#define SYNC_OUT() (C->psw = (uint8_t)F, C->pc = ipc, C->cycles = cyc)
#define SYNC_IN()                                                                \
    do {                                                                         \
        F = C->psw;                                                              \
        R = C->rb;                                                               \
        cyc = C->cycles;                                                         \
        if (C->exit_req) {                                                       \
            lim = n;                                                             \
            if (C->hold_req) { C->hold_n = n; C->hold_req = 0; }                 \
        }                                                                        \
    } while (0)

#define RD8(dst, addr)                                                           \
    do {                                                                         \
        uint32_t a_ = (addr);                                                    \
        const uint8_t *p_ = C->rp[a_ >> 8];                                      \
        if (LIKELY(p_ != NULL)) {                                                \
            (dst) = p_[a_ & 0xFFu];                                              \
        } else {                                                                 \
            SYNC_OUT();                                                          \
            (dst) = rl78__rd8_slow(C, a_);                                       \
            SYNC_IN();                                                           \
        }                                                                        \
    } while (0)
/* read with flash-access clock penalty (Note 2 of the operation list) */
#define RD8F(dst, addr)                                                          \
    do {                                                                         \
        uint32_t af_ = (addr);                                                   \
        cyc += C->flashpen[af_ >> 8];                                            \
        RD8(dst, af_);                                                           \
    } while (0)
#define WR8(addr, val)                                                           \
    do {                                                                         \
        uint32_t a_ = (addr);                                                    \
        uint8_t *p_ = C->wp[a_ >> 8];                                            \
        if (LIKELY(p_ != NULL)) {                                                \
            p_[a_ & 0xFFu] = (uint8_t)(val);                                     \
        } else {                                                                 \
            SYNC_OUT();                                                          \
            rl78__wr8_slow(C, a_, (uint8_t)(val));                               \
            SYNC_IN();                                                           \
        }                                                                        \
    } while (0)
#define RD16(dst, addr)                                                          \
    do {                                                                         \
        uint32_t a_ = (addr) & 0xFFFFEu;                                         \
        const uint8_t *p_ = C->rp[a_ >> 8];                                      \
        if (LIKELY(p_ != NULL)) {                                                \
            (dst) = G16(p_ + (a_ & 0xFFu));                                      \
        } else {                                                                 \
            SYNC_OUT();                                                          \
            (dst) = rl78__rd16_slow(C, a_);                                      \
            SYNC_IN();                                                           \
        }                                                                        \
    } while (0)
#define RD16F(dst, addr)                                                         \
    do {                                                                         \
        uint32_t af_ = (addr);                                                   \
        cyc += C->flashpen[af_ >> 8];                                            \
        RD16(dst, af_);                                                          \
    } while (0)
#define WR16(addr, val)                                                          \
    do {                                                                         \
        uint32_t a_ = (addr) & 0xFFFFEu;                                         \
        uint8_t *p_ = C->wp[a_ >> 8];                                            \
        if (LIKELY(p_ != NULL)) {                                                \
            S16(p_ + (a_ & 0xFFu), (val));                                       \
        } else {                                                                 \
            SYNC_OUT();                                                          \
            rl78__wr16_slow(C, a_, (uint16_t)(val));                             \
            SYNC_IN();                                                           \
        }                                                                        \
    } while (0)

/* PSW changed by an instruction (not via the bus): update bank pointer. */
#define PSW_CHANGED()                                                            \
    do {                                                                         \
        if (s1) F &= ~(F_RBS0 | F_RBS1);                                         \
        R = M + 0xFFEF8u - 8u * (((F >> 3) & 1u) | ((F >> 4) & 2u));             \
        C->rb = R;                                                               \
    } while (0)
/* interrupt request hold instruction: interrupts are not acknowledged
   before the next instruction completes */
#define HOLD()                                                                   \
    do {                                                                         \
        C->hold_n = n;                                                           \
        if (C->nirqs) lim = n;                                                   \
    } while (0)

#define PUSH8(v)                                                                 \
    do {                                                                         \
        sp = (sp - 1u) & 0xFFFFu;                                                \
        WR8(0xF0000u | sp, (v));                                                 \
    } while (0)

#define UNDEF() goto undefined

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

/* ES: prefix validity (instructions that have an ES: form in table 5-8) */
static int es_ok(unsigned op, unsigned op2)
{
    unsigned lo = op & 0xF, hi = op >> 4;
    switch (op) {
    case 0x61: {
        unsigned l2 = op2 & 0xF, h2 = op2 >> 4;
        if (h2 >= 8 && (l2 == 0 || l2 == 2)) return 1; /* ALU A,[HL+B]/[HL+C] */
        switch (op2) {
        case 0xC9: case 0xD9: case 0xE9: case 0xF9:                       /* MOV [HL+B/C] */
        case 0xAA: case 0xAC: case 0xAD: case 0xAE: case 0xAF: case 0xB9: case 0xA9: /* XCH */
        case 0x09: case 0x29: case 0x49:                                  /* ADDW.. [HL+byte] */
        case 0x59: case 0x69: case 0x79: case 0x89:                       /* INC.. [HL+byte] */
        case 0xCE: case 0xDE:                                             /* MOVS, CMPS */
            return 1;
        default: return 0;
        }
    }
    case 0x71: {
        unsigned l2 = op2 & 0xF;
        if (op2 < 0x80) return l2 == 0 || l2 == 8;
        return l2 >= 1 && l2 <= 7;
    }
    case 0x31: {
        unsigned l2 = op2 & 0xF;
        return op2 >= 0x80 && (l2 == 1 || l2 == 3 || l2 == 5);
    }
    default: break;
    }
    if (hi <= 7 && (lo == 0xD || lo == 0xE || lo == 0xF)) return 1; /* ALU A,[HL]/[HL+b]/!a */
    switch (op) {
    case 0x02: case 0x22: case 0x42: case 0x40:
    case 0x09: case 0x18: case 0x19: case 0x28: case 0x29: case 0x38: case 0x39:
    case 0x48: case 0x49: case 0x58: case 0x59: case 0x68: case 0x69: case 0x78: case 0x79:
    case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8F:
    case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9F:
    case 0xA0: case 0xA2: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAF:
    case 0xB0: case 0xB2: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBF:
    case 0xCA: case 0xCC: case 0xCF:
    case 0xD5: case 0xD9: case 0xDB: case 0xE5: case 0xE9: case 0xEB: case 0xF5: case 0xF9: case 0xFB:
        return 1;
    default: return 0;
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
