/*
 * xlrun.c - step 2 of the translation test: load the translated code (a DLL
 * built by clang from the output of xlgen) and check that running it gives
 * exactly what the interpreter gives.
 *
 *   xlrun <dir with xl_image.bin> <dll>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "rl78.h"
#include "tasm.h"
#include "xlprog.h"

static int failures, checks;
#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        checks++;                                                                 \
        if (!(cond)) { failures++; if (failures <= 20) { printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } \
    } while (0)

static rl78_cpu *mk(const uint8_t *img)
{
    rl78_config cfg;
    rl78_cpu *c;
    rl78_default_config(&cfg, RL78_CORE_S3);
    cfg.rom_size = XL_ROM;
    cfg.mirror_start = 0xF2000;
    cfg.mirror_end = 0xFC000;
    cfg.mirror_src = 0x02000;
    c = rl78_create(&cfg);
    rl78_load(c, 0, img, XL_ROM);
    rl78_reset(c);
    rl78_clear_counters(c);
    return c;
}

/* the registers and counters (cheap: compared after every run) */
static int same_regs(rl78_cpu *a, rl78_cpu *b)
{
    int r;
    for (r = 0; r < RL78_REG_COUNT; r++)
        if (rl78_get_reg(a, (rl78_reg)r) != rl78_get_reg(b, (rl78_reg)r)) return 0;
    return rl78_insn_count(a) == rl78_insn_count(b) && rl78_clock_count(a) == rl78_clock_count(b);
}

static int same_mem(rl78_cpu *a, rl78_cpu *b)
{
    static uint8_t ma[0x100000], mb[0x100000];
    rl78_peek(a, 0, ma, sizeof ma);
    rl78_peek(b, 0, mb, sizeof mb);
    return memcmp(ma, mb, sizeof ma) == 0;
}

static const char *where(rl78_cpu *a, rl78_cpu *b)
{
    static char s[200];
    snprintf(s, sizeof s, "interpreter pc %05X insns %llu clocks %llu / translated pc %05X insns %llu clocks %llu",
             (unsigned)rl78_get_reg(a, RL78_REG_PC), (unsigned long long)rl78_insn_count(a), (unsigned long long)rl78_clock_count(a),
             (unsigned)rl78_get_reg(b, RL78_REG_PC), (unsigned long long)rl78_insn_count(b), (unsigned long long)rl78_clock_count(b));
    return s;
}

static uint32_t rnd_state = 12345u;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

