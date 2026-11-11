/*
 * t_alu.c - exhaustive / randomized comparison of arithmetic, logic, shift,
 * rotate, multiply/divide instructions against naive reference models.
 *
 * The reference models compute bit by bit (ripple-carry adder, borrow chain,
 * shift-and-add multiplier, restoring divider), deliberately unlike the
 * simulator's implementation.  Flag semantics follow R01US0015 section 6:
 *   ADD/ADDC: CY = carry out of bit 7, AC = carry from bit 3 to bit 4
 *   SUB/SUBC/CMP: CY = borrow from bit 7, AC = borrow from bit 4 to bit 3
 *   AND/OR/XOR: only Z
 *   INC: Z, AC (carry 3->4), CY unchanged;  DEC: Z, AC (borrow 4->3)
 *   shifts/rotates: only CY
 *   ADDW/SUBW/CMPW: Z, CY; AC "undefined" (simulator: carry/borrow 3->4,
 *   checked here so that a change in behaviour is noticed)
 */
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ---- reference models ---- */
static unsigned ref_add(unsigned a, unsigned b, unsigned cin, int bits, unsigned *cy, unsigned *ac)
{
    unsigned c = cin, r = 0;
    int i;
    *ac = 0;
    for (i = 0; i < bits; i++) {
        unsigned ai = (a >> i) & 1, bi = (b >> i) & 1;
        r |= (ai ^ bi ^ c) << i;
        c = (ai & bi) | (ai & c) | (bi & c);
        if (i == 3) *ac = c;
    }
    *cy = c;
    return r;
}

static unsigned ref_sub(unsigned a, unsigned b, unsigned bin, int bits, unsigned *cy, unsigned *ac)
{
    unsigned br = bin, r = 0;
    int i;
    *ac = 0;
    for (i = 0; i < bits; i++) {
        unsigned ai = (a >> i) & 1, bi = (b >> i) & 1;
        r |= (ai ^ bi ^ br) << i;
        br = ((!ai) & bi) | ((!ai) & br) | (bi & br);
        if (i == 3) *ac = br;
    }
    *cy = br;
    return r;
}

static uint64_t ref_mul(uint64_t a, uint64_t b) /* shift and add */
{
    uint64_t r = 0;
    int i;
    for (i = 0; i < 64; i++)
        if ((b >> i) & 1) r += a << i;
    return r;
}

static void ref_div(uint32_t a, uint32_t d, uint32_t *q, uint32_t *rem) /* restoring */
{
    uint64_t r = 0;
    uint32_t qq = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= d) { r -= d; qq |= 1u << i; }
    }
    *q = qq;
    *rem = (uint32_t)r;
}

/* ---- harness ---- */
static rl78_cpu *cpu;

static void exec_bytes(const uint8_t *b, int n)
{
    rl78_load(cpu, 0x200, b, (size_t)n);
    rl78_set_reg(cpu, RL78_REG_PC, 0x200);
    rl78_step(cpu);
}

static unsigned psw_with(unsigned flags)
{
    /* random IE/ISP bits that must be preserved; bank 0 */
    return (flags & (RL78_PSW_Z | RL78_PSW_AC | RL78_PSW_CY)) | (rnd() & (RL78_PSW_IE | RL78_PSW_ISP0 | RL78_PSW_ISP1));
}

#define FLAGS (RL78_PSW_Z | RL78_PSW_AC | RL78_PSW_CY)

