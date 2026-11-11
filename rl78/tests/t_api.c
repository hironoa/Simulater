/*
 * t_api.c - public API behaviour: breakpoints, hooks, interrupt requests from
 * hooks, stop requests, snapshots, HALT/STOP policies, the RL78/G13
 * multiplier/divider SFRs (examples from the RL78/G13 hardware manual
 * R01UH0146 chapter 14 timing diagrams), faults and the disassembler API.
 */
#include <string.h>

#include "tcommon.h"

static const char *loop_src =
    ".org 0\n.dw start\n.org 100H\nstart:\n"
    "  MOVW SP, #0FE00H\n"
    "  MOVW AX, #0\n"
    "again:\n"
    "  CALL !func\n"
    "after:\n"
    "  INCW AX\n"
    "  BR $again\n"
    "func:\n"
    "  MOV ES, #0\n"
    "  MOV B, ES:!func\n"          /* reads its own first byte as data */
    "  RET\n";

static void test_breakpoints(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    long func, after;
    uint64_t n0;
    if (t_load(c, loop_src)) return;
    func = tasm_label("func");
    after = tasm_label("after");
    CHECK(rl78_add_breakpoint(c, (uint32_t)func) == 0);
    CHECK_EQ(rl78_run(c, 1000), RL78_STOP_BREAKPOINT);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), (unsigned long)func);
    CHECK_EQ(rl78_insn_count(c), 3); /* MOVW, MOVW, CALL; the breakpoint insn is not executed */
    /* data reads of the patched byte see the original */
    CHECK_EQ(rl78_peek8(c, (uint32_t)func), 0x41);
    CHECK_EQ(rl78_read8(c, (uint32_t)func), 0x41);
    /* resume: the original instruction executes, then MOV B,ES:!func reads 41H */
    CHECK(rl78_add_breakpoint(c, (uint32_t)after) == 0);
    CHECK_EQ(rl78_run(c, 1000), RL78_STOP_BREAKPOINT);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), (unsigned long)after);  /* "run to return address" */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_B), 0x41);
    n0 = rl78_insn_count(c);
    CHECK_EQ(rl78_run(c, 1000), RL78_STOP_BREAKPOINT);           /* next lap stops at func again */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), (unsigned long)func);
    CHECK_EQ(rl78_insn_count(c) - n0, 3);                        /* INCW, BR, CALL */
    CHECK_EQ(rl78_remove_breakpoint(c, (uint32_t)func), 0);
    CHECK_EQ(rl78_remove_breakpoint(c, (uint32_t)func), -1);
    CHECK_EQ(rl78_memory(c)[func], 0x41);                        /* byte restored */
    CHECK_EQ(rl78_run(c, 1000), RL78_STOP_BREAKPOINT);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), (unsigned long)after);
    rl78_clear_breakpoints(c);
    CHECK_EQ(rl78_run(c, 100), RL78_STOP_LIMIT);
    /* breakpoint on an ES:-prefixed instruction; starting a run at a
       breakpoint executes it (resume semantics) */
    {
        uint8_t code[] = {0x00, 0x11, 0x8F, 0x80, 0xFE, 0x00}; /* NOP ; MOV A, ES:!0FE80H ; NOP */
        uint8_t v = 0x5C;
        rl78_load(c, 0x8FFF, code, sizeof code);
        rl78_load(c, 0xFFE80, &v, 1);
        rl78_set_reg(c, RL78_REG_ES, 0x0F);
        rl78_set_reg(c, RL78_REG_PC, 0x8FFF);
        rl78_add_breakpoint(c, 0x9000);
        CHECK_EQ(rl78_run(c, 10), RL78_STOP_BREAKPOINT);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x9000);
        CHECK_EQ(rl78_step(c), RL78_STOP_LIMIT);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_A), 0x5C);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x9004);
        rl78_clear_breakpoints(c);
    }
    /* breakpoint in RAM code that the program overwrites */
    {
        uint8_t code[] = {0xCF, 0x01, 0xFD, 0x51,  /* MOV !0FD01H, #51H  (patch next insn: MOV A,#xx) */
                          0x00, 0x00};
        uint8_t ram[] = {0x00, 0x00, 0x00};      /* at FFD00: NOP NOP NOP */
        rl78_load(c, 0x9100, code, 4);
        rl78_load(c, 0x9104, code + 4, 2);
        rl78_load(c, 0xFFD00, ram, 3);
        rl78_add_breakpoint(c, 0xFFD01);
        rl78_set_reg(c, RL78_REG_PC, 0x9100);
        CHECK_EQ(rl78_step(c), RL78_STOP_LIMIT);
        CHECK_EQ(rl78_peek8(c, 0xFFD01), 0x51);   /* write went to the saved byte */
        rl78_set_reg(c, RL78_REG_PC, 0xFFD00);
        CHECK_EQ(rl78_run(c, 10), RL78_STOP_BREAKPOINT);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0xFFD01);
        rl78_clear_breakpoints(c);
        CHECK_EQ(rl78_memory(c)[0xFFD01], 0x51);
    }
    /* no fixed limit: 3000 breakpoints on consecutive NOPs, each one hit */
    {
        static uint8_t nops[3001];
        int i, ok = 0, hits = 0;
        memset(nops, 0, sizeof nops);
        rl78_load(c, 0x4FFF, nops, sizeof nops);
        for (i = 0; i < 3000; i++)
            if (rl78_add_breakpoint(c, 0x5000u + (uint32_t)i) == 0) ok++;
        CHECK_EQ(ok, 3000);
        CHECK_EQ(rl78_breakpoint_count(c), 3000);
        rl78_set_reg(c, RL78_REG_PC, 0x4FFF);
        for (i = 0; i < 3000; i++)
            if (rl78_run(c, 10) == RL78_STOP_BREAKPOINT && rl78_get_reg(c, RL78_REG_PC) == 0x5000u + (uint32_t)i) hits++;
        CHECK_EQ(hits, 3000);
        CHECK_EQ(rl78_peek8(c, 0x5123), 0x00);         /* original bytes visible */
        CHECK_EQ(rl78_remove_breakpoint(c, 0x5123), 0);
        CHECK_EQ(rl78_breakpoint_count(c), 2999);
        rl78_clear_breakpoints(c);
        CHECK_EQ(rl78_breakpoint_count(c), 0);
        for (i = 0, ok = 0; i < 3000; i++) ok += rl78_memory(c)[0x5000 + i] == 0x00;
        CHECK_EQ(ok, 3000);
    }
    rl78_destroy(c);
}