int main(int argc, char **argv)
{
    char path[1024], err[256];
    uint8_t *img = (uint8_t *)malloc(XL_ROM);
    const rl78_xl_module *(*get)(const rl78_xl_host *) = NULL;
    const rl78_xl_module *mod;
    rl78_cpu *a, *b;
    FILE *f;
    long sub, fib;
    int i;

    if (argc < 3) { fprintf(stderr, "usage: xlrun <dir> <dll>\n"); return 2; }
    snprintf(path, sizeof path, "%s/xl_image.bin", argv[1]);
    if (!img || !(f = fopen(path, "rb")) || fread(img, 1, XL_ROM, f) != XL_ROM) { fprintf(stderr, "xlrun: cannot read %s\n", path); return 2; }
    fclose(f);
#ifdef _WIN32
    {
        HMODULE h = LoadLibraryA(argv[2]);
        if (!h) { fprintf(stderr, "xlrun: cannot load %s (error %lu)\n", argv[2], (unsigned long)GetLastError()); return 2; }
        get = (const rl78_xl_module *(*)(const rl78_xl_host *))(void (*)(void))GetProcAddress(h, "rl78_xl_module_get");
    }
#else
    {
        void *h = dlopen(argv[2], RTLD_NOW);
        if (!h) { fprintf(stderr, "xlrun: cannot load %s: %s\n", argv[2], dlerror()); return 2; }
        *(void **)&get = dlsym(h, "rl78_xl_module_get");
    }
#endif
    if (!get) { fprintf(stderr, "xlrun: rl78_xl_module_get not found in %s\n", argv[2]); return 2; }
    mod = get(rl78_xl_host_get());

    /* labels of the test program (for the breakpoint checks) */
    {
        uint8_t *tmp = (uint8_t *)calloc(1, 0x100000);
        if (!tmp || tasm_program(xl_prog, tmp, NULL, err, sizeof err)) { fprintf(stderr, "xlrun: asm error: %s\n", err); return 2; }
        free(tmp);
        sub = tasm_label("sub");
        fib = tasm_label("fib");
        if (sub < 0 || fib < 0) { fprintf(stderr, "xlrun: labels not found\n"); return 2; }
    }

    printf("== translated code: one long run\n");
    a = mk(img);
    b = mk(img);
    CHECK(rl78_xl_attach(b, mod, err, sizeof err) == 0, "attach: %s", err);
    {
        const uint64_t N = 20000000;
        rl78_stop_reason ra = rl78_run(a, N), rb = rl78_run(b, N);
        CHECK(ra == RL78_STOP_LIMIT && rb == RL78_STOP_LIMIT, "stop reasons %d / %d", (int)ra, (int)rb);
        CHECK(same_regs(a, b), "registers differ: %s", where(a, b));
        CHECK(same_mem(a, b), "memory differs");
        CHECK(rl78_xl_insn_count(b) > N * 9 / 10, "translated code ran only %llu of %llu instructions",
              (unsigned long long)rl78_xl_insn_count(b), (unsigned long long)N);
        printf("  translated code ran %llu of %llu instructions\n", (unsigned long long)rl78_xl_insn_count(b), (unsigned long long)N);
    }

    printf("== translated code: runs of every length stop at the same instruction\n");
    for (i = 0; i < 3000 && failures == 0; i++) {
        /* short lengths cut blocks at every position; long ones cross many calls */
        uint64_t len = i < 1500 ? (uint64_t)(1 + i % 67) : (uint64_t)(1 + rnd() % 5000);
        rl78_stop_reason ra = rl78_run(a, len), rb = rl78_run(b, len);
        CHECK(ra == rb, "run %d: stop reasons %d / %d", i, (int)ra, (int)rb);
        CHECK(same_regs(a, b), "run %d of %llu: %s", i, (unsigned long long)len, where(a, b));
        if (i % 500 == 0) CHECK(same_mem(a, b), "run %d: memory differs", i);
    }
    CHECK(same_mem(a, b), "memory differs after the short runs");

    printf("== translated code: breakpoints\n");
    {
        /* at a block start (function entry): translated code hands over to the interpreter there */
        uint64_t before = rl78_xl_insn_count(b);
        rl78_add_breakpoint(a, (uint32_t)sub);
        rl78_add_breakpoint(b, (uint32_t)sub);
        for (i = 0; i < 50; i++) {
            rl78_stop_reason ra = rl78_run(a, 100000), rb = rl78_run(b, 100000);
            CHECK(ra == RL78_STOP_BREAKPOINT && rb == RL78_STOP_BREAKPOINT, "entry breakpoint %d: stop reasons %d / %d", i, (int)ra, (int)rb);
            CHECK(same_regs(a, b), "entry breakpoint %d: %s", i, where(a, b));
        }
        CHECK(rl78_get_reg(b, RL78_REG_PC) == (uint32_t)sub, "stopped at %05X, not at sub", (unsigned)rl78_get_reg(b, RL78_REG_PC));
        CHECK(rl78_xl_insn_count(b) > before, "translated code was not used between entry breakpoints");
        rl78_remove_breakpoint(a, (uint32_t)sub);
        rl78_remove_breakpoint(b, (uint32_t)sub);

        /* inside a block (fib + 3 bytes: the BC after CMPW): translated code must not run over it */
        rl78_add_breakpoint(a, (uint32_t)fib + 3u);
        rl78_add_breakpoint(b, (uint32_t)fib + 3u);
        before = rl78_xl_insn_count(b);
        for (i = 0; i < 50; i++) {
            rl78_stop_reason ra = rl78_run(a, 100000), rb = rl78_run(b, 100000);
            CHECK(ra == RL78_STOP_BREAKPOINT && rb == RL78_STOP_BREAKPOINT, "inner breakpoint %d: stop reasons %d / %d", i, (int)ra, (int)rb);
            CHECK(same_regs(a, b), "inner breakpoint %d: %s", i, where(a, b));
        }
        CHECK(rl78_xl_insn_count(b) == before, "translated code ran with a breakpoint inside a block");
        rl78_remove_breakpoint(a, (uint32_t)fib + 3u);
        rl78_remove_breakpoint(b, (uint32_t)fib + 3u);
        before = rl78_xl_insn_count(b);
        CHECK(rl78_run(a, 100000) == RL78_STOP_LIMIT && rl78_run(b, 100000) == RL78_STOP_LIMIT, "run after removing the breakpoints");
        CHECK(same_regs(a, b), "after the breakpoints: %s", where(a, b));
        CHECK(rl78_xl_insn_count(b) > before, "translated code was not used again after removing the breakpoint");
    }

    printf("== translated code: detach, and a module that does not fit\n");
    {
        uint64_t before;
        rl78_config cfg;
        rl78_cpu *o;
        CHECK(rl78_xl_attach(b, NULL, err, sizeof err) == 0, "detach");
        before = rl78_xl_insn_count(b);
        rl78_run(a, 50000);
        rl78_run(b, 50000);
        CHECK(same_regs(a, b), "after detach: %s", where(a, b));
        CHECK(rl78_xl_insn_count(b) == before, "translated code ran after detach");
        rl78_default_config(&cfg, RL78_CORE_S2);
        cfg.rom_size = XL_ROM;
        o = rl78_create(&cfg);
        CHECK(rl78_xl_attach(o, mod, err, sizeof err) != 0, "a module for the S3 core was accepted by an S2 CPU");
        rl78_destroy(o);
        rl78_default_config(&cfg, RL78_CORE_S3);
        o = rl78_create(&cfg);
        CHECK(rl78_xl_attach(o, mod, err, sizeof err) != 0, "translated code was accepted without a write-protected flash");
        rl78_destroy(o);
    }
    CHECK(same_mem(a, b), "memory differs at the end");

    printf("xlrun: %d checks, %d failures\n", checks, failures);
    rl78_destroy(a);
    rl78_destroy(b);
    free(img);
    return failures || checks == 0 ? 1 : 0;
}
