/*
 * t_prog.c - small hand-written programs (assembled with tasm) with results
 * worked out by hand: loops, recursion and the stack, far calls, interrupt
 * acceptance and RETI, register banks, ES: table access, mirror reads, a
 * software BCD addition using AC/CY.
 */
#include <string.h>

#include "tcommon.h"

static uint16_t rd16(rl78_cpu *c, uint32_t a) { return (uint16_t)(rl78_peek8(c, a) | (rl78_peek8(c, a + 1) << 8)); }

static void prog_sum(void)
{
    /* sum 1..100 = 5050 = 13BAH */
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  MOVW AX, #0\n"
        "  MOVW BC, #100\n"
        "loop:\n"
        "  ADDW AX, BC\n"
        "  DECW BC\n"
        "  XCHW AX, BC\n"
        "  CMPW AX, #0\n"
        "  XCHW AX, BC\n"
        "  BNZ $loop\n"
        "  MOVW !0FE80H, AX\n"
        "  HALT\n";
    rl78_cpu *c = t_cpu(RL78_CORE_S2);
    rl78_stop_reason r;
    if (t_load(c, src)) return;
    r = t_run(c, 100000);
    CHECK_EQ(r, RL78_STOP_HALT);
    CHECK_EQ(rd16(c, 0xFFE80), 5050);
    /* 4 setup + 100*6 loop + MOVW + HALT */
    CHECK_EQ(rl78_insn_count(c), 3 + 600 + 2);
    /* clocks (S2): 3 setup*1, loop body: ADDW 1, DECW 1, XCHW 1, CMPW 1, XCHW 1,
       BNZ taken 4 (99x) / not taken 2 (1x); MOVW !addr,AX 1; HALT 3 */
    CHECK_EQ(rl78_clock_count(c), 3 + 100 * 5 + 99 * 4 + 2 + 1 + 3);
    rl78_destroy(c);
}

static void prog_fib(void)
{
    /* recursive fib(15) = 610 = 262H; AX = n in, AX = fib(n) out */
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  MOVW AX, #15\n"
        "  CALL !fib\n"
        "  MOVW !0FE80H, AX\n"
        "  HALT\n"
        "fib:\n"
        "  CMPW AX, #2\n"
        "  BC $small\n"           /* n < 2: return n */
        "  PUSH AX\n"             /* save n */
        "  DECW AX\n"
        "  CALL !fib\n"           /* fib(n-1) */
        "  MOVW BC, AX\n"
        "  POP AX\n"
        "  PUSH BC\n"             /* save fib(n-1) */
        "  DECW AX\n"
        "  DECW AX\n"
        "  CALL !fib\n"           /* fib(n-2) */
        "  POP BC\n"
        "  ADDW AX, BC\n"
        "small:\n"
        "  RET\n";
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    if (t_load(c, src)) return;
    CHECK_EQ(t_run(c, 1000000), RL78_STOP_HALT);
    CHECK_EQ(rd16(c, 0xFFE80), 610);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFE00);
    rl78_destroy(c);
}

static void prog_far(void)
{
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  CALL !!23456H\n"
        "  MOV !0FE81H, A\n"
        "  MOV CS, #2\n"
        "  MOVW HL, #3456H\n"
        "  CALL HL\n"             /* CS:HL = 23456H again */
        "  MOV !0FE82H, A\n"
        "  BR !!30000H\n"
        ".org 23456H\n"
        "  INC 0FE20H\n"
        "  MOV A, 0FE20H\n"
        "  RET\n"
        ".org 30000H\n"
        "  MOV A, #77H\n"
        "  MOV !0FE83H, A\n"
        "  HALT\n";
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    if (t_load(c, src)) return;
    rl78_write8(c, 0xFFE20, 0);
    CHECK_EQ(t_run(c, 1000), RL78_STOP_HALT);
    CHECK_EQ(rl78_peek8(c, 0xFFE81), 1);
    CHECK_EQ(rl78_peek8(c, 0xFFE82), 2);
    CHECK_EQ(rl78_peek8(c, 0xFFE83), 0x77);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x30007);
    rl78_destroy(c);
}