static void test_alu8(void)
{
    /* op k: 0 ADD 1 ADDC 2 SUB 3 SUBC 4 CMP 5 AND 6 OR 7 XOR */
    unsigned k, a, b, cin;
    unsigned long n = 0;
    for (k = 0; k < 8; k++) {
        uint8_t reg_form[2] = {0x61, (uint8_t)(0x08 + k * 0x10)}; /* op A, X */
        for (a = 0; a < 256; a++)
            for (b = 0; b < 256; b++)
                for (cin = 0; cin < 2; cin++) {
                    unsigned psw0 = psw_with(cin ? RL78_PSW_CY : 0) | ((rnd() & 1) ? RL78_PSW_Z | RL78_PSW_AC : 0);
                    unsigned r = 0, cy = 0, ac = 0, expf, exppsw, z;
                    int arith = k <= 4;
                    switch (k) {
                    case 0: r = ref_add(a, b, 0, 8, &cy, &ac); break;
                    case 1: r = ref_add(a, b, cin, 8, &cy, &ac); break;
                    case 2: case 4: r = ref_sub(a, b, 0, 8, &cy, &ac); break;
                    case 3: r = ref_sub(a, b, cin, 8, &cy, &ac); break;
                    case 5: r = a & b; break;
                    case 6: r = a | b; break;
                    default: r = a ^ b; break;
                    }
                    z = (r & 0xFF) == 0;
                    if (arith) expf = (z ? RL78_PSW_Z : 0) | (ac ? RL78_PSW_AC : 0) | (cy ? RL78_PSW_CY : 0);
                    else expf = (z ? RL78_PSW_Z : 0) | (psw0 & (RL78_PSW_AC | RL78_PSW_CY));
                    exppsw = (psw0 & ~FLAGS) | expf;
                    rl78_set_reg(cpu, RL78_REG_A, a);
                    rl78_set_reg(cpu, RL78_REG_X, b);
                    rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                    exec_bytes(reg_form, 2);
                    n++;
                    t_checks++;
                    if (rl78_get_reg(cpu, RL78_REG_A) != (k == 4 ? a : (r & 0xFF)) ||
                        rl78_get_reg(cpu, RL78_REG_PSW) != exppsw || rl78_get_reg(cpu, RL78_REG_X) != b)
                        t_fail(__FILE__, __LINE__, "op%u A=%02X X=%02X CY=%u: got A=%02X PSW=%02X expected A=%02X PSW=%02X",
                               k, a, b, cin, (unsigned)rl78_get_reg(cpu, RL78_REG_A),
                               (unsigned)rl78_get_reg(cpu, RL78_REG_PSW), k == 4 ? a : r & 0xFF, exppsw);
                    /* immediate form on a subset (same ALU, different decoder path) */
                    if ((b & 0x0F) == (a & 0x0F) || b == 0 || b == 0xFF) {
                        uint8_t imm[2] = {(uint8_t)(0x0C + k * 0x10), (uint8_t)b};
                        rl78_set_reg(cpu, RL78_REG_A, a);
                        rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                        exec_bytes(imm, 2);
                        CHECKF(rl78_get_reg(cpu, RL78_REG_A) == (k == 4 ? a : (r & 0xFF)) && rl78_get_reg(cpu, RL78_REG_PSW) == exppsw,
                               "imm op%u A=%02X #%02X", k, a, b);
                    }
                }
    }
    printf("  alu8 exhaustive: %lu cases\n", n);
}

static void test_incdec(void)
{
    unsigned a, d, cin;
    for (d = 0; d < 2; d++)
        for (a = 0; a < 256; a++)
            for (cin = 0; cin < 2; cin++) {
                uint8_t op[1] = {(uint8_t)(d ? 0x93 : 0x83)}; /* INC B / DEC B */
                unsigned cy, ac, r, psw0 = psw_with(cin ? RL78_PSW_CY : 0);
                unsigned exp;
                if (d) r = ref_sub(a, 1, 0, 8, &cy, &ac);
                else r = ref_add(a, 1, 0, 8, &cy, &ac);
                exp = (psw0 & ~(RL78_PSW_Z | RL78_PSW_AC)) | ((r & 0xFF) ? 0 : RL78_PSW_Z) | (ac ? RL78_PSW_AC : 0);
                rl78_set_reg(cpu, RL78_REG_B, a);
                rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                exec_bytes(op, 1);
                CHECKF(rl78_get_reg(cpu, RL78_REG_B) == (r & 0xFF) && rl78_get_reg(cpu, RL78_REG_PSW) == exp,
                       "%s %02X: got %02X PSW %02X exp %02X PSW %02X", d ? "DEC" : "INC", a,
                       (unsigned)rl78_get_reg(cpu, RL78_REG_B), (unsigned)rl78_get_reg(cpu, RL78_REG_PSW), r & 0xFF, exp);
            }
}

