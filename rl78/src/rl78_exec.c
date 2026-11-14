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
    /* code bytes: use raw memory, but hide breakpoints (rd8 does that only
       for pages marked slow; breakpoint pages always are) */
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
    uint32_t seg;
    unsigned op;

    *why = -1;
    C->exit_req = 0;
    C->hold_req = 0;
    C->hold_n = 0;

    while (n < lim) {
        n++;
        ipc = pc;
        seg = 0xF0000u;
        op = M[pc];
    dispatch:
        switch (op) {
        /* ---------------------------------------------------------------- */
        case 0x00: /* NOP */
            CK(1);
            pc += 1;
            break;

        /* ---- 16-bit operations ---- */
        case 0x01: { unsigned r = addw(AX, AX, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x03: { unsigned r = addw(AX, BC, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x05: { unsigned r = addw(AX, DE, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x07: { unsigned r = addw(AX, HL, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x02: { unsigned v; RD16F(v, ADDR16(W1)); { unsigned r = addw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 3; break; }
        case 0x04: { unsigned r = addw(AX, W1, &F); S16(R, r); CKS1(1, 2); pc += 3; break; }
        case 0x06: { unsigned v; RD16(v, SADDR(B1)); { unsigned r = addw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 2; break; }
        case 0x23: { unsigned r = subw(AX, BC, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x25: { unsigned r = subw(AX, DE, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x27: { unsigned r = subw(AX, HL, &F); S16(R, r); CKS1(1, 2); pc += 1; break; }
        case 0x22: { unsigned v; RD16F(v, ADDR16(W1)); { unsigned r = subw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 3; break; }
        case 0x24: { unsigned r = subw(AX, W1, &F); S16(R, r); CKS1(1, 2); pc += 3; break; }
        case 0x26: { unsigned v; RD16(v, SADDR(B1)); { unsigned r = subw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 2; break; }
        case 0x43: subw(AX, BC, &F); CKS1(1, 2); pc += 1; break;
        case 0x45: subw(AX, DE, &F); CKS1(1, 2); pc += 1; break;
        case 0x47: subw(AX, HL, &F); CKS1(1, 2); pc += 1; break;
        case 0x42: { unsigned v; RD16F(v, ADDR16(W1)); subw(AX, v, &F); CKS1(1, 2); pc += 3; break; }
        case 0x44: subw(AX, W1, &F); CKS1(1, 2); pc += 3; break;
        case 0x46: { unsigned v; RD16(v, SADDR(B1)); subw(AX, v, &F); CKS1(1, 2); pc += 2; break; }

        case 0x10: /* ADDW SP,#byte */
            C->sp = (uint16_t)((C->sp + B1) & 0xFFFEu);
            CKS1(1, 2);
            pc += 2;
            break;
        case 0x20: /* SUBW SP,#byte */
            C->sp = (uint16_t)((C->sp - B1) & 0xFFFEu);
            CKS1(1, 2);
            pc += 2;
            break;

        case 0x08: { uint8_t t = rA; rA = rX; rX = t; CK(1); pc += 1; break; } /* XCH A,X */

        /* ---- MOVW rp <-> AX ---- */
        case 0x12: R[2] = R[0]; R[3] = R[1]; CKS1(1, 2); pc += 1; break;
        case 0x14: R[4] = R[0]; R[5] = R[1]; CKS1(1, 2); pc += 1; break;
        case 0x16: R[6] = R[0]; R[7] = R[1]; CKS1(1, 2); pc += 1; break;
        case 0x13: R[0] = R[2]; R[1] = R[3]; CKS1(1, 2); pc += 1; break;
        case 0x15: R[0] = R[4]; R[1] = R[5]; CKS1(1, 2); pc += 1; break;
        case 0x17: R[0] = R[6]; R[1] = R[7]; CKS1(1, 2); pc += 1; break;
        case 0x30: R[0] = M[pc + 1]; R[1] = M[pc + 2]; CKS1(1, 2); pc += 3; break;
        case 0x32: R[2] = M[pc + 1]; R[3] = M[pc + 2]; CKS1(1, 2); pc += 3; break;
        case 0x34: R[4] = M[pc + 1]; R[5] = M[pc + 2]; CKS1(1, 2); pc += 3; break;
        case 0x36: R[6] = M[pc + 1]; R[7] = M[pc + 2]; CKS1(1, 2); pc += 3; break;
        case 0x33: case 0x35: case 0x37: { /* XCHW AX,rp */
            unsigned k = (op - 0x31u);
            uint8_t t0 = R[0], t1 = R[1];
            R[0] = R[k]; R[1] = R[k + 1]; R[k] = t0; R[k + 1] = t1;
            CKS1(1, 2);
            pc += 1;
            break;
        }

        /* ---- 8-bit transfers with word[B]/word[C]/word[BC] ---- */
        case 0x09: { unsigned v; RD8F(v, ADDR16(W1 + rB)); rA = (uint8_t)v; CK(1); pc += 3; break; }
        case 0x18: WR8(ADDR16(W1 + rB), rA); CK(1); pc += 3; break;
        case 0x19: WR8(ADDR16(W1 + rB), B3); CK(1); pc += 4; break;
        case 0x29: { unsigned v; RD8F(v, ADDR16(W1 + rC)); rA = (uint8_t)v; CK(1); pc += 3; break; }
        case 0x28: WR8(ADDR16(W1 + rC), rA); CK(1); pc += 3; break;
        case 0x38: WR8(ADDR16(W1 + rC), B3); CK(1); pc += 4; break;
        case 0x49: { unsigned v; RD8F(v, ADDR16(W1 + BC)); rA = (uint8_t)v; CK(1); pc += 3; break; }
        case 0x48: WR8(ADDR16(W1 + BC), rA); CK(1); pc += 3; break;
        case 0x39: WR8(ADDR16(W1 + BC), B3); CK(1); pc += 4; break;
        case 0x59: { unsigned v; RD16F(v, ADDR16(W1 + rB)); S16(R, v); CKS1(1, 2); pc += 3; break; }
        case 0x58: WR16(ADDR16(W1 + rB), AX); CKS1(1, 2); pc += 3; break;
        case 0x69: { unsigned v; RD16F(v, ADDR16(W1 + rC)); S16(R, v); CKS1(1, 2); pc += 3; break; }
        case 0x68: WR16(ADDR16(W1 + rC), AX); CKS1(1, 2); pc += 3; break;
        case 0x79: { unsigned v; RD16F(v, ADDR16(W1 + BC)); S16(R, v); CKS1(1, 2); pc += 3; break; }
        case 0x78: WR16(ADDR16(W1 + BC), AX); CKS1(1, 2); pc += 3; break;

        case 0x40: { /* CMP !addr16,#byte */
            unsigned v;
            RD8F(v, ADDR16(W1));
            alu8(OP_CMP, v, B3, &F);
            CK(1);
            pc += 4;
            break;
        }
        case 0x41: C->es = (uint8_t)(B1 & 0x0Fu); CK(1); pc += 2; break; /* MOV ES,#byte */

        /* ---- 8-bit ALU rows (map 1, columns A..F) ---- */
#define ALU_ROW(base, OPN)                                                         \
    case (base) + 0xA: { /* op saddr,#byte */                                      \
        uint32_t a = SADDR(B1);                                                    \
        unsigned v, r;                                                             \
        RD8(v, a);                                                                 \
        r = alu8(OPN, v, B2, &F);                                                  \
        if ((OPN) != OP_CMP) { WR8(a, r); CK(2); } else CK(1);                     \
        pc += 3;                                                                   \
        break;                                                                     \
    }                                                                              \
    case (base) + 0xB: { unsigned v, r; RD8(v, SADDR(B1)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 2; break; }             \
    case (base) + 0xC: { unsigned r = alu8(OPN, rA, B1, &F);                       \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 2; break; }             \
    case (base) + 0xD: { unsigned v, r; RD8F(v, ADDR16(HL)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 1; break; }             \
    case (base) + 0xE: { unsigned v, r; RD8F(v, ADDR16(HL + B1)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 2; break; }             \
    case (base) + 0xF: { unsigned v, r; RD8F(v, ADDR16(W1)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 3; break; }
            ALU_ROW(0x00, OP_ADD)
            ALU_ROW(0x10, OP_ADDC)
            ALU_ROW(0x20, OP_SUB)
            ALU_ROW(0x30, OP_SUBC)
            ALU_ROW(0x40, OP_CMP)
            ALU_ROW(0x50, OP_AND)
            ALU_ROW(0x60, OP_OR)
            ALU_ROW(0x70, OP_XOR)
#undef ALU_ROW

        /* ---- MOV r,#byte / MOV A,r / MOV r,A ---- */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            R[op & 7u] = (uint8_t)B1;
            CK(1);
            pc += 2;
            break;
        case 0x60: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
            rA = R[op & 7u];
            CK(1);
            pc += 1;
            break;
        case 0x70: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
            R[op & 7u] = rA;
            CK(1);
            pc += 1;
            break;

        /* ---- INC/DEC r ---- */
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
            R[op & 7u] = (uint8_t)inc8(R[op & 7u], &F);
            CK(1);
            pc += 1;
            break;
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
            R[op & 7u] = (uint8_t)dec8(R[op & 7u], &F);
            CK(1);
            pc += 1;
            break;

        /* ---- MOV A,mem / mem,A ---- */
        case 0x88: { unsigned v; RD8(v, 0xF0000u | ((C->sp + B1) & 0xFFFFu)); rA = (uint8_t)v; CK(1); pc += 2; break; }
        case 0x98: WR8(0xF0000u | ((C->sp + B1) & 0xFFFFu), rA); CK(1); pc += 2; break;
        case 0xC8: WR8(0xF0000u | ((C->sp + B1) & 0xFFFFu), B2); CK(1); pc += 3; break;
        case 0x89: { unsigned v; RD8F(v, ADDR16(DE)); rA = (uint8_t)v; CK(1); pc += 1; break; }
        case 0x99: WR8(ADDR16(DE), rA); CK(1); pc += 1; break;
        case 0x8A: { unsigned v; RD8F(v, ADDR16(DE + B1)); rA = (uint8_t)v; CK(1); pc += 2; break; }
        case 0x9A: WR8(ADDR16(DE + B1), rA); CK(1); pc += 2; break;
        case 0xCA: WR8(ADDR16(DE + B1), B2); CK(1); pc += 3; break;
        case 0x8B: { unsigned v; RD8F(v, ADDR16(HL)); rA = (uint8_t)v; CK(1); pc += 1; break; }
        case 0x9B: WR8(ADDR16(HL), rA); CK(1); pc += 1; break;
        case 0x8C: { unsigned v; RD8F(v, ADDR16(HL + B1)); rA = (uint8_t)v; CK(1); pc += 2; break; }
        case 0x9C: WR8(ADDR16(HL + B1), rA); CK(1); pc += 2; break;
        case 0xCC: WR8(ADDR16(HL + B1), B2); CK(1); pc += 3; break;
        case 0x8D: { unsigned v; RD8(v, SADDR(B1)); rA = (uint8_t)v; CK(1); pc += 2; break; }
        case 0x9D: WR8(SADDR(B1), rA); CK(1); pc += 2; break;
        case 0xCD: WR8(SADDR(B1), B2); CK(1); pc += 3; break;
        case 0x8E: { unsigned v; RD8(v, SFRA(B1)); rA = (uint8_t)v; CK(1); pc += 2; break; }
        case 0x9E: { /* MOV sfr,A  (MOV PSW,A: 3 clocks, hold) */
            unsigned s = B1;
            WR8(SFRA(s), rA);
            if (s == 0xFAu) { CK(3); HOLD(); } else CK(1);
            pc += 2;
            break;
        }
        case 0xCE: { /* MOV sfr,#byte ; S3: CE FB xx = MULHU..MACH */
            unsigned s = B1;
            if (s == 0xFBu && s3) {
                unsigned sub = B2;
                if (sub == 0x01u) { /* MULHU */
                    uint32_t r = (uint32_t)AX * (uint32_t)BC;
                    S16(R, r & 0xFFFFu); S16(R + 2, r >> 16);
                    CK(2); pc += 3; break;
                }
                if (sub == 0x02u) { /* MULH */
                    int32_t r = (int32_t)(int16_t)AX * (int32_t)(int16_t)BC;
                    uint32_t u = (uint32_t)r;
                    S16(R, u & 0xFFFFu); S16(R + 2, u >> 16);
                    CK(2); pc += 3; break;
                }
                if (sub == 0x03u) { /* DIVHU */
                    unsigned a = AX, d = DE;
                    if (d == 0) { S16(R + 4, a); S16(R, 0xFFFFu); }
                    else { S16(R, a / d); S16(R + 4, a % d); }
                    CK(9); pc += 3; break;
                }
                if (sub == 0x0Bu) { /* DIVWU */
                    uint32_t a = (uint32_t)AX | ((uint32_t)BC << 16);
                    uint32_t d = (uint32_t)DE | ((uint32_t)HL << 16);
                    uint32_t q, r;
                    if (d == 0) { q = 0xFFFFFFFFu; r = a; }
                    else { q = a / d; r = a % d; }
                    S16(R, q & 0xFFFFu); S16(R + 2, q >> 16);
                    S16(R + 4, r & 0xFFFFu); S16(R + 6, r >> 16);
                    CK(17); pc += 3; break;
                }
                if (sub == 0x05u || sub == 0x06u) { /* MACHU / MACH */
                    unsigned lo, hi;
                    uint32_t acc, res;
                    RD16(lo, 0xFFFF0u);
                    RD16(hi, 0xFFFF2u);
                    acc = (uint32_t)lo | ((uint32_t)hi << 16);
                    if (sub == 0x05u) {
                        uint32_t prod = (uint32_t)AX * (uint32_t)BC;
                        uint64_t sum = (uint64_t)acc + prod;
                        res = (uint32_t)sum;
                        F = (F & ~(F_AC | F_CY)) | ((sum >> 32) ? F_CY : 0u);
                    } else {
                        int64_t prod = (int64_t)((int32_t)(int16_t)AX * (int32_t)(int16_t)BC);
                        int64_t sum = (int64_t)(int32_t)acc + prod;
                        res = (uint32_t)sum;
                        F = (F & ~(F_AC | F_CY)) |
                            ((sum > 0x7FFFFFFFLL || sum < -0x80000000LL) ? F_CY : 0u) |
                            ((res & 0x80000000u) ? F_AC : 0u);
                    }
                    /* MACR shares FFFF0H-FFFF3H with MDAL/MDAH of the G13
                       multiplier: write it without multiplier side effects */
                    SYNC_OUT();
                    rl78__write_macr(C, res);
                    SYNC_IN();
                    CK(3); pc += 3; break;
                }
                /* other CE FB xx: plain MOV to the reserved SFR FFFFBH */
            }
            WR8(SFRA(s), B2);
            if (s == 0xFAu) { CK(3); HOLD(); } else CK(1);
            pc += 3;
            break;
        }
        case 0x8F: { unsigned v; RD8F(v, ADDR16(W1)); rA = (uint8_t)v; CK(1); pc += 3; break; }
        case 0x9F: WR8(ADDR16(W1), rA); CK(1); pc += 3; break;
        case 0xCF: WR8(ADDR16(W1), B3); CK(1); pc += 4; break;

        /* ---- INC/DEC/INCW/DECW memory ---- */
        case 0xA0: { uint32_t a = ADDR16(W1); unsigned v; RD8(v, a); WR8(a, inc8(v, &F)); CK(2); pc += 3; break; }
        case 0xA4: { uint32_t a = SADDR(B1); unsigned v; RD8(v, a); WR8(a, inc8(v, &F)); CK(2); pc += 2; break; }
        case 0xB0: { uint32_t a = ADDR16(W1); unsigned v; RD8(v, a); WR8(a, dec8(v, &F)); CK(2); pc += 3; break; }
        case 0xB4: { uint32_t a = SADDR(B1); unsigned v; RD8(v, a); WR8(a, dec8(v, &F)); CK(2); pc += 2; break; }
        case 0xA2: { uint32_t a = ADDR16(W1); unsigned v; RD16(v, a); WR16(a, (v + 1u) & 0xFFFFu); CKS1(2, 4); pc += 3; break; }
        case 0xA6: { uint32_t a = SADDR(B1); unsigned v; RD16(v, a); WR16(a, (v + 1u) & 0xFFFFu); CKS1(2, 4); pc += 2; break; }
        case 0xB2: { uint32_t a = ADDR16(W1); unsigned v; RD16(v, a); WR16(a, (v - 1u) & 0xFFFFu); CKS1(2, 4); pc += 3; break; }
        case 0xB6: { uint32_t a = SADDR(B1); unsigned v; RD16(v, a); WR16(a, (v - 1u) & 0xFFFFu); CKS1(2, 4); pc += 2; break; }
        case 0xA1: case 0xA3: case 0xA5: case 0xA7: {
            uint8_t *p = R + (op - 0xA1u);
            unsigned v = (G16(p) + 1u) & 0xFFFFu;
            S16(p, v);
            CKS1(1, 2);
            pc += 1;
            break;
        }
        case 0xB1: case 0xB3: case 0xB5: case 0xB7: {
            uint8_t *p = R + (op - 0xB1u);
            unsigned v = (G16(p) - 1u) & 0xFFFFu;
            S16(p, v);
            CKS1(1, 2);
            pc += 1;
            break;
        }

        /* ---- MOVW AX,mem / mem,AX ---- */
        case 0xA8: { unsigned v; RD16(v, 0xF0000u | ((C->sp + B1) & 0xFFFFu)); S16(R, v); CKS1(1, 2); pc += 2; break; }
        case 0xB8: WR16(0xF0000u | ((C->sp + B1) & 0xFFFFu), AX); CKS1(1, 2); pc += 2; break;
        case 0xA9: { unsigned v; RD16F(v, ADDR16(DE)); S16(R, v); CKS1(1, 2); pc += 1; break; }
        case 0xB9: WR16(ADDR16(DE), AX); CKS1(1, 2); pc += 1; break;
        case 0xAA: { unsigned v; RD16F(v, ADDR16(DE + B1)); S16(R, v); CKS1(1, 2); pc += 2; break; }
        case 0xBA: WR16(ADDR16(DE + B1), AX); CKS1(1, 2); pc += 2; break;
        case 0xAB: { unsigned v; RD16F(v, ADDR16(HL)); S16(R, v); CKS1(1, 2); pc += 1; break; }
        case 0xBB: WR16(ADDR16(HL), AX); CKS1(1, 2); pc += 1; break;
        case 0xAC: { unsigned v; RD16F(v, ADDR16(HL + B1)); S16(R, v); CKS1(1, 2); pc += 2; break; }
        case 0xBC: WR16(ADDR16(HL + B1), AX); CKS1(1, 2); pc += 2; break;
        case 0xAD: { unsigned v; RD16(v, SADDR(B1)); S16(R, v); CKS1(1, 2); pc += 2; break; }
        case 0xBD: WR16(SADDR(B1), AX); CKS1(1, 2); pc += 2; break;
        case 0xAE: { unsigned v; RD16(v, SFRA(B1)); S16(R, v); CKS1(1, 2); pc += 2; break; }
        case 0xBE: WR16(SFRA(B1), AX); CKS1(1, 2); pc += 2; break;
        case 0xAF: { unsigned v; RD16F(v, ADDR16(W1)); S16(R, v); CKS1(1, 2); pc += 3; break; }
        case 0xBF: WR16(ADDR16(W1), AX); CKS1(1, 2); pc += 3; break;
        case 0xC9: WR16(SADDR(B1), W2); CKS1(1, 2); pc += 4; break;
        case 0xCB: WR16(SFRA(B1), W2); CKS1(1, 2); pc += 4; break;
        case 0xDA: { unsigned v; RD16(v, SADDR(B1)); S16(R + 2, v); CKS1(1, 2); pc += 2; break; }
        case 0xEA: { unsigned v; RD16(v, SADDR(B1)); S16(R + 4, v); CKS1(1, 2); pc += 2; break; }
        case 0xFA: { unsigned v; RD16(v, SADDR(B1)); S16(R + 6, v); CKS1(1, 2); pc += 2; break; }
        case 0xDB: { unsigned v; RD16F(v, ADDR16(W1)); S16(R + 2, v); CKS1(1, 2); pc += 3; break; }
        case 0xEB: { unsigned v; RD16F(v, ADDR16(W1)); S16(R + 4, v); CKS1(1, 2); pc += 3; break; }
        case 0xFB: { unsigned v; RD16F(v, ADDR16(W1)); S16(R + 6, v); CKS1(1, 2); pc += 3; break; }

        /* ---- PUSH/POP rp ---- */
        case 0xC1: case 0xC3: case 0xC5: case 0xC7: {
            uint8_t *p = R + (op - 0xC1u);
            uint32_t sp = (C->sp - 2u) & 0xFFFFu;
            WR8(0xF0000u | ((sp + 1u) & 0xFFFFu), p[1]);
            WR8(0xF0000u | sp, p[0]);
            C->sp = (uint16_t)sp;
            CKS1(1, 2);
            pc += 1;
            break;
        }
        case 0xC0: case 0xC2: case 0xC4: case 0xC6: {
            unsigned lo, hi;
            uint32_t sp = C->sp;
            uint8_t *p;
            RD8(lo, 0xF0000u | sp);
            RD8(hi, 0xF0000u | ((sp + 1u) & 0xFFFFu));
            p = R + (op - 0xC0u);
            p[0] = (uint8_t)lo;
            p[1] = (uint8_t)hi;
            C->sp = (uint16_t)((sp + 2u) & 0xFFFFu);
            CKS1(1, 2);
            pc += 1;
            break;
        }

        /* ---- CMP0 / ONEB / CLRB / ONEW / CLRW ---- */
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            static const uint8_t ri[4] = {0, 1, 2, 3};
            unsigned v = R[ri[op & 3u]];
            F = (F & ~(F_Z | F_AC | F_CY)) | (v ? 0u : F_Z);
            CK(1);
            pc += 1;
            break;
        }
        case 0xD4: { unsigned v; RD8(v, SADDR(B1)); F = (F & ~(F_Z | F_AC | F_CY)) | (v ? 0u : F_Z); CK(1); pc += 2; break; }
        case 0xD5: { unsigned v; RD8F(v, ADDR16(W1)); F = (F & ~(F_Z | F_AC | F_CY)) | (v ? 0u : F_Z); CK(1); pc += 3; break; }
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: R[op & 3u] = 1; CK(1); pc += 1; break;
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: R[op & 3u] = 0; CK(1); pc += 1; break;
        case 0xE4: WR8(SADDR(B1), 1); CK(1); pc += 2; break;
        case 0xF4: WR8(SADDR(B1), 0); CK(1); pc += 2; break;
        case 0xE5: WR8(ADDR16(W1), 1); CK(1); pc += 3; break;
        case 0xF5: WR8(ADDR16(W1), 0); CK(1); pc += 3; break;
        case 0xE6: R[0] = 1; R[1] = 0; CKS1(1, 2); pc += 1; break;
        case 0xE7: R[2] = 1; R[3] = 0; CKS1(1, 2); pc += 1; break;
        case 0xF6: R[0] = 0; R[1] = 0; CKS1(1, 2); pc += 1; break;
        case 0xF7: R[2] = 0; R[3] = 0; CKS1(1, 2); pc += 1; break;

        case 0xD6: { unsigned r = (unsigned)rA * rX; S16(R, r); CKS1(1, 2); pc += 1; break; } /* MULU X */

        case 0xD7: { /* RET */
            uint32_t sp = C->sp;
            unsigned l, h, s;
            RD8(l, 0xF0000u | sp);
            RD8(h, 0xF0000u | ((sp + 1u) & 0xFFFFu));
            RD8(s, 0xF0000u | ((sp + 2u) & 0xFFFFu));
            C->sp = (uint16_t)((sp + 4u) & 0xFFFFu);
            pc = (l | (h << 8) | ((s & 0xFu) << 16));
            CKS1(6, 7);
            break;
        }

        case 0xD8: { unsigned v; RD8(v, SADDR(B1)); rX = (uint8_t)v; CK(1); pc += 2; break; }
        case 0xE8: { unsigned v; RD8(v, SADDR(B1)); rB = (uint8_t)v; CK(1); pc += 2; break; }
        case 0xF8: { unsigned v; RD8(v, SADDR(B1)); rC = (uint8_t)v; CK(1); pc += 2; break; }
        case 0xD9: { unsigned v; RD8F(v, ADDR16(W1)); rX = (uint8_t)v; CK(1); pc += 3; break; }
        case 0xE9: { unsigned v; RD8F(v, ADDR16(W1)); rB = (uint8_t)v; CK(1); pc += 3; break; }
        case 0xF9: { unsigned v; RD8F(v, ADDR16(W1)); rC = (uint8_t)v; CK(1); pc += 3; break; }

        /* ---- branches ---- */
        case 0xDC: if (F & F_CY) { pc = (pc + 2u + (uint32_t)(int32_t)(int8_t)B1) & 0xFFFFFu; CK(4); } else { pc += 2; CK(2); } break;
        case 0xDE: if (!(F & F_CY)) { pc = (pc + 2u + (uint32_t)(int32_t)(int8_t)B1) & 0xFFFFFu; CK(4); } else { pc += 2; CK(2); } break;
        case 0xDD: if (F & F_Z) { pc = (pc + 2u + (uint32_t)(int32_t)(int8_t)B1) & 0xFFFFFu; CK(4); } else { pc += 2; CK(2); } break;
        case 0xDF: if (!(F & F_Z)) { pc = (pc + 2u + (uint32_t)(int32_t)(int8_t)B1) & 0xFFFFFu; CK(4); } else { pc += 2; CK(2); } break;
        case 0xEC: pc = (W1 | ((B3 & 0xFu) << 16)); CK(3); break;
        case 0xED: pc = W1; CK(3); break;
        case 0xEE: pc = (pc + 3u + (uint32_t)(int32_t)(int16_t)W1) & 0xFFFFFu; CK(3); break;
        case 0xEF: pc = (pc + 2u + (uint32_t)(int32_t)(int8_t)B1) & 0xFFFFFu; CK(3); break;
        case 0xFC: case 0xFD: case 0xFE: { /* CALL !!addr20 / !addr16 / $!addr20 */
            uint32_t ret, tgt, sp = C->sp;
            if (op == 0xFC) { ret = pc + 4u; tgt = W1 | ((B3 & 0xFu) << 16); }
            else if (op == 0xFD) { ret = pc + 3u; tgt = W1; }
            else { ret = pc + 3u; tgt = (pc + 3u + (uint32_t)(int32_t)(int16_t)W1) & 0xFFFFFu; }
            ret &= 0xFFFFFu;
            WR8(0xF0000u | ((sp - 1u) & 0xFFFFu), 0);
            WR8(0xF0000u | ((sp - 2u) & 0xFFFFu), ret >> 16);
            WR8(0xF0000u | ((sp - 3u) & 0xFFFFu), ret >> 8);
            WR8(0xF0000u | ((sp - 4u) & 0xFFFFu), ret);
            C->sp = (uint16_t)((sp - 4u) & 0xFFFFu);
            pc = tgt;
            CKS1(3, 4);
            break;
        }

        /* ---- ES: prefix ---- */
        case 0x11: {
            unsigned nop = M[pc + 1];
            if (!es_ok(nop, M[pc + 2])) UNDEF();
            seg = (uint32_t)C->es << 16;
            pc += 1;
            CK(1);
            op = nop;
            goto dispatch;
        }

        /* ================================================================ */
        case 0x61: {
            unsigned o = B1;
            switch (o) {
            /* ALU r,A (x0..x7) and A,r (x8..xF), rows 0..7 */
#define ALU_REG_ROW(row, OPN)                                                      \
    case (row) * 16 + 0: case (row) * 16 + 1: case (row) * 16 + 2: case (row) * 16 + 3: \
    case (row) * 16 + 4: case (row) * 16 + 5: case (row) * 16 + 6: case (row) * 16 + 7: { \
        unsigned k = o & 7u;                                                       \
        unsigned r = alu8(OPN, R[k], rA, &F);                                      \
        if ((OPN) != OP_CMP) R[k] = (uint8_t)r;                                    \
        CK(1); pc += 2; break; }                                                   \
    case (row) * 16 + 8: case (row) * 16 + 10: case (row) * 16 + 11: case (row) * 16 + 12: \
    case (row) * 16 + 13: case (row) * 16 + 14: case (row) * 16 + 15: {           \
        unsigned k = o & 7u;                                                       \
        unsigned r = alu8(OPN, rA, R[k], &F);                                      \
        if ((OPN) != OP_CMP) rA = (uint8_t)r;                                      \
        CK(1); pc += 2; break; }
                ALU_REG_ROW(0, OP_ADD)
                ALU_REG_ROW(1, OP_ADDC)
                ALU_REG_ROW(2, OP_SUB)
                ALU_REG_ROW(3, OP_SUBC)
                ALU_REG_ROW(4, OP_CMP)
                ALU_REG_ROW(5, OP_AND)
                ALU_REG_ROW(6, OP_OR)
                ALU_REG_ROW(7, OP_XOR)
#undef ALU_REG_ROW
                /* ALU A,[HL+B] (x0) / A,[HL+C] (x2), rows 8..F */
#define ALU_HLX_ROW(row, OPN)                                                      \
    case (row) * 16 + 0: { unsigned v, r; RD8F(v, ADDR16(HL + rB)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 2; break; }             \
    case (row) * 16 + 2: { unsigned v, r; RD8F(v, ADDR16(HL + rC)); r = alu8(OPN, rA, v, &F); \
        if ((OPN) != OP_CMP) rA = (uint8_t)r; CK(1); pc += 2; break; }
                ALU_HLX_ROW(8, OP_ADD)
                ALU_HLX_ROW(9, OP_ADDC)
                ALU_HLX_ROW(10, OP_SUB)
                ALU_HLX_ROW(11, OP_SUBC)
                ALU_HLX_ROW(12, OP_CMP)
                ALU_HLX_ROW(13, OP_AND)
                ALU_HLX_ROW(14, OP_OR)
                ALU_HLX_ROW(15, OP_XOR)
#undef ALU_HLX_ROW

            case 0x09: { unsigned v; RD16F(v, ADDR16(HL + B2)); { unsigned r = addw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 3; break; }
            case 0x29: { unsigned v; RD16F(v, ADDR16(HL + B2)); { unsigned r = subw(AX, v, &F); S16(R, r); } CKS1(1, 2); pc += 3; break; }
            case 0x49: { unsigned v; RD16F(v, ADDR16(HL + B2)); subw(AX, v, &F); CKS1(1, 2); pc += 3; break; }
            case 0x59: { uint32_t a = ADDR16(HL + B2); unsigned v; RD8(v, a); WR8(a, inc8(v, &F)); CK(2); pc += 3; break; }
            case 0x69: { uint32_t a = ADDR16(HL + B2); unsigned v; RD8(v, a); WR8(a, dec8(v, &F)); CK(2); pc += 3; break; }
            case 0x79: { uint32_t a = ADDR16(HL + B2); unsigned v; RD16(v, a); WR16(a, (v + 1u) & 0xFFFFu); CKS1(2, 4); pc += 3; break; }
            case 0x89: { uint32_t a = ADDR16(HL + B2); unsigned v; RD16(v, a); WR16(a, (v - 1u) & 0xFFFFu); CKS1(2, 4); pc += 3; break; }

            /* XCH A,r (r = C,B,E,D,L,H) */
            case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
                unsigned k = o & 7u;
                uint8_t t = rA; rA = R[k]; R[k] = t;
                CK(1);
                pc += 2;
                break;
            }
/* A is the register of the bank selected when the instruction starts, even
   if the write switches banks (XCH A, PSW). */
#define XCH_MEM(addr, len)                                                         \
    { uint32_t a = (addr); unsigned v; uint8_t *pa = &rA; RD8(v, a); WR8(a, *pa); *pa = (uint8_t)v; CK(2); pc += (len); break; }
            case 0xA8: XCH_MEM(SADDR(B2), 3)
            case 0xAB: XCH_MEM(SFRA(B2), 3)
            case 0xAA: XCH_MEM(ADDR16(W2), 4)
            case 0xAC: XCH_MEM(ADDR16(HL), 2)
            case 0xAD: XCH_MEM(ADDR16(HL + B2), 3)
            case 0xAE: XCH_MEM(ADDR16(DE), 2)
            case 0xAF: XCH_MEM(ADDR16(DE + B2), 3)
            case 0xB9: XCH_MEM(ADDR16(HL + rB), 2)
            case 0xA9: XCH_MEM(ADDR16(HL + rC), 2)
#undef XCH_MEM
            case 0xB8: { unsigned v; RD8(v, SADDR(B2)); C->es = (uint8_t)(v & 0xFu); CK(1); pc += 3; break; }

            case 0xC9: { unsigned v; RD8F(v, ADDR16(HL + rB)); rA = (uint8_t)v; CK(1); pc += 2; break; }
            case 0xD9: WR8(ADDR16(HL + rB), rA); CK(1); pc += 2; break;
            case 0xE9: { unsigned v; RD8F(v, ADDR16(HL + rC)); rA = (uint8_t)v; CK(1); pc += 2; break; }
            case 0xF9: WR8(ADDR16(HL + rC), rA); CK(1); pc += 2; break;

            case 0xCE: { /* MOVS [HL+byte],X */
                unsigned x = rX;
                WR8(ADDR16(HL + B2), x);
                F = (F & ~(F_Z | F_CY)) | (x ? 0u : F_Z) | ((rA == 0 || x == 0) ? F_CY : 0u);
                CK(1);
                pc += 3;
                break;
            }
            case 0xDE: { /* CMPS X,[HL+byte] */
                unsigned v, x = rX, r;
                RD8F(v, ADDR16(HL + B2));
                r = (x - v);
                F = (F & ~(F_Z | F_AC | F_CY)) | ((r & 0xFFu) ? 0u : F_Z) | ((x ^ v ^ r) & 0x10u) |
                    (((r & 0xFFu) != 0 || rA == 0 || x == 0) ? F_CY : 0u);
                CK(1);
                pc += 3;
                break;
            }

            /* CALLT [addr5] */
            case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x94: case 0x95: case 0x96: case 0x97:
            case 0xA4: case 0xA5: case 0xA6: case 0xA7:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            case 0xC4: case 0xC5: case 0xC6: case 0xC7:
            case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            case 0xF4: case 0xF5: case 0xF6: case 0xF7: {
                uint32_t tab = 0x80u + (((o & 0xFu) - 4u) << 4) + (((o >> 4) - 8u) << 1);
                uint32_t ret = (pc + 2u) & 0xFFFFFu, sp = C->sp;
                unsigned tgt = (unsigned)M[tab] | ((unsigned)M[tab + 1] << 8);
                WR8(0xF0000u | ((sp - 1u) & 0xFFFFu), 0);
                WR8(0xF0000u | ((sp - 2u) & 0xFFFFu), ret >> 16);
                WR8(0xF0000u | ((sp - 3u) & 0xFFFFu), ret >> 8);
                WR8(0xF0000u | ((sp - 4u) & 0xFFFFu), ret);
                C->sp = (uint16_t)((sp - 4u) & 0xFFFFu);
                pc = tgt;
                CKS1(5, 6);
                break;
            }
            case 0xCA: case 0xDA: case 0xEA: case 0xFA: { /* CALL rp */
                uint32_t ret = (pc + 2u) & 0xFFFFFu, sp = C->sp;
                unsigned tgt = G16(R + ((o >> 4) - 0xCu) * 2u);
                WR8(0xF0000u | ((sp - 1u) & 0xFFFFu), 0);
                WR8(0xF0000u | ((sp - 2u) & 0xFFFFu), ret >> 16);
                WR8(0xF0000u | ((sp - 3u) & 0xFFFFu), ret >> 8);
                WR8(0xF0000u | ((sp - 4u) & 0xFFFFu), ret);
                C->sp = (uint16_t)((sp - 4u) & 0xFFFFu);
                pc = ((uint32_t)(C->cs & 0xFu) << 16) | tgt;
                CKS1(3, 4);
                break;
            }
            case 0xCB: pc = ((uint32_t)(C->cs & 0xFu) << 16) | AX; CK(3); break; /* BR AX */
            case 0xCC: { /* BRK */
                uint32_t ret = (pc + 2u) & 0xFFFFFu, sp = C->sp;
                WR8(0xF0000u | ((sp - 1u) & 0xFFFFu), F);
                WR8(0xF0000u | ((sp - 2u) & 0xFFFFu), ret >> 16);
                WR8(0xF0000u | ((sp - 3u) & 0xFFFFu), ret >> 8);
                WR8(0xF0000u | ((sp - 4u) & 0xFFFFu), ret);
                C->sp = (uint16_t)((sp - 4u) & 0xFFFFu);
                F &= ~F_IE;
                pc = (unsigned)M[0x7E] | ((unsigned)M[0x7F] << 8);
                CKS1(5, 7);
                break;
            }
            case 0xFC: case 0xEC: { /* RETI / RETB */
                uint32_t sp = C->sp;
                unsigned l, h, s, p;
                RD8(l, 0xF0000u | sp);
                RD8(h, 0xF0000u | ((sp + 1u) & 0xFFFFu));
                RD8(s, 0xF0000u | ((sp + 2u) & 0xFFFFu));
                RD8(p, 0xF0000u | ((sp + 3u) & 0xFFFFu));
                C->sp = (uint16_t)((sp + 4u) & 0xFFFFu);
                pc = l | (h << 8) | ((s & 0xFu) << 16);
                F = p;
                PSW_CHANGED();
                CKS1(6, 8);
                HOLD();
                break;
            }
            case 0xCD: { /* POP PSW */
                uint32_t sp = C->sp;
                unsigned p;
                RD8(p, 0xF0000u | ((sp + 1u) & 0xFFFFu));
                C->sp = (uint16_t)((sp + 2u) & 0xFFFFu);
                F = p;
                PSW_CHANGED();
                CKS1(3, 4);
                pc += 2;
                HOLD();
                break;
            }
            case 0xDD: { /* PUSH PSW */
                uint32_t sp = C->sp;
                WR8(0xF0000u | ((sp - 1u) & 0xFFFFu), F);
                WR8(0xF0000u | ((sp - 2u) & 0xFFFFu), 0);
                C->sp = (uint16_t)((sp - 2u) & 0xFFFFu);
                CKS1(1, 2);
                pc += 2;
                break;
            }
            case 0xCF: case 0xDF: case 0xEF: case 0xFF: { /* SEL RBn */
                unsigned b = (o >> 4) - 0xCu;
                if (s1) UNDEF();
                F = (F & ~(F_RBS0 | F_RBS1)) | ((b & 1u) << 3) | ((b & 2u) << 4);
                PSW_CHANGED();
                CK(1);
                pc += 2;
                break;
            }
            case 0xDB: { unsigned a = rA; F = (F & ~F_CY) | (a & 1u); rA = (uint8_t)((a >> 1) | (a << 7)); CK(1); pc += 2; break; } /* ROR */
            case 0xEB: { unsigned a = rA; F = (F & ~F_CY) | (a >> 7); rA = (uint8_t)((a << 1) | (a >> 7)); CK(1); pc += 2; break; }    /* ROL */
            case 0xFB: { unsigned a = rA; unsigned c = F & F_CY; F = (F & ~F_CY) | (a & 1u); rA = (uint8_t)((a >> 1) | (c << 7)); CK(1); pc += 2; break; } /* RORC */
            case 0xDC: { unsigned a = rA; unsigned c = F & F_CY; F = (F & ~F_CY) | (a >> 7); rA = (uint8_t)((a << 1) | c); CK(1); pc += 2; break; } /* ROLC */
            case 0xEE: case 0xFE: { /* ROLWC AX,1 / BC,1 */
                uint8_t *p = R + (o == 0xEEu ? 0u : 2u);
                unsigned v = G16(p), c = F & F_CY;
                F = (F & ~F_CY) | (v >> 15);
                v = ((v << 1) | c) & 0xFFFFu;
                S16(p, v);
                CKS1(1, 2);
                pc += 2;
                break;
            }
            /* skips */
#define SKIP_IF(cond)                                                              \
    {                                                                              \
        CK(1);                                                                     \
        pc += 2;                                                                   \
        HOLD();                                                                    \
        if (cond) {                                                                \
            int l_;                                                                \
            unsigned first_;                                                       \
            SYNC_OUT();                                                            \
            l_ = rl78__insn_len(C, pc);                                            \
            first_ = rl78__rd8(C, pc);                                             \
            n++; /* the skipped instruction occupies one instruction slot */      \
            CK(first_ == 0x11u ? 2 : 1);                                           \
            pc += (uint32_t)l_;                                                    \
        }                                                                          \
        break;                                                                     \
    }
            case 0xC8: SKIP_IF(F & F_CY)
            case 0xD8: SKIP_IF(!(F & F_CY))
            case 0xE8: SKIP_IF(F & F_Z)
            case 0xF8: SKIP_IF(!(F & F_Z))
            case 0xE3: SKIP_IF(!(F & (F_Z | F_CY)))
            case 0xF3: SKIP_IF(F & (F_Z | F_CY))
#undef SKIP_IF
            case 0xC3: if (!(F & (F_Z | F_CY))) { pc = (pc + 3u + (uint32_t)(int32_t)(int8_t)B2) & 0xFFFFFu; CK(4); } else { pc += 3; CK(2); } break; /* BH */
            case 0xD3: if (F & (F_Z | F_CY)) { pc = (pc + 3u + (uint32_t)(int32_t)(int8_t)B2) & 0xFFFFFu; CK(4); } else { pc += 3; CK(2); } break;    /* BNH */
            case 0xED: /* HALT */
                CK(3);
                pc += 2;
                if (C->cfg.halt_policy == RL78_SLEEP_RETURN) {
                    C->halted = 1;
                    *why = RL78_STOP_HALT;
                    lim = n;
                }
                break;
            case 0xFD: /* STOP */
                CK(3);
                pc += 2;
                if (C->cfg.stop_policy == RL78_SLEEP_RETURN) {
                    C->halted = 2;
                    *why = RL78_STOP_STOPMODE;
                    lim = n;
                }
                break;
            default: UNDEF();
            }
            break;
        }

        /* ================================================================ */
        case 0x71: {
            unsigned o = B1;
            unsigned bit = (o >> 4) & 7u;
            unsigned m = 1u << bit;
            if (o < 0x80u) {
                switch (o & 0xFu) {
                case 0x0: { uint32_t a = ADDR16(W2); unsigned v; RD8(v, a); WR8(a, v | m); CK(2); pc += 4; break; } /* SET1 !addr16.b */
                case 0x8: { uint32_t a = ADDR16(W2); unsigned v; RD8(v, a); WR8(a, v & ~m); CK(2); pc += 4; break; } /* CLR1 !addr16.b */
                case 0x1: { uint32_t a = SADDR(B2); unsigned v; RD8(v, a); WR8(a, (F & F_CY) ? (v | m) : (v & ~m)); CK(2); pc += 3; break; } /* MOV1 saddr.b,CY */
                case 0x2: { uint32_t a = SADDR(B2); unsigned v; RD8(v, a); WR8(a, v | m); CK(2); pc += 3; break; }
                case 0x3: { uint32_t a = SADDR(B2); unsigned v; RD8(v, a); WR8(a, v & ~m); CK(2); pc += 3; break; }
                case 0x4: { unsigned v; RD8(v, SADDR(B2)); F = (F & ~F_CY) | ((v >> bit) & 1u); CK(1); pc += 3; break; }
                case 0x5: { unsigned v; RD8(v, SADDR(B2)); F &= ~F_CY | ((v >> bit) & 1u); CK(1); pc += 3; break; }
                case 0x6: { unsigned v; RD8(v, SADDR(B2)); F |= (v >> bit) & 1u; CK(1); pc += 3; break; }
                case 0x7: { unsigned v; RD8(v, SADDR(B2)); F ^= (v >> bit) & 1u; CK(1); pc += 3; break; }
                case 0x9: { /* MOV1 sfr.b,CY */
                    unsigned s = B2, v;
                    uint32_t a = SFRA(s);
                    if (s == 0xFAu) {
                        F = (F & F_CY) ? (F | m) : (F & ~m);
                        PSW_CHANGED();
                        CK(4); pc += 3; HOLD(); break;
                    }
                    RD8(v, a); WR8(a, (F & F_CY) ? (v | m) : (v & ~m)); CK(2); pc += 3; break;
                }
                case 0xA: case 0xB: { /* SET1/CLR1 sfr.b (EI/DI = PSW.7) */
                    unsigned s = B2, v;
                    uint32_t a = SFRA(s);
                    if (s == 0xFAu) {
                        F = ((o & 0xFu) == 0xAu) ? (F | m) : (F & ~m);
                        PSW_CHANGED();
                        CK(4); pc += 3; HOLD(); break;
                    }
                    RD8(v, a);
                    WR8(a, ((o & 0xFu) == 0xAu) ? (v | m) : (v & ~m));
                    CK(2); pc += 3; break;
                }
                case 0xC: { unsigned v; RD8(v, SFRA(B2)); F = (F & ~F_CY) | ((v >> bit) & 1u); CK(1); pc += 3; break; }
                case 0xD: { unsigned v; RD8(v, SFRA(B2)); F &= ~F_CY | ((v >> bit) & 1u); CK(1); pc += 3; break; }
                case 0xE: { unsigned v; RD8(v, SFRA(B2)); F |= (v >> bit) & 1u; CK(1); pc += 3; break; }
                default: { unsigned v; RD8(v, SFRA(B2)); F ^= (v >> bit) & 1u; CK(1); pc += 3; break; }
                }
            } else {
                switch (o & 0xFu) {
                case 0x0:
                    if (o == 0x80u) { F |= F_CY; CK(1); pc += 2; break; }  /* SET1 CY */
                    if (o == 0xC0u) { F ^= F_CY; CK(1); pc += 2; break; }  /* NOT1 CY */
                    UNDEF();
                case 0x8:
                    if (o == 0x88u) { F &= ~F_CY; CK(1); pc += 2; break; } /* CLR1 CY */
                    UNDEF();
                case 0x1: { uint32_t a = ADDR16(HL); unsigned v; RD8(v, a); WR8(a, (F & F_CY) ? (v | m) : (v & ~m)); CK(2); pc += 2; break; }
                case 0x2: { uint32_t a = ADDR16(HL); unsigned v; RD8(v, a); WR8(a, v | m); CK(2); pc += 2; break; }
                case 0x3: { uint32_t a = ADDR16(HL); unsigned v; RD8(v, a); WR8(a, v & ~m); CK(2); pc += 2; break; }
                case 0x4: { unsigned v; RD8F(v, ADDR16(HL)); F = (F & ~F_CY) | ((v >> bit) & 1u); CK(1); pc += 2; break; }
                case 0x5: { unsigned v; RD8F(v, ADDR16(HL)); F &= ~F_CY | ((v >> bit) & 1u); CK(1); pc += 2; break; }
                case 0x6: { unsigned v; RD8F(v, ADDR16(HL)); F |= (v >> bit) & 1u; CK(1); pc += 2; break; }
                case 0x7: { unsigned v; RD8F(v, ADDR16(HL)); F ^= (v >> bit) & 1u; CK(1); pc += 2; break; }
                case 0x9: rA = (uint8_t)((F & F_CY) ? (rA | m) : (rA & ~m)); CK(1); pc += 2; break;
                case 0xA: rA = (uint8_t)(rA | m); CK(1); pc += 2; break;
                case 0xB: rA = (uint8_t)(rA & ~m); CK(1); pc += 2; break;
                case 0xC: F = (F & ~F_CY) | ((rA >> bit) & 1u); CK(1); pc += 2; break;
                case 0xD: F &= ~F_CY | ((rA >> bit) & 1u); CK(1); pc += 2; break;
                case 0xE: F |= (rA >> bit) & 1u; CK(1); pc += 2; break;
                default: F ^= (rA >> bit) & 1u; CK(1); pc += 2; break;
                }
            }
            break;
        }

        /* ================================================================ */
        case 0x31: {
            unsigned o = B1;
            unsigned lo = o & 0xFu;
            unsigned bit = (o >> 4) & 7u;
            unsigned m = 1u << bit;
            if (lo <= 5u) {
                unsigned v, vb, taken, pen = 0;
                uint32_t a = 0;
                int len, kind; /* kind: 0 saddr/sfr, 1 A, 2 [HL] */
                if (o < 0x80u) {
                    if (lo & 1u) { kind = 1; len = 3; v = rA; }
                    else { kind = 0; len = 4; a = SADDR(B2); RD8(v, a); }
                } else {
                    if (lo & 1u) {
                        kind = 2; len = 3; a = ADDR16(HL);
                        if (lo != 1u) pen = C->flashpen[a >> 8];
                        RD8(v, a);
                    } else { kind = 0; len = 4; a = SFRA(B2); RD8(v, a); }
                }
                vb = v;
                v = (v >> bit) & 1u;
                if (lo <= 1u) { /* BTCLR */
                    taken = v;
                    if (taken) {
                        if (kind == 1) rA = (uint8_t)(rA & ~m);
                        else if (kind == 0 && a == 0xFFFFAu) {
                            F &= ~m;
                            PSW_CHANGED();
                            HOLD();
                        } else {
                            WR8(a, vb & ~m);
                        }
                    } else if (kind == 0 && a == 0xFFFFAu) {
                        HOLD();
                    }
                } else if (lo <= 3u) taken = v; /* BT */
                else taken = !v;                /* BF */
                {
                    unsigned d = M[pc + (unsigned)len - 1u];
                    if (taken) {
                        pc = (pc + (uint32_t)len + (uint32_t)(int32_t)(int8_t)d) & 0xFFFFFu;
                        CK(5);
                        if (pen) CK(pen - 1u);
                    } else {
                        pc += (uint32_t)len;
                        CK(3);
                        CK(pen);
                    }
                }
                break;
            }
            if (lo == 6u || o < 0x10u) UNDEF();
            {
                unsigned cnt = o >> 4;
                switch (lo) {
                case 0x7: case 0x8: case 0x9: { /* SHL C/B/A, cnt */
                    uint8_t *p = (lo == 7u) ? &rC : (lo == 8u) ? &rB : &rA;
                    unsigned v;
                    if (cnt > 7u) UNDEF();
                    v = *p;
                    F = (F & ~F_CY) | ((v >> (8u - cnt)) & 1u);
                    *p = (uint8_t)(v << cnt);
                    CK(1);
                    break;
                }
                case 0xA: { /* SHR A,cnt */
                    unsigned v;
                    if (cnt > 7u) UNDEF();
                    v = rA;
                    F = (F & ~F_CY) | ((v >> (cnt - 1u)) & 1u);
                    rA = (uint8_t)(v >> cnt);
                    CK(1);
                    break;
                }
                case 0xB: { /* SAR A,cnt */
                    int v;
                    if (cnt > 7u) UNDEF();
                    v = (int8_t)rA;
                    F = (F & ~F_CY) | ((unsigned)(v >> (cnt - 1u)) & 1u);
                    rA = (uint8_t)(v >> cnt);
                    CK(1);
                    break;
                }
                case 0xC: case 0xD: { /* SHLW BC/AX, cnt */
                    uint8_t *p = R + (lo == 0xCu ? 2u : 0u);
                    unsigned v = G16(p);
                    F = (F & ~F_CY) | ((v >> (16u - cnt)) & 1u);
                    v = (v << cnt) & 0xFFFFu;
                    S16(p, v);
                    CKS1(1, 2);
                    break;
                }
                case 0xE: { /* SHRW AX,cnt */
                    unsigned v = AX;
                    F = (F & ~F_CY) | ((v >> (cnt - 1u)) & 1u);
                    v >>= cnt;
                    S16(R, v);
                    CKS1(1, 2);
                    break;
                }
                default: { /* SARW AX,cnt */
                    int32_t v = (int16_t)AX;
                    F = (F & ~F_CY) | ((unsigned)(v >> (cnt - 1u)) & 1u);
                    v >>= cnt;
                    S16(R, (unsigned)v & 0xFFFFu);
                    CKS1(1, 2);
                    break;
                }
                }
                pc += 2;
            }
            break;
        }

        /* ================================================================ */
        case BP_OPCODE: {
            if (!C->bpbits || !(C->bpbits[pc >> 3] & (1u << (pc & 7u)))) UNDEF();
            if (C->skip_bp_pc == pc && seg == 0xF0000u) {
                C->skip_bp_pc = 0xFFFFFFFFu;
                op = C->bporig[pc];
                if (op == BP_OPCODE) UNDEF();
                goto dispatch;
            }
            n--;
            *why = RL78_STOP_BREAKPOINT;
            lim = n;
            break;
        }

        default: /* 0x21 */
            UNDEF();
        }
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