/* ---- hooks ---- */
typedef struct {
    int reads, writes, last_size;
    uint32_t last_addr, last_val;
    int irq_on_write, stop_on_write;
} hookctx;

static uint32_t h_read(void *ctx, rl78_cpu *cpu, uint32_t addr, int size)
{
    hookctx *h = (hookctx *)ctx;
    (void)cpu;
    h->reads++;
    h->last_size = size;
    h->last_addr = addr;
    return size == 2 ? 0xBEEF : 0x40u + (addr & 0xF);
}

static void h_write(void *ctx, rl78_cpu *cpu, uint32_t addr, uint32_t val, int size)
{
    hookctx *h = (hookctx *)ctx;
    h->writes++;
    h->last_size = size;
    h->last_addr = addr;
    h->last_val = val;
    if (h->irq_on_write) rl78_request_interrupt(cpu, 0x0004, 0);
    if (h->stop_on_write) rl78_request_stop(cpu);
}

static void test_hooks(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S2);
    hookctx h;
    static const char *src =
        ".org 0\n.dw start\n.dw 0\n.dw isr\n.org 100H\nstart:\n"
        "  MOVW SP, #0FE00H\n"
        "  MOV A, 0FFF33H\n"        /* hooked read (sfr) */
        "  MOV !0FE80H, A\n"
        "  MOVW AX, 0FFF30H\n"      /* 16-bit hooked read */
        "  MOVW !0FE82H, AX\n"
        "  MOV A, #9AH\n"
        "  MOV 0FFF31H, A\n"        /* hooked write */
        "  MOV A, !0FD10H\n"        /* plain RAM in a page that has a hook */
        "  MOV !0FE84H, A\n"
        "  EI\n"
        "  MOV !0FD08H, A\n"        /* hooked RAM write -> requests interrupt */
        "  NOP\n"
        "  MOV !0FE85H, A\n"
        "  HALT\n"
        "isr:\n"
        "  MOV !0FE86H, #11H\n"
        "  RETI\n";
    memset(&h, 0, sizeof h);
    if (t_load(c, src)) return;
    CHECK(rl78_add_hook(c, 0xFFF30, 0xFFF34, h_read, h_write, &h) == 0);
    CHECK(rl78_add_hook(c, 0xFFD08, 0xFFD09, NULL, h_write, &h) == 0);
    rl78_write8(c, 0xFFD10, 0x77); /* goes to plain memory */
    h.writes = 0;
    h.irq_on_write = 0;
    CHECK_EQ(t_run(c, 7), RL78_STOP_LIMIT);
    CHECK_EQ(rl78_peek8(c, 0xFFE80), 0x43);
    CHECK_EQ(rl78_peek8(c, 0xFFE82) | (rl78_peek8(c, 0xFFE83) << 8), 0xBEEF);
    CHECK_EQ(h.writes, 1);
    CHECK_EQ(h.last_addr, 0xFFF31);
    CHECK_EQ(h.last_val, 0x9A);
    CHECK_EQ(h.last_size, 1);
    h.irq_on_write = 1;
    CHECK_EQ(t_run(c, 100), RL78_STOP_HALT);
    CHECK_EQ(rl78_peek8(c, 0xFFE84), 0x77);
    CHECK_EQ(rl78_peek8(c, 0xFFE86), 0x11);   /* ISR ran */
    CHECK_EQ(rl78_peek8(c, 0xFFE85), 0x77);   /* and returned */
    /* stop request from a hook */
    h.irq_on_write = 0;
    h.stop_on_write = 1;
    {
        uint8_t code[] = {0x9E, 0x31, 0x00, 0x00, 0x00}; /* MOV 0FFF31H, A ; NOP... */
        rl78_load(c, 0x9000, code, sizeof code);
        rl78_set_reg(c, RL78_REG_PC, 0x9000);
        CHECK_EQ(rl78_run(c, 100), RL78_STOP_REQUEST);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x9002);
    }
    rl78_clear_hooks(c);
    rl78_destroy(c);
}

