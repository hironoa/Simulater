/* tcommon.h - tiny test framework shared by the test suites. */
#ifndef TCOMMON_H
#define TCOMMON_H

#include <stdint.h>
#include <stdio.h>

#include "rl78.h"
#include "tasm.h"

extern unsigned long t_checks, t_fails;
extern const char *t_suite;

void t_fail(const char *file, int line, const char *fmt, ...);

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        t_checks++;                                                                       \
        if (!(cond)) t_fail(__FILE__, __LINE__, "%s", #cond);                             \
    } while (0)
#define CHECKF(cond, ...)                                                                 \
    do {                                                                                  \
        t_checks++;                                                                       \
        if (!(cond)) t_fail(__FILE__, __LINE__, __VA_ARGS__);                             \
    } while (0)
#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        unsigned long va_ = (unsigned long)(a), vb_ = (unsigned long)(b);                 \
        t_checks++;                                                                       \
        if (va_ != vb_) t_fail(__FILE__, __LINE__, "%s == %s: got %lX expected %lX", #a, #b, va_, vb_); \
    } while (0)

/* Standard test machine: code flash 00000H-0FFFFH, mirror F2000H-FBFFFH
   (source 02000H), RAM above.  HALT/STOP ignored unless asked. */
rl78_cpu *t_cpu(rl78_core core);
/* Assemble src into cpu memory and reset (reset vector must be in src, or
   pc is set to `start` label / 0x100 when missing). */
int t_load(rl78_cpu *cpu, const char *src);
/* Run until HALT or limit; returns stop reason. */
rl78_stop_reason t_run(rl78_cpu *cpu, uint64_t limit);

void suite_decode(void);
void suite_manual(void);
void suite_alu(void);
void suite_insn(void);
void suite_prog(void);
void suite_api(void);
void suite_regress(void);

#endif
