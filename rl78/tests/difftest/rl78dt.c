/*
 * rl78dt.c - command-line runner used by difftest.py to execute the same
 * binary that is run on the GNU simulator (rl78-elf-run).
 *
 *   rl78dt run  CORE BIN LOADADDR ENTRY [--mdu] [--clk FILE] [--trace]
 *       Loads BIN at LOADADDR, runs from ENTRY until HALT/STOP.  Bytes
 *       written to SDR00 (FFF10H) go to stdout (the GNU simulator's console
 *       convention).  Exit status = register A at HALT (also like the GNU
 *       simulator).  Memory is pre-filled like the GNU simulator does
 *       (FFH below F0000H, 33H above) so that reads of unwritten bytes agree.
 *       --clk FILE writes "pc clocks" for every executed instruction address.
 *   rl78dt dis  CORE BIN LOADADDR ADDRFILE
 *       Disassembles the instruction at each hex address in ADDRFILE.
 *   rl78dt table
 *       Prints the test assembler's encoding table ("pattern|bytes").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rl78.h"
#include "tasm.h"

static void sdr_write(void *ctx, rl78_cpu *cpu, uint32_t addr, uint32_t val, int size)
{
    (void)ctx;
    (void)cpu;
    /* only the byte stored at FFF10H is printed (a 16-bit write prints its low byte) */
    if (addr == 0xFFF10) putchar((int)(val & 0xFF));
    rl78_memory(cpu)[addr] = (uint8_t)val;
    if (size == 2) rl78_memory(cpu)[addr + 1] = (uint8_t)(val >> 8);
}

static rl78_core parse_core(const char *s)
{
    if (!strcmp(s, "S1")) return RL78_CORE_S1;
    if (!strcmp(s, "S2")) return RL78_CORE_S2;
    return RL78_CORE_S3;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static rl78_cpu *make(rl78_core core, int mdu, const uint8_t *bin, size_t len, uint32_t load)
{
    rl78_config cfg;
    rl78_cpu *cpu;
    uint8_t *m;
    rl78_default_config(&cfg, core);
    cfg.mdu = mdu;
    /* the GNU simulator's default mirror: F1000H-F7FFFH <- 01000H (G10: F8000H, 4 KB, from 0) */
    if (core == RL78_CORE_S1) {
        cfg.mirror_start = 0xF8000; cfg.mirror_end = 0xF9000; cfg.mirror_src = 0;
    } else {
        cfg.mirror_start = 0xF1000; cfg.mirror_end = 0xF8000; cfg.mirror_src = 0x1000;
    }
    cpu = rl78_create(&cfg);
    m = rl78_memory(cpu);
    memset(m, 0xFF, 0xF0000);
    memset(m + 0xF0000, 0x33, 0x10000);
    rl78_load(cpu, load, bin, len);
    rl78_add_hook(cpu, 0xFFF10, 0xFFF12, NULL, sdr_write, NULL);
    return cpu;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "table")) {
        int i, n = tasm_table_size();
        for (i = 0; i < n; i++) {
            const char *p, *b;
            tasm_table_entry(i, &p, &b);
            printf("%s|%s\n", p, b);
        }
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "run")) {
        size_t len;
        uint8_t *bin = read_file(argv[3], &len);
        uint32_t load = (uint32_t)strtoul(argv[4], NULL, 0), entry = (uint32_t)strtoul(argv[5], NULL, 0);
        int mdu = 0, i;
        const char *clkfile = NULL;
        rl78_cpu *cpu;
        rl78_stop_reason r;
        uint32_t *clk = NULL;
        for (i = 6; i < argc; i++) {
            if (!strcmp(argv[i], "--mdu")) mdu = 1;
            else if (!strcmp(argv[i], "--clk") && i + 1 < argc) clkfile = argv[++i];
        }
        cpu = make(parse_core(argv[2]), mdu, bin, len, load);
        rl78_set_reg(cpu, RL78_REG_PC, entry);
        if (clkfile) {
            clk = (uint32_t *)calloc(0x100000, sizeof *clk);
            for (;;) {
                uint32_t pc = rl78_get_reg(cpu, RL78_REG_PC);
                uint64_t c0 = rl78_clock_count(cpu);
                r = rl78_run(cpu, 1);
                clk[pc] += (uint32_t)(rl78_clock_count(cpu) - c0);
                if (r != RL78_STOP_LIMIT) break;
                if (rl78_insn_count(cpu) > 50000000u) break;
            }
        } else {
            r = rl78_run(cpu, 200000000u);
        }
        fflush(stdout);
        if (clkfile) {
            FILE *f = fopen(clkfile, "w");
            uint32_t a;
            for (a = 0; a < 0x100000; a++)
                if (clk[a]) fprintf(f, "%05x %u\n", (unsigned)a, (unsigned)clk[a]);
            fclose(f);
        }
        if (r == RL78_STOP_UNDEFINED) {
            fprintf(stderr, "rl78dt: %s\n", rl78_last_fault(cpu)->text);
            return 255;
        }
        if (r != RL78_STOP_HALT && r != RL78_STOP_STOPMODE) {
            fprintf(stderr, "rl78dt: stopped: %s at %05X\n", rl78_stop_reason_name(r),
                    (unsigned)rl78_get_reg(cpu, RL78_REG_PC));
            return 254;
        }
        return (int)rl78_get_reg(cpu, RL78_REG_A);
    }
    if (argc >= 6 && !strcmp(argv[1], "dis")) {
        size_t len;
        uint8_t *bin = read_file(argv[3], &len);
        uint32_t load = (uint32_t)strtoul(argv[4], NULL, 0);
        rl78_cpu *cpu = make(parse_core(argv[2]), 0, bin, len, load);
        FILE *f = fopen(argv[5], "r");
        char line[64];
        if (!f) { perror(argv[5]); return 2; }
        while (fgets(line, sizeof line, f)) {
            char t[96];
            uint32_t a = (uint32_t)strtoul(line, NULL, 16);
            int l = rl78_disasm(cpu, a, t, sizeof t);
            printf("%05x %d %s\n", (unsigned)a, l, t);
        }
        fclose(f);
        return 0;
    }
    fprintf(stderr, "usage: rl78dt run CORE BIN LOAD ENTRY [--mdu] [--clk F] | dis CORE BIN LOAD ADDRS | table\n");
    return 2;
}