static void test_snapshot(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    rl78_snapshot *s;
    uint8_t m0[64];
    if (t_load(c, loop_src)) return;
    t_run(c, 37);
    rl78_add_breakpoint(c, (uint32_t)tasm_label("func"));
    s = rl78_snapshot_create(c);
    {
        uint32_t pc = rl78_get_reg(c, RL78_REG_PC);
        unsigned ax = rl78_get_reg(c, RL78_REG_AX), sp = rl78_get_reg(c, RL78_REG_SP), psw = rl78_get_reg(c, RL78_REG_PSW);
        uint64_t ic = rl78_insn_count(c), cc = rl78_clock_count(c);
        rl78_peek(c, 0xFFDE0, m0, sizeof m0);
        rl78_run(c, 1000);
        rl78_run(c, 1000);
        rl78_set_reg(c, RL78_REG_AX, 0x5555);
        rl78_write8(c, 0xFFDF0, 0xAA);
        rl78_snapshot_restore(c, s);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), pc);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_AX), ax);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), sp);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PSW), psw);
        CHECK_EQ(rl78_insn_count(c), ic);
        CHECK_EQ(rl78_clock_count(c), cc);
        {
            uint8_t m1[64];
            rl78_peek(c, 0xFFDE0, m1, sizeof m1);
            CHECK(memcmp(m0, m1, sizeof m0) == 0);
        }
        /* breakpoint still active after restore */
        CHECK_EQ(rl78_run(c, 1000), RL78_STOP_BREAKPOINT);
    }
    rl78_snapshot_free(s);
    rl78_destroy(c);
}