static void test_shift8(void)
{
    unsigned a, cnt, cin, kind;
    /* kind: 0 SHL A,1 SHR A,2 SAR A,3 SHL B,4 SHL C */
    for (kind = 0; kind < 5; kind++)
        for (cnt = 1; cnt <= 7; cnt++)
            for (a = 0; a < 256; a++)
                for (cin = 0; cin < 2; cin++) {
                    static const uint8_t lo[5] = {0x09, 0x0A, 0x0B, 0x08, 0x07};
                    uint8_t op[2] = {0x31, (uint8_t)((cnt << 4) | lo[kind])};
                    unsigned v = a, cy = cin, i, psw0 = psw_with((cin ? RL78_PSW_CY : 0) | (rnd() & (RL78_PSW_Z | RL78_PSW_AC)));
                    rl78_reg reg = kind == 3 ? RL78_REG_B : kind == 4 ? RL78_REG_C : RL78_REG_A;
                    for (i = 0; i < cnt; i++) {
                        if (kind == 1) { cy = v & 1; v = v >> 1; }
                        else if (kind == 2) { cy = v & 1; v = (v >> 1) | (v & 0x80); }
                        else { cy = (v >> 7) & 1; v = (v << 1) & 0xFF; }
                    }
                    rl78_set_reg(cpu, reg, a);
                    rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                    exec_bytes(op, 2);
                    CHECKF(rl78_get_reg(cpu, reg) == v && rl78_get_reg(cpu, RL78_REG_PSW) == ((psw0 & ~RL78_PSW_CY) | cy),
                           "shift kind %u cnt %u a %02X", kind, cnt, a);
                }
}

static void test_shift16(void)
{
    unsigned a, cnt, kind;
    unsigned long n = 0;
    /* kind: 0 SHLW AX, 1 SHLW BC, 2 SHRW AX, 3 SARW AX */
    for (kind = 0; kind < 4; kind++)
        for (cnt = 1; cnt <= 15; cnt++)
            for (a = 0; a < 65536; a += (kind == 0 ? 1 : 7)) {
                static const uint8_t lo[4] = {0x0D, 0x0C, 0x0E, 0x0F};
                uint8_t op[2] = {0x31, (uint8_t)((cnt << 4) | lo[kind])};
                unsigned v = a, cy = rnd() & 1, i, psw0 = psw_with((cy ? RL78_PSW_CY : 0) | (rnd() & (RL78_PSW_Z | RL78_PSW_AC)));
                rl78_reg reg = kind == 1 ? RL78_REG_BC : RL78_REG_AX;
                for (i = 0; i < cnt; i++) {
                    if (kind == 2) { cy = v & 1; v >>= 1; }
                    else if (kind == 3) { cy = v & 1; v = (v >> 1) | (v & 0x8000); }
                    else { cy = (v >> 15) & 1; v = (v << 1) & 0xFFFF; }
                }
                rl78_set_reg(cpu, reg, a);
                rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                exec_bytes(op, 2);
                n++;
                CHECKF(rl78_get_reg(cpu, reg) == v && rl78_get_reg(cpu, RL78_REG_PSW) == ((psw0 & ~RL78_PSW_CY) | cy),
                       "shiftw kind %u cnt %u a %04X: got %04X PSW %02X", kind, cnt, a,
                       (unsigned)rl78_get_reg(cpu, reg), (unsigned)rl78_get_reg(cpu, RL78_REG_PSW));
            }
    printf("  16-bit shifts: %lu cases\n", n);
}

