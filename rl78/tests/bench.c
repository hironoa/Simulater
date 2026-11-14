/*
 * bench.c - throughput benchmark: a loop with a typical instruction mix
 * (8/16-bit ALU, loads/stores via [HL+byte], saddr, !addr16, branches,
 * CALL/RET, PUSH/POP, skip, bit ops, shifts, a flash table read via the
 * mirror area).  Reports instructions/second and simulated clocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rl78.h"
#include "tasm.h"

static const char *prog =
    ".org 0\n"
    ".dw start\n"
    ".org 2000H\n"
    "table: .db 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16\n"
    ".org 0100H\n"
    "start:\n"
    "  MOVW SP, #0FE00H\n"
    "  MOVW HL, #0FD00H\n"
    "  MOVW DE, #0\n"
    "  MOVW BC, #0\n"
    "  CLRB 0FE20H\n"
    "loop:\n"
    "  MOV A, [HL+10H]\n"          /* load */
    "  ADD A, #3\n"
    "  MOV [HL+10H], A\n"          /* store */
    "  MOVW AX, BC\n"
    "  ADDW AX, #1234H\n"
    "  MOVW BC, AX\n"
    "  INC E\n"
    "  MOV A, E\n"
    "  AND A, #0FH\n"
    "  MOV C, A\n"
    "  MOV A, 2000H[C]\n"         /* flash table read via mirror */
    "  ADD A, 0FE20H\n"           /* saddr */
    "  MOV 0FE20H, A\n"
    "  CMP A, #55H\n"
    "  SKZ\n"
    "  XOR A, #1\n"
    "  BT A.0, $odd\n"
    "  SHL A, 1\n"
    "  BR $join\n"
    "odd:\n"
    "  SHR A, 1\n"
    "join:\n"
    "  MOV !0FD40H, A\n"
    "  CALL !sub\n"
    "  SET1 0FE21H.3\n"
    "  CLR1 0FE21H.3\n"
    "  DECW DE\n"
    "  MOVW AX, DE\n"
    "  CMPW AX, #0\n"
    "  BNZ $loop\n"
    "  BR $loop\n"
    "sub:\n"
    "  PUSH HL\n"
    "  MOVW AX, [HL+20H]\n"
    "  INCW AX\n"
    "  MOVW [HL+20H], AX\n"
    "  POP HL\n"
    "  RET\n";

int main(int argc, char **argv)
{
    rl78_config cfg;
    rl78_cpu *cpu;
    uint8_t *img = (uint8_t *)calloc(1, 0x100000);
    char err[256];
    uint64_t n = (argc > 1) ? strtoull(argv[1], NULL, 10) : 500000000ull;
    struct timespec t0, t1;
    double sec;
    int rep;
    double best = 0;

    if (tasm_program(prog, img, NULL, err, sizeof err)) {
        fprintf(stderr, "asm error: %s\n", err);
        return 1;
    }
    rl78_default_config(&cfg, RL78_CORE_S3);
    cfg.rom_size = 0x10000;
    cfg.mirror_start = 0xF2000;
    cfg.mirror_end = 0xFC000;
    cfg.mirror_src = 0x02000;
    cpu = rl78_create(&cfg);
    rl78_load(cpu, 0, img, 0x10000);
    for (rep = 0; rep < 3; rep++) {
        rl78_reset(cpu);
        rl78_clear_counters(cpu);
        timespec_get(&t0, TIME_UTC);
        if (rl78_run(cpu, n) != RL78_STOP_LIMIT) {
            fprintf(stderr, "unexpected stop: %s\n", rl78_last_fault(cpu)->text);
            return 1;
        }
        timespec_get(&t1, TIME_UTC);
        sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (n / sec > best) best = (double)n / sec;
        printf("bench: %llu insns, %llu clocks, %.3f s, %.1f M insns/s\n",
               (unsigned long long)rl78_insn_count(cpu), (unsigned long long)rl78_clock_count(cpu),
               sec, (double)n / sec / 1e6);
    }
    printf("bench best: %.1f M insns/s\n", best / 1e6);
    rl78_destroy(cpu);
    free(img);
    return 0;
}