static void test_sleep_policy(void)
{
    rl78_config cfg;
    rl78_cpu *c;
    static const char *src = ".org 0\n.dw start\n.org 100H\nstart:\n  HALT\n  STOP\n  MOV A, #3\n  HALT\n";
    rl78_default_config(&cfg, RL78_CORE_S2);
    cfg.halt_policy = RL78_SLEEP_IGNORE;
    cfg.stop_policy = RL78_SLEEP_RETURN;
    c = rl78_create(&cfg);
    if (t_load(c, src)) return;
    CHECK_EQ(rl78_run(c, 100), RL78_STOP_STOPMODE);
    CHECK_EQ(rl78_is_halted(c), 2);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x104);
    rl78_request_interrupt(c, 0x0002, 3); /* IE=0: releases STOP, not acknowledged */
    CHECK_EQ(rl78_run(c, 2), RL78_STOP_LIMIT);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_A), 3);
    CHECK_EQ(rl78_pending_interrupts(c), 1);
    rl78_cancel_interrupts(c);
    rl78_destroy(c);
}

/* G13 multiplier/divider: values from the timing diagrams of R01UH0146
   figures 14-6 .. 14-10 */
static void test_mdu(void)
{
    rl78_config cfg;
    rl78_cpu *c;
    rl78_default_config(&cfg, RL78_CORE_S2);
    cfg.mdu = 1;
    c = rl78_create(&cfg);
#define MDAL 0xFFFF0
#define MDAH 0xFFFF2
#define MDBH 0xFFFF4
#define MDBL 0xFFFF6
#define MDCL 0xF00E0
#define MDCH 0xF00E2
#define MDUC 0xF00E8
    /* 14-6: unsigned 2 x 3 = 6 */
    rl78_write8(c, MDUC, 0x00);
    rl78_write16(c, MDAL, 0xFFFF);
    rl78_write16(c, MDAH, 0xFFFF);
    CHECK_EQ(rl78_read16(c, MDBH), 0xFFFE);
    CHECK_EQ(rl78_read16(c, MDBL), 0x0001);
    rl78_write16(c, MDAL, 0x0002);
    rl78_write16(c, MDAH, 0x0003);
    CHECK_EQ(rl78_read16(c, MDBH), 0x0000);
    CHECK_EQ(rl78_read16(c, MDBL), 0x0006);
    /* 14-7: signed -2 x 32767 = -65534 */
    rl78_write8(c, MDUC, 0x08);
    rl78_write16(c, MDAL, 0xFFFE);
    rl78_write16(c, MDAH, 0x7FFF);
    CHECK_EQ(rl78_read16(c, MDBH), 0xFFFF);
    CHECK_EQ(rl78_read16(c, MDBL), 0x0002);
    /* 14-8: unsigned MAC 2 x 3 + 3 = 9; then 32767 x 2 + 4294901762 = 0 with overflow */
    rl78_write8(c, MDUC, 0x40);
    rl78_write16(c, MDCL, 0x0003);
    rl78_write16(c, MDCH, 0x0000);
    rl78_write16(c, MDAL, 0x0002);
    rl78_write16(c, MDAH, 0x0003);
    CHECK_EQ(rl78_read16(c, MDCL), 0x0009);
    CHECK_EQ(rl78_read16(c, MDCH), 0x0000);
    CHECK_EQ(rl78_read8(c, MDUC), 0x40);
    rl78_write16(c, MDCL, 0x0002);
    rl78_write16(c, MDCH, 0xFFFF);
    rl78_write16(c, MDAL, 0x7FFF);
    rl78_write16(c, MDAH, 0x0002);
    CHECK_EQ(rl78_read16(c, MDCL), 0x0000);
    CHECK_EQ(rl78_read16(c, MDCH), 0x0000);
    CHECK_EQ(rl78_read16(c, MDBL), 0xFFFE);
    CHECK_EQ(rl78_read8(c, MDUC), 0x44);       /* MACOF */
    /* 14-9: signed MAC 2 x 3 + (-4) = 2; then 32767 x (-1) + (-2147483647) overflows
       (each figure starts from the reset state) */
    rl78_reset(c);
    rl78_write8(c, MDUC, 0x48);
    rl78_write16(c, MDCH, 0xFFFF);
    CHECK_EQ(rl78_read8(c, MDUC), 0x4A);       /* MACSF follows the initial value */
    rl78_write16(c, MDCL, 0xFFFC);
    rl78_write16(c, MDAL, 0x0002);
    rl78_write16(c, MDAH, 0x0003);
    CHECK_EQ(rl78_read16(c, MDCL), 0x0002);
    CHECK_EQ(rl78_read16(c, MDCH), 0x0000);
    CHECK_EQ(rl78_read8(c, MDUC), 0x48);
    rl78_write16(c, MDCH, 0x8000);
    rl78_write16(c, MDCL, 0x0001);
    rl78_write16(c, MDAL, 0x7FFF);
    rl78_write16(c, MDAH, 0xFFFF);
    CHECK_EQ(rl78_read16(c, MDCH), 0x7FFF);
    CHECK_EQ(rl78_read16(c, MDCL), 0x8002);
    CHECK_EQ(rl78_read8(c, MDUC), 0x4C);       /* MACOF, MACSF = 0 */
    /* 14-10: 35 / 6 = 5 remainder 5 */
    rl78_reset(c);
    rl78_write8(c, MDUC, 0x80);
    rl78_write16(c, MDAH, 0x0000);
    rl78_write16(c, MDAL, 0x0023);
    rl78_write16(c, MDBH, 0x0000);
    rl78_write16(c, MDBL, 0x0006);
    rl78_write8(c, MDUC, 0x81);                /* DIVST */
    CHECK_EQ(rl78_read8(c, MDUC), 0x80);       /* completes (instantly in this model) */
    CHECK_EQ(rl78_read16(c, MDAL), 0x0005);
    CHECK_EQ(rl78_read16(c, MDAH), 0x0000);
    CHECK_EQ(rl78_read16(c, MDCL), 0x0005);
    CHECK_EQ(rl78_read16(c, MDCH), 0x0000);
    /* the same through instructions */
    {
        static const char *src =
            ".org 0\n.dw start\n.org 100H\nstart:\n"
            "  MOV A, #0\n  MOV !00E8H, A\n"          /* MDUC = 0 (2nd SFR via !addr16) */
            "  MOVW AX, #1234H\n  MOVW 0FFFF0H, AX\n"
            "  MOVW AX, #0010H\n  MOVW 0FFFF2H, AX\n"
            "  MOVW AX, 0FFFF6H\n  MOVW BC, AX\n"     /* MDBL */
            "  MOVW AX, 0FFFF4H\n"                    /* MDBH */
            "  HALT\n";
        if (t_load(c, src) == 0) {
            CHECK_EQ(rl78_run(c, 100), RL78_STOP_HALT);
            CHECK_EQ(rl78_get_reg(c, RL78_REG_BC), 0x2340);
            CHECK_EQ(rl78_get_reg(c, RL78_REG_AX), 0x0001);
        }
    }
    rl78_destroy(c);
}