static void test_rotate(void)
{
    unsigned a, cin, kind;
    /* 0 ROR, 1 ROL, 2 RORC, 3 ROLC */
    for (kind = 0; kind < 4; kind++)
        for (a = 0; a < 256; a++)
            for (cin = 0; cin < 2; cin++) {
                static const uint8_t opc[4] = {0xDB, 0xEB, 0xFB, 0xDC};
                uint8_t op[2] = {0x61, opc[kind]};
                unsigned v, cy, psw0 = psw_with(cin ? RL78_PSW_CY : 0);
                unsigned b0 = a & 1, b7 = (a >> 7) & 1;
                switch (kind) {
                case 0: v = (a >> 1) | (b0 << 7); cy = b0; break;
                case 1: v = ((a << 1) & 0xFF) | b7; cy = b7; break;
                case 2: v = (a >> 1) | (cin << 7); cy = b0; break;
                default: v = ((a << 1) & 0xFF) | cin; cy = b7; break;
                }
                rl78_set_reg(cpu, RL78_REG_A, a);
                rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                exec_bytes(op, 2);
                CHECKF(rl78_get_reg(cpu, RL78_REG_A) == v && rl78_get_reg(cpu, RL78_REG_PSW) == ((psw0 & ~RL78_PSW_CY) | cy),
                       "rotate %u a %02X cy %u", kind, a, cin);
            }
    for (kind = 0; kind < 2; kind++)
        for (a = 0; a < 65536; a++) {
            uint8_t op[2] = {0x61, (uint8_t)(kind ? 0xFE : 0xEE)};
            unsigned c0 = a & 1; /* vary carry-in with the value */
            unsigned psw0 = psw_with(c0 ? RL78_PSW_CY : 0);
            unsigned v = ((a << 1) & 0xFFFF) | c0, cy = (a >> 15) & 1;
            rl78_reg reg = kind ? RL78_REG_BC : RL78_REG_AX;
            rl78_set_reg(cpu, reg, a);
            rl78_set_reg(cpu, RL78_REG_PSW, psw0);
            exec_bytes(op, 2);
            CHECKF(rl78_get_reg(cpu, reg) == v && rl78_get_reg(cpu, RL78_REG_PSW) == ((psw0 & ~RL78_PSW_CY) | cy),
                   "ROLWC %u %04X", kind, a);
        }
}

static void one_w(unsigned kind, unsigned a, unsigned b)
{
    /* kind 0 ADDW AX,BC 1 SUBW AX,BC 2 CMPW AX,BC */
    static const uint8_t opc[3] = {0x03, 0x23, 0x43};
    uint8_t op[1] = {opc[kind]};
    unsigned cy, ac, r, psw0 = psw_with(rnd() & FLAGS), exp;
    if (kind == 0) r = ref_add(a, b, 0, 16, &cy, &ac);
    else r = ref_sub(a, b, 0, 16, &cy, &ac);
    exp = (psw0 & ~FLAGS) | (r ? 0 : RL78_PSW_Z) | (ac ? RL78_PSW_AC : 0) | (cy ? RL78_PSW_CY : 0);
    rl78_set_reg(cpu, RL78_REG_AX, a);
    rl78_set_reg(cpu, RL78_REG_BC, b);
    rl78_set_reg(cpu, RL78_REG_PSW, psw0);
    exec_bytes(op, 1);
    CHECKF(rl78_get_reg(cpu, RL78_REG_AX) == (kind == 2 ? a : r) && rl78_get_reg(cpu, RL78_REG_PSW) == exp,
           "W op%u %04X,%04X: got %04X PSW %02X exp %04X %02X", kind, a, b,
           (unsigned)rl78_get_reg(cpu, RL78_REG_AX), (unsigned)rl78_get_reg(cpu, RL78_REG_PSW), r, exp);
}

static void test_addw(void)
{
    static const unsigned edge[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x0F, 0x10, 0xFFF0};
    unsigned kind, i, j;
    unsigned long n;
    for (kind = 0; kind < 3; kind++) {
        for (i = 0; i < sizeof edge / sizeof edge[0]; i++)
            for (j = 0; j < sizeof edge / sizeof edge[0]; j++) one_w(kind, edge[i], edge[j]);
        for (n = 0; n < 300000; n++) one_w(kind, rnd() & 0xFFFF, rnd() & 0xFFFF);
    }
}