static void prog_interrupt(void)
{
    /* main loop counts in bank 0; ISR (vector 0004H) switches to bank 1,
       records PSW and SP, counts, returns with RETI. */
    static const char *src =
        ".org 0\n.dw start\n.dw isr\n.dw isr2\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  MOVW AX, #0\n"
        "  EI\n"
        "loop:\n"
        "  INCW AX\n"
        "  BR $loop\n"
        "isr:\n"
        "  SEL RB1\n"
        "  MOV A, PSW\n"
        "  MOV !0FE90H, A\n"
        "  MOVW AX, SP\n"
        "  MOVW !0FE92H, AX\n"
        "  INC !0FE94H\n"
        "  RETI\n"
        "isr2:\n"                 /* level-0 ISR: enables nesting test */
        "  INC !0FE95H\n"
        "  RETI\n";
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    uint32_t pc_before;
    unsigned ax_before;
    if (t_load(c, src)) return;
    rl78_write8(c, 0xFFE94, 0);
    rl78_write8(c, 0xFFE95, 0);
    CHECK_EQ(t_run(c, 50), RL78_STOP_LIMIT);
    pc_before = rl78_get_reg(c, RL78_REG_PC);
    ax_before = rl78_get_reg(c, RL78_REG_AX);
    CHECK(rl78_request_interrupt(c, 0x0002, 1) == 0);
    CHECK_EQ(t_run(c, 1), RL78_STOP_LIMIT); /* accepted, then SEL RB1 executed */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFDFC);
    /* pushed PSW: IE=1, ISP=3 (reset), bank 0 */
    CHECK_EQ(rl78_peek8(c, 0xFFDFF), 0x86);
    CHECK_EQ(rl78_peek8(c, 0xFFDFC) | (rl78_peek8(c, 0xFFDFD) << 8) | (rl78_peek8(c, 0xFFDFE) << 16), pc_before);
    CHECK_EQ(t_run(c, 6), RL78_STOP_LIMIT); /* through RETI */
    /* inside: IE=0, ISP = level-1 = 0, RBS = 1 -> PSW 08H */
    CHECK_EQ(rl78_peek8(c, 0xFFE90), 0x08);
    CHECK_EQ(rd16(c, 0xFFE92), 0xFDFC);
    CHECK_EQ(rl78_peek8(c, 0xFFE94), 1);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), pc_before);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PSW), 0x86);   /* restored, bank 0 */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_AX), ax_before);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFE00);
    /* masked: DI -> the request stays pending */
    rl78_set_reg(c, RL78_REG_PSW, 0x06);
    rl78_request_interrupt(c, 0x0002, 1);
    CHECK_EQ(t_run(c, 20), RL78_STOP_LIMIT);
    CHECK_EQ(rl78_pending_interrupts(c), 1);
    CHECK_EQ(rl78_peek8(c, 0xFFE94), 1);
    /* EI: acknowledged only after the instruction following EI */
    {
        uint8_t ei[3] = {0x71, 0x7A, 0xFA};
        uint8_t nop2[2] = {0x00, 0x00};
        rl78_load(c, 0x8000, ei, 3);
        rl78_load(c, 0x8003, nop2, 2);
        rl78_set_reg(c, RL78_REG_PC, 0x8000);
        CHECK_EQ(t_run(c, 1), RL78_STOP_LIMIT); /* EI */
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x8003);
        CHECK_EQ(rl78_pending_interrupts(c), 1);
        CHECK_EQ(t_run(c, 1), RL78_STOP_LIMIT); /* NOP after EI still executes */
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x8004);
        CHECK_EQ(t_run(c, 1), RL78_STOP_LIMIT); /* now acknowledged + first ISR insn */
        CHECK_EQ(rl78_pending_interrupts(c), 0);
        CHECK_EQ(rd16(c, 0xFFDFC), 0x8004);
    }
    rl78_destroy(c);
}

