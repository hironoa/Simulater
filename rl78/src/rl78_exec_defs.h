/*
 * rl78_exec_defs.h - flag arithmetic and the bus/register macros used by the opcode switch
 * (rl78_exec_ops.inc).  Shared by the interpreter (rl78_exec.c) and by translated code
 * (rl78_xlate_rt.h).  Needs only <stdint.h>/<stddef.h>: translated code is compiled
 * freestanding, without a C library.
 */
#ifndef RL78_EXEC_DEFS_H
#define RL78_EXEC_DEFS_H

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

/* Record the target of a CALL (rl78_set_call_profile): one bit per address.
   The translator uses it to pick the functions worth translating. */
#define CALLPROF(t)                                                              \
    do {                                                                         \
        if (UNLIKELY(C->callprof != NULL)) C->callprof[(t) >> 3] |= (uint8_t)(1u << ((t) & 7u)); \
    } while (0)

/* ES: prefix validity (instructions that have an ES: form in table 5-8) */
static inline int es_ok(unsigned op, unsigned op2)
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

#endif