static void test_mul(void)
{
    unsigned a, x;
    unsigned long n;
    for (a = 0; a < 256; a++)
        for (x = 0; x < 256; x++) {
            uint8_t op[1] = {0xD6};
            unsigned psw0 = psw_with(rnd() & FLAGS);
            rl78_set_reg(cpu, RL78_REG_A, a);
            rl78_set_reg(cpu, RL78_REG_X, x);
            rl78_set_reg(cpu, RL78_REG_PSW, psw0);
            exec_bytes(op, 1);
            CHECKF(rl78_get_reg(cpu, RL78_REG_AX) == (unsigned)ref_mul(a, x) && rl78_get_reg(cpu, RL78_REG_PSW) == psw0,
                   "MULU %02X*%02X", a, x);
        }
    for (n = 0; n < 200000; n++) {
        unsigned ax = rnd() & 0xFFFF, bc = rnd() & 0xFFFF, sgn = n & 1;
        uint8_t op[3] = {0xCE, 0xFB, (uint8_t)(sgn ? 0x02 : 0x01)};
        uint64_t p;
        unsigned psw0 = psw_with(rnd() & FLAGS);
        if (n < 16) { ax = (n & 2) ? 0x8000 : 0xFFFF; bc = (n & 4) ? 0x8000 : 0xFFFF; }
        if (sgn) {
            /* signed via magnitude multiply and sign fix-up */
            unsigned ma = (ax & 0x8000) ? (0x10000 - ax) : ax, mb = (bc & 0x8000) ? (0x10000 - bc) : bc;
            p = ref_mul(ma, mb);
            if (((ax ^ bc) & 0x8000) && p) p = (~p + 1);
        } else {
            p = ref_mul(ax, bc);
        }
        p &= 0xFFFFFFFFu;
        rl78_set_reg(cpu, RL78_REG_AX, ax);
        rl78_set_reg(cpu, RL78_REG_BC, bc);
        rl78_set_reg(cpu, RL78_REG_PSW, psw0);
        exec_bytes(op, 3);
        CHECKF(rl78_get_reg(cpu, RL78_REG_AX) == (p & 0xFFFF) && rl78_get_reg(cpu, RL78_REG_BC) == (p >> 16) &&
               rl78_get_reg(cpu, RL78_REG_PSW) == psw0,
               "%s %04X*%04X", sgn ? "MULH" : "MULHU", ax, bc);
    }
}

static void test_div(void)
{
    unsigned long n;
    for (n = 0; n < 300000; n++) {
        unsigned ax = rnd() & 0xFFFF, de = rnd() & ((n & 3) == 0 ? 0xFF : 0xFFFF);
        uint8_t op[3] = {0xCE, 0xFB, 0x03};
        uint32_t q, r;
        unsigned psw0 = psw_with(rnd() & FLAGS);
        if (n < 8) de = 0; /* divide by zero: AX=FFFFH, DE=dividend (manual 6.5 DIVHU) */
        if (n >= 8 && n < 16) { ax = 0xFFFF; de = (unsigned)n - 7; }
        if (de == 0) { q = 0xFFFF; r = ax; }
        else ref_div(ax, de, &q, &r);
        rl78_set_reg(cpu, RL78_REG_AX, ax);
        rl78_set_reg(cpu, RL78_REG_DE, de);
        rl78_set_reg(cpu, RL78_REG_PSW, psw0);
        exec_bytes(op, 3);
        CHECKF(rl78_get_reg(cpu, RL78_REG_AX) == q && rl78_get_reg(cpu, RL78_REG_DE) == r && rl78_get_reg(cpu, RL78_REG_PSW) == psw0,
               "DIVHU %04X/%04X", ax, de);
    }
    for (n = 0; n < 300000; n++) {
        uint32_t a = rnd(), d = rnd() >> (rnd() & 31);
        uint8_t op[3] = {0xCE, 0xFB, 0x0B};
        uint32_t q, r;
        if (n < 8) d = 0;
        if (n >= 8 && n < 16) { a = 0xFFFFFFFFu; d = (uint32_t)n - 7; }
        if (d == 0) { q = 0xFFFFFFFFu; r = a; }
        else ref_div(a, d, &q, &r);
        rl78_set_reg(cpu, RL78_REG_AX, a & 0xFFFF);
        rl78_set_reg(cpu, RL78_REG_BC, a >> 16);
        rl78_set_reg(cpu, RL78_REG_DE, d & 0xFFFF);
        rl78_set_reg(cpu, RL78_REG_HL, d >> 16);
        exec_bytes(op, 3);
        CHECKF(rl78_get_reg(cpu, RL78_REG_AX) == (q & 0xFFFF) && rl78_get_reg(cpu, RL78_REG_BC) == (q >> 16) &&
               rl78_get_reg(cpu, RL78_REG_DE) == (r & 0xFFFF) && rl78_get_reg(cpu, RL78_REG_HL) == (r >> 16),
               "DIVWU %08X/%08X", a, d);
    }
}