static void prog_priority(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    static const char *src =
        ".org 0\n.dw start\n.dw isr\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  EI\n"
        "loop:\n"
        "  BR $loop\n"
        "isr:\n"
        "  EI\n"                  /* allow nesting */
        "  NOP\n"
        "  NOP\n"
        "  NOP\n"
        "  DI\n"
        "  RETI\n";
    if (t_load(c, src)) return;
    t_run(c, 10);
    /* level 2 accepted: ISP becomes 1 */
    rl78_request_interrupt(c, 0x0002, 2);
    t_run(c, 2); /* accept + EI + NOP? (EI is a hold insn) */
    CHECK_EQ((rl78_get_reg(c, RL78_REG_PSW) >> 1) & 3, 1);
    /* a level-3 request is lower than ISP: stays pending */
    rl78_request_interrupt(c, 0x0002, 3);
    t_run(c, 2);
    CHECK_EQ(rl78_pending_interrupts(c), 1);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFDFC);
    /* a level-0 request nests */
    rl78_request_interrupt(c, 0x0002, 0);
    t_run(c, 1);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFDF8);
    CHECK_EQ((rl78_get_reg(c, RL78_REG_PSW) >> 1) & 3, 0);
    CHECK_EQ(rl78_pending_interrupts(c), 1); /* level 3 still waiting */
    t_run(c, 200);
    CHECK_EQ(rl78_pending_interrupts(c), 0); /* accepted after the nested ISRs returned */
    rl78_destroy(c);
}

static void prog_halt_wake(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S2);
    static const char *src =
        ".org 0\n.dw start\n.dw isr\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  EI\n"
        "  HALT\n"
        "  MOV A, #1\n"
        "  HALT\n"
        "isr:\n"
        "  MOV X, #5\n"
        "  RETI\n";
    if (t_load(c, src)) return;
    CHECK_EQ(t_run(c, 100), RL78_STOP_HALT);
    CHECK_EQ(rl78_is_halted(c), 1);
    CHECK_EQ(t_run(c, 100), RL78_STOP_HALT); /* still halted, nothing executed */
    rl78_request_interrupt(c, 0x0002, 3);
    CHECK_EQ(t_run(c, 100), RL78_STOP_HALT);  /* ISR, RETI, MOV A,#1, HALT */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_X), 5);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_A), 1);
    rl78_destroy(c);
}

static void prog_banks(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S2);
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  SEL RB0\n  MOVW AX, #1100H\n"
        "  SEL RB1\n  MOVW AX, #2211H\n"
        "  SEL RB2\n  MOVW AX, #3322H\n"
        "  SEL RB3\n  MOVW AX, #4433H\n"
        "  SEL RB1\n  MOVW BC, AX\n"
        "  HALT\n";
    if (t_load(c, src)) return;
    CHECK_EQ(t_run(c, 100), RL78_STOP_HALT);
    CHECK_EQ(rl78_get_bank_reg(c, 0, RL78_REG_AX), 0x1100);
    CHECK_EQ(rl78_get_bank_reg(c, 1, RL78_REG_AX), 0x2211);
    CHECK_EQ(rl78_get_bank_reg(c, 2, RL78_REG_AX), 0x3322);
    CHECK_EQ(rl78_get_bank_reg(c, 3, RL78_REG_AX), 0x4433);
    CHECK_EQ(rl78_get_bank_reg(c, 1, RL78_REG_BC), 0x2211);
    CHECK_EQ(rd16(c, 0xFFEF8), 0x1100); /* bank 0 AX in RAM */
    CHECK_EQ(rd16(c, 0xFFEE0), 0x4433); /* bank 3 AX in RAM */
    rl78_destroy(c);
}