/* RL78_CORE_AUTO: S3 instructions and G13 multiplier SFRs together */
static void test_auto_core(void)
{
    rl78_config cfg;
    rl78_cpu *c;
    char t[64];
    uint8_t mulhu[3] = {0xCE, 0xFB, 0x01};
    static const char *src =
        ".org 0\n.dw start\n.org 100H\nstart:\n"
        "  MOV A, #0\n  MOV !00E8H, A\n"            /* MDUC = 0 */
        "  MOVW AX, #0003H\n  MOVW 0FFFF0H, AX\n"   /* MDAL */
        "  MOVW AX, #0005H\n  MOVW 0FFFF2H, AX\n"   /* MDAH -> MDB = 15 */
        "  MOVW AX, #1000H\n  MOVW BC, #0010H\n"
        "  MACHU\n"                                  /* MACR = 00050003H + 10000H */
        "  MULHU\n"                                  /* BCAX = 1000H * 10H */
        "  HALT\n";
    rl78_default_config(&cfg, RL78_CORE_AUTO);
    c = rl78_create(&cfg);
    CHECK_EQ(rl78_get_config(c)->mdu, 1);
    if (t_load(c, src)) return;
    CHECK_EQ(rl78_run(c, 100), RL78_STOP_HALT);
    CHECK_EQ(rl78_read16(c, 0xFFFF6), 15);           /* MDBL kept: MACH did not trigger the multiplier */
    CHECK_EQ(rl78_read16(c, 0xFFFF4), 0);
    CHECK_EQ(rl78_read16(c, 0xFFFF0), 0x0003);       /* MACR low */
    CHECK_EQ(rl78_read16(c, 0xFFFF2), 0x0006);       /* MACR high: 0005H + 1 */
    CHECK_EQ(rl78_get_reg(c, RL78_REG_AX), 0x0000);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_BC), 0x0001);
    CHECK_EQ(rl78_disasm_buf(RL78_CORE_AUTO, mulhu, 3, 0, t, sizeof t), 3);
    CHECK(strcmp(t, "MULHU") == 0);
    rl78_destroy(c);
}