static void test_mac(void)
{
    unsigned long n;
    for (n = 0; n < 200000; n++) {
        unsigned ax = rnd() & 0xFFFF, bc = rnd() & 0xFFFF, sgn = n & 1;
        uint32_t acc = rnd();
        uint8_t op[3] = {0xCE, 0xFB, (uint8_t)(sgn ? 0x06 : 0x05)};
        uint8_t m[4];
        unsigned psw0 = psw_with(rnd() & FLAGS), exp;
        uint32_t res;
        if (n < 32) { acc = (n & 4) ? 0x7FFFFFFFu : 0x80000000u; ax = (n & 8) ? 0x7FFF : 0x8000; bc = (n & 16) ? 0x7FFF : 0x8000; }
        if (sgn) {
            /* 33-bit two's complement arithmetic done by hand */
            int64_t p = (int64_t)(int16_t)ax * (int64_t)(int16_t)bc;
            int64_t s = (int64_t)(int32_t)acc + p;
            int ovf = (s > 2147483647LL) || (s < -2147483648LL);
            res = (uint32_t)s;
            exp = (psw0 & ~(RL78_PSW_AC | RL78_PSW_CY)) | (ovf ? RL78_PSW_CY : 0) | ((res >> 31) ? RL78_PSW_AC : 0);
        } else {
            unsigned cy, ac;
            uint32_t p = (uint32_t)ref_mul(ax, bc);
            uint32_t lo = ref_add(acc & 0xFFFF, p & 0xFFFF, 0, 16, &cy, &ac);
            uint32_t hi = ref_add(acc >> 16, p >> 16, cy, 16, &cy, &ac);
            res = lo | (hi << 16);
            exp = (psw0 & ~(RL78_PSW_AC | RL78_PSW_CY)) | (cy ? RL78_PSW_CY : 0);
        }
        m[0] = (uint8_t)acc; m[1] = (uint8_t)(acc >> 8); m[2] = (uint8_t)(acc >> 16); m[3] = (uint8_t)(acc >> 24);
        rl78_write16(cpu, 0xFFFF0, (uint16_t)(acc & 0xFFFF));
        rl78_write16(cpu, 0xFFFF2, (uint16_t)(acc >> 16));
        rl78_set_reg(cpu, RL78_REG_AX, ax);
        rl78_set_reg(cpu, RL78_REG_BC, bc);
        rl78_set_reg(cpu, RL78_REG_PSW, psw0);
        exec_bytes(op, 3);
        CHECKF(rl78_read16(cpu, 0xFFFF0) == (res & 0xFFFF) && rl78_read16(cpu, 0xFFFF2) == (res >> 16) &&
               rl78_get_reg(cpu, RL78_REG_PSW) == exp && rl78_get_reg(cpu, RL78_REG_AX) == ax,
               "%s acc %08X + %04X*%04X: got %04X%04X PSW %02X exp %08X %02X", sgn ? "MACH" : "MACHU", acc, ax, bc,
               rl78_read16(cpu, 0xFFFF2), rl78_read16(cpu, 0xFFFF0), (unsigned)rl78_get_reg(cpu, RL78_REG_PSW), res, exp);
        (void)m;
    }
}