static void prog_es_table_mirror(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    static const char *src =
        ".org 0\n.dw start\n"
        ".org 2100H\n.db 10H, 20H, 30H\n"            /* readable through the mirror */
        ".org 34560H\n.db 1, 2, 3, 4, 5, 6, 7, 8\n"  /* far table */
        ".org 100H\nstart:\n"
        "  MOV ES, #3\n"
        "  MOVW BC, #0\n"
        "  CLRB 0FE21H\n"
        "loop:\n"
        "  MOV A, ES:4560H[BC]\n"
        "  ADD A, 0FE21H\n"
        "  MOV 0FE21H, A\n"
        "  INCW BC\n"
        "  MOVW AX, BC\n"
        "  CMPW AX, #8\n"
        "  BNZ $loop\n"
        "  MOV A, 0FE21H\n"
        "  MOV !0FE80H, A\n"
        "  MOV A, !2101H\n"                          /* F2101H -> 02101H */
        "  MOV !0FE81H, A\n"
        "  MOVW HL, #2100H\n"
        "  MOV A, [HL+2]\n"
        "  MOV !0FE82H, A\n"
        "  HALT\n";
    if (t_load(c, src)) return;
    CHECK_EQ(t_run(c, 1000), RL78_STOP_HALT);
    CHECK_EQ(rl78_peek8(c, 0xFFE80), 36);
    CHECK_EQ(rl78_peek8(c, 0xFFE81), 0x20);
    CHECK_EQ(rl78_peek8(c, 0xFFE82), 0x30);
    rl78_destroy(c);
}

/* BCD addition by software decimal adjust using AC and CY (the reason the
   RL78 has an AC flag; it has no DAA instruction). */
static void prog_bcd(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S2);
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  ADD A, X\n"            /* binary add of two BCD bytes */
        "  MOV1 0FE20H.0, CY\n"   /* keep CY */
        "  MOV C, A\n"            /* keep the binary sum */
        "  MOV B, #0\n"           /* correction */
        "  BT PSW.4, $lowfix\n"   /* AC: carry out of the low digit */
        "  AND A, #0FH\n"
        "  CMP A, #0AH\n"
        "  BC $lowok\n"
        "lowfix:\n"
        "  MOV B, #06H\n"
        "lowok:\n"
        "  MOV A, C\n"
        "  BT 0FE20H.0, $hifix\n"
        "  CMP A, #9AH\n"
        "  BC $hiok\n"
        "hifix:\n"
        "  XCH A, B\n"
        "  OR A, #60H\n"
        "  XCH A, B\n"
        "hiok:\n"
        "  MOV A, C\n"
        "  ADD A, B\n"
        "  HALT\n";
    unsigned a, b;
    if (t_load(c, src)) return;
    (void)src;
    for (a = 0; a < 100; a += 7)
        for (b = 0; b < 100; b += 3) {
            unsigned ba = ((a / 10) << 4) | (a % 10), bb = ((b / 10) << 4) | (b % 10);
            unsigned sum = (a + b) % 100;
            unsigned exp = ((sum / 10) << 4) | (sum % 10);
            rl78_set_reg(c, RL78_REG_PC, 0x100);
            rl78_set_reg(c, RL78_REG_SP, 0xFE00);
            rl78_set_reg(c, RL78_REG_A, ba);
            rl78_set_reg(c, RL78_REG_X, bb);
            rl78_set_reg(c, RL78_REG_PSW, 0x06);
            CHECK_EQ(t_run(c, 100), RL78_STOP_HALT);
            CHECKF(rl78_get_reg(c, RL78_REG_A) == exp, "BCD %02X + %02X = %02X expected %02X", ba, bb,
                   (unsigned)rl78_get_reg(c, RL78_REG_A), exp);
        }
    rl78_destroy(c);
}

void suite_prog(void)
{
    prog_sum();
    prog_fib();
    prog_far();
    prog_interrupt();
    prog_priority();
    prog_halt_wake();
    prog_banks();
    prog_es_table_mirror();
    prog_bcd();
}
