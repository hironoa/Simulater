/* test_main.c - test runner: runs all suites, prints counts, exit 1 on failure. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "tcommon.h"

unsigned long t_checks, t_fails;
const char *t_suite = "";

void t_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    t_fails++;
    if (t_fails > 200) return; /* keep output readable */
    fprintf(stderr, "FAIL [%s] %s:%d: ", t_suite, file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

rl78_cpu *t_cpu(rl78_core core)
{
    rl78_config cfg;
    rl78_default_config(&cfg, core);
    cfg.rom_size = 0x10000;
    cfg.mirror_start = 0xF2000;
    cfg.mirror_end = 0xFC000;
    cfg.mirror_src = 0x02000;
    cfg.halt_policy = RL78_SLEEP_RETURN;
    cfg.stop_policy = RL78_SLEEP_RETURN;
    return rl78_create(&cfg);
}

int t_load(rl78_cpu *cpu, const char *src)
{
    static uint8_t img[0x100000];
    char err[256];
    uint32_t i;
    memset(img, 0, sizeof img);
    if (tasm_program(src, img, NULL, err, sizeof err)) {
        t_fails++;
        fprintf(stderr, "FAIL [%s] assembler: %s\n", t_suite, err);
        return -1;
    }
    /* load everything that is non-zero plus the whole code flash */
    rl78_load(cpu, 0, img, 0x10000);
    for (i = 0x10000; i < 0x100000; i += 0x100) {
        uint32_t k;
        for (k = 0; k < 0x100; k++)
            if (img[i + k]) { rl78_load(cpu, i, img + i, 0x100); break; }
    }
    rl78_reset(cpu);
    if (img[0] == 0 && img[1] == 0) {
        long s = tasm_label("start");
        rl78_set_reg(cpu, RL78_REG_PC, s >= 0 ? (uint32_t)s : 0x100u);
    }
    return 0;
}

rl78_stop_reason t_run(rl78_cpu *cpu, uint64_t limit)
{
    return rl78_run(cpu, limit);
}

int main(int argc, char **argv)
{
    struct {
        const char *name;
        void (*fn)(void);
    } suites[] = {
        {"decode", suite_decode}, {"manual", suite_manual}, {"alu", suite_alu},
        {"insn", suite_insn},     {"prog", suite_prog},     {"api", suite_api},
        {"regress", suite_regress},
    };
    size_t i;
    for (i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        unsigned long c0 = t_checks, f0 = t_fails;
        if (argc > 1 && strcmp(argv[1], suites[i].name)) continue;
        t_suite = suites[i].name;
        suites[i].fn();
        printf("suite %-8s checks %10lu  failures %lu\n", suites[i].name, t_checks - c0,
               t_fails - f0);
    }
    printf("TOTAL checks %lu failures %lu\n", t_checks, t_fails);
    if (t_checks == 0) {
        printf("no checks executed -> FAIL\n");
        return 1;
    }
    return t_fails ? 1 : 0;
}