static void test_cmps_movs_cmp0(void)
{
    unsigned x, v, a;
    for (x = 0; x < 256; x++)
        for (v = 0; v < 256; v++)
            for (a = 0; a < 2; a++) {
                /* CMPS X, [HL+0]: Z = (X-v==0); CY = result!=0 || A==0 || X==0; AC = borrow 4->3 */
                uint8_t op[3] = {0x61, 0xDE, 0x00};
                unsigned cy, ac, r = ref_sub(x, v, 0, 8, &cy, &ac), av = a ? 0x5A : 0x00;
                unsigned psw0 = psw_with(rnd() & FLAGS);
                unsigned exp = (psw0 & ~FLAGS) | ((r & 0xFF) ? 0 : RL78_PSW_Z) | (ac ? RL78_PSW_AC : 0) |
                               (((r & 0xFF) != 0 || av == 0 || x == 0) ? RL78_PSW_CY : 0);
                uint8_t mv = (uint8_t)v;
                rl78_load(cpu, 0xFFE80, &mv, 1);
                rl78_set_reg(cpu, RL78_REG_HL, 0xFE80);
                rl78_set_reg(cpu, RL78_REG_X, x);
                rl78_set_reg(cpu, RL78_REG_A, av);
                rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                exec_bytes(op, 3);
                CHECKF(rl78_get_reg(cpu, RL78_REG_PSW) == exp && rl78_get_reg(cpu, RL78_REG_X) == x,
                       "CMPS X=%02X v=%02X A=%02X: PSW %02X exp %02X", x, v, av, (unsigned)rl78_get_reg(cpu, RL78_REG_PSW), exp);
            }
    for (x = 0; x < 256; x++)
        for (a = 0; a < 256; a += 17) {
            /* MOVS [HL+1], X: Z = (X==0); CY = (A==0 || X==0); AC unchanged */
            uint8_t op[3] = {0x61, 0xCE, 0x01};
            unsigned psw0 = psw_with(rnd() & FLAGS);
            unsigned exp = (psw0 & ~(RL78_PSW_Z | RL78_PSW_CY)) | (x ? 0 : RL78_PSW_Z) | ((a == 0 || x == 0) ? RL78_PSW_CY : 0);
            rl78_set_reg(cpu, RL78_REG_HL, 0xFE80);
            rl78_set_reg(cpu, RL78_REG_X, x);
            rl78_set_reg(cpu, RL78_REG_A, a);
            rl78_set_reg(cpu, RL78_REG_PSW, psw0);
            exec_bytes(op, 3);
            CHECKF(rl78_get_reg(cpu, RL78_REG_PSW) == exp && rl78_peek8(cpu, 0xFFE81) == x, "MOVS X=%02X A=%02X", x, a);
        }
    for (x = 0; x < 256; x++) {
        uint8_t op[1] = {0xD3}; /* CMP0 B */
        unsigned psw0 = psw_with(rnd() & FLAGS);
        rl78_set_reg(cpu, RL78_REG_B, x);
        rl78_set_reg(cpu, RL78_REG_PSW, psw0);
        exec_bytes(op, 1);
        CHECK_EQ(rl78_get_reg(cpu, RL78_REG_PSW), (psw0 & ~FLAGS) | (x ? 0 : RL78_PSW_Z));
    }
}

static void test_bitops_a(void)
{
    unsigned a, bit, cin, kind;
    /* map 3 column C..F with A operand, 9..B: MOV1 A.b,CY / SET1 / CLR1 */
    for (kind = 0; kind < 7; kind++)
        for (bit = 0; bit < 8; bit++)
            for (a = 0; a < 256; a += 5)
                for (cin = 0; cin < 2; cin++) {
                    static const uint8_t lo[7] = {0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
                    uint8_t op[2] = {0x71, (uint8_t)(0x80 | (bit << 4) | lo[kind])};
                    unsigned psw0 = psw_with((cin ? RL78_PSW_CY : 0) | (rnd() & (RL78_PSW_Z | RL78_PSW_AC)));
                    unsigned bv = (a >> bit) & 1, expa = a, expcy = cin;
                    switch (kind) {
                    case 0: expa = cin ? (a | (1u << bit)) : (a & ~(1u << bit)); break;
                    case 1: expa = a | (1u << bit); break;
                    case 2: expa = a & ~(1u << bit); break;
                    case 3: expcy = bv; break;
                    case 4: expcy = cin & bv; break;
                    case 5: expcy = cin | bv; break;
                    default: expcy = cin ^ bv; break;
                    }
                    rl78_set_reg(cpu, RL78_REG_A, a);
                    rl78_set_reg(cpu, RL78_REG_PSW, psw0);
                    exec_bytes(op, 2);
                    CHECKF(rl78_get_reg(cpu, RL78_REG_A) == (expa & 0xFF) &&
                           rl78_get_reg(cpu, RL78_REG_PSW) == ((psw0 & ~RL78_PSW_CY) | expcy),
                           "bitop %u bit %u A=%02X CY=%u", kind, bit, a, cin);
                }
}

void suite_alu(void)
{
    rl78_config cfg;
    rl78_default_config(&cfg, RL78_CORE_S3);
    cpu = rl78_create(&cfg);
    rl78_set_reg(cpu, RL78_REG_SP, 0xFE00);
    test_alu8();
    test_incdec();
    test_shift8();
    test_shift16();
    test_rotate();
    test_addw();
    test_mul();
    test_div();
    test_mac();
    test_cmps_movs_cmp0();
    test_bitops_a();
    rl78_destroy(cpu);
}