static void test_misc(void)
{
    rl78_cpu *c = t_cpu(RL78_CORE_S3);
    char t[96];
    uint8_t b[8];
    /* fault information */
    {
        uint8_t bad[2] = {0x61, 0x81};
        rl78_load(c, 0x9000, bad, 2);
        rl78_set_reg(c, RL78_REG_PC, 0x9000);
        CHECK_EQ(rl78_run(c, 10), RL78_STOP_UNDEFINED);
        CHECK_EQ(rl78_last_fault(c)->pc, 0x9000);
        CHECK(strstr(rl78_last_fault(c)->text, "undefined instruction at 09000") != NULL);
    }
    /* disassembler */
    b[0] = 0x11; b[1] = 0x8C; b[2] = 0x12;
    CHECK_EQ(rl78_disasm_buf(RL78_CORE_S3, b, 3, 0, t, sizeof t), 3);
    CHECK(strcmp(t, "MOV A, ES:[HL+12H]") == 0);
    b[0] = 0xDC; b[1] = 0xFE;
    CHECK_EQ(rl78_disasm_buf(RL78_CORE_S3, b, 2, 0x1000, t, sizeof t), 2);
    CHECK(strcmp(t, "BC $01000H") == 0);
    b[0] = 0xCE; b[1] = 0xFB; b[2] = 0x0B;
    rl78_disasm_buf(RL78_CORE_S3, b, 3, 0, t, sizeof t);
    CHECK(strcmp(t, "DIVWU") == 0);
    rl78_disasm_buf(RL78_CORE_S2, b, 3, 0, t, sizeof t);
    CHECK(strcmp(t, "MOV 0FFFFBH, #0BH") == 0);
    b[0] = 0x8E; b[1] = 0xFA;
    rl78_disasm_buf(RL78_CORE_S3, b, 2, 0, t, sizeof t);
    CHECK(strcmp(t, "MOV A, PSW") == 0);
    CHECK_EQ(rl78_disasm_buf(RL78_CORE_S3, b, 1, 0, t, sizeof t), 0); /* truncated */
    /* reset vector, counters */
    {
        uint8_t v[2] = {0x34, 0x12};
        rl78_load(c, 0, v, 2);
        rl78_reset(c);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x1234);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PSW), 0x06);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_ES), 0x0F);
        rl78_clear_counters(c);
        rl78_add_clocks(c, 10);
        CHECK_EQ(rl78_clock_count(c), 10);
        CHECK_EQ(rl78_insn_count(c), 0);
    }
    /* force_interrupt ignores IE */
    {
        uint8_t vec[2] = {0x00, 0x30};
        rl78_load(c, 0x0006, vec, 2);
        rl78_set_reg(c, RL78_REG_SP, 0xFE00);
        rl78_set_reg(c, RL78_REG_PSW, 0x06);
        rl78_set_reg(c, RL78_REG_PC, 0x2222);
        rl78_force_interrupt(c, 0x0006);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_PC), 0x3000);
        CHECK_EQ(rl78_get_reg(c, RL78_REG_SP), 0xFDFC);
        CHECK_EQ(rl78_peek8(c, 0xFFDFF), 0x06);
    }
    rl78_destroy(c);
    /* S1: PMC accepts only 00H, PSW has no RBS bits */
    c = t_cpu(RL78_CORE_S1);
    rl78_write8(c, 0xFFFFE, 0x01);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PMC), 0);
    rl78_write8(c, 0xFFFFA, 0xFF);
    CHECK_EQ(rl78_get_reg(c, RL78_REG_PSW), 0xD7);
    rl78_destroy(c);
}

void suite_api(void)
{
    test_breakpoints();
    test_hooks();
    test_snapshot();
    test_sleep_policy();
    test_mdu();
    test_auto_core();
    test_misc();
}
