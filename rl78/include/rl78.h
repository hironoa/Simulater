/*
 * rl78.h - Renesas RL78 (S1/S2/S3 core) instruction-set simulator, public API.
 *
 * C11, no external dependencies.  Reference: Renesas "RL78 Family User's
 * Manual: Software" (R01US0015), Rev.2.40.
 *
 * Memory model
 *   - 1 MB flat address space (00000H-FFFFFH).
 *   - General-purpose registers live in RAM at FFEE0H-FFEFFH (bank selected
 *     by PSW.RBS1/RBS0); they are therefore also visible via saddr/!addr16.
 *   - Fixed SFRs: SPL/SPH (FFFF8/9), PSW (FFFFA), CS (FFFFC), ES (FFFFD),
 *     PMC (FFFFE), MEM (FFFFF).
 *   - Optional mirror area (product dependent, see rl78_config).
 *   - Optional RL78/G13-style multiplier/divider unit (MDAL..MDUC) and, on
 *     the S3 core, the MACR register used by MACH/MACHU (FFFF0H-FFFF3H).
 *   - Address ranges can be hooked (callbacks).  Unhooked addresses are plain
 *     memory and use a direct-array fast path.
 */
#ifndef RL78_H
#define RL78_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rl78_cpu rl78_cpu;
typedef struct rl78_snapshot rl78_snapshot;

typedef enum {
    RL78_CORE_S1 = 1, /* RL78/G10 etc.: no register banks, slower 16-bit ops */
    RL78_CORE_S2 = 2, /* RL78/G13 etc. */
    RL78_CORE_S3 = 3, /* RL78/G14, G23 etc.: MULHU/MULH/DIVHU/DIVWU/MACHU/MACH */
    RL78_CORE_AUTO = 4 /* superset for an unknown product: S3 instruction set and
                          clocks plus the G13 multiplier/divider SFRs (cfg.mdu is
                          forced on).  MACH/MACHU update MACR (FFFF0H-FFFF3H, the
                          same addresses as MDAL/MDAH) without triggering the
                          multiplier; program writes to MDAL/MDAH still do. */
} rl78_core;

/* What to do when HALT / STOP is executed. */
typedef enum {
    RL78_SLEEP_RETURN = 0, /* stop rl78_run() with RL78_STOP_HALT / RL78_STOP_STOPMODE;
                              the CPU stays asleep until an interrupt is requested */
    RL78_SLEEP_IGNORE = 1  /* treat as NOP and continue */
} rl78_sleep_policy;

typedef struct {
    rl78_core core;
    /* Code flash size in bytes (00000H..rom_size-1).  Writes to this range are
       ignored (as on the real device).  Data reads from it use the "flash"
       clock count (Note 2 of the operation list).  0 = no write protection and
       00000H-EFFFFH is treated as flash for clock counting. */
    uint32_t rom_size;
    /* Data flash range (for clock counting only).  size 0 = none. */
    uint32_t data_flash_start, data_flash_size;
    /* Mirror area in the F-space: [mirror_start, mirror_end).  Reads return
       the byte at mirror_src + (addr - mirror_start) (+10000H when PMC.MAA=1
       on S2/S3).  Writes are ignored.  mirror_end == 0 disables the mirror.
       Example (S2, 64KB flash, 4KB RAM): start F2000H, end FEF00H, src 02000H. */
    uint32_t mirror_start, mirror_end, mirror_src;
    /* Enable the RL78/G13 multiplier/divider/multiply-accumulator SFRs
       (MDAL FFFF0H, MDAH FFFF2H, MDBH FFFF4H, MDBL FFFF6H, MDCL F00E0H,
       MDCH F00E2H, MDUC F00E8H). */
    int mdu;
    rl78_sleep_policy halt_policy;
    rl78_sleep_policy stop_policy;
    /* Clocks added when an interrupt is accepted (G13 manual: 9..16). */
    unsigned irq_clocks;
} rl78_config;

/* Fill *cfg with defaults for the given core (no mirror, no MDU,
   rom_size 0, halt/stop return, irq_clocks 9). */
void rl78_default_config(rl78_config *cfg, rl78_core core);

rl78_cpu *rl78_create(const rl78_config *cfg);
void rl78_destroy(rl78_cpu *cpu);
const rl78_config *rl78_get_config(const rl78_cpu *cpu);

/* ---- memory ------------------------------------------------------------ */
/* Raw image load: writes bytes directly to memory (bypasses hooks and ROM
   write protection, keeps breakpoints consistent). */
void rl78_load(rl78_cpu *cpu, uint32_t addr, const void *data, size_t len);
/* Raw read of memory as the program sees it, without hooks and without
   side effects (breakpoint bytes are hidden).  Mirror is applied. */
uint8_t rl78_peek8(rl78_cpu *cpu, uint32_t addr);
void rl78_peek(rl78_cpu *cpu, uint32_t addr, void *out, size_t len);
/* Bus access as the CPU would do it (hooks and SFR side effects apply). */
uint8_t rl78_read8(rl78_cpu *cpu, uint32_t addr);
void rl78_write8(rl78_cpu *cpu, uint32_t addr, uint8_t val);
uint16_t rl78_read16(rl78_cpu *cpu, uint32_t addr);
void rl78_write16(rl78_cpu *cpu, uint32_t addr, uint16_t val);
/* Direct pointer to the 1 MB backing store (advanced use; do not write to
   bytes where breakpoints are set). */
uint8_t *rl78_memory(rl78_cpu *cpu);

/* ---- registers --------------------------------------------------------- */
typedef enum {
    RL78_REG_X, RL78_REG_A, RL78_REG_C, RL78_REG_B,
    RL78_REG_E, RL78_REG_D, RL78_REG_L, RL78_REG_H,   /* current bank, 8 bit */
    RL78_REG_AX, RL78_REG_BC, RL78_REG_DE, RL78_REG_HL, /* current bank, 16 bit */
    RL78_REG_PC, RL78_REG_SP, RL78_REG_PSW, RL78_REG_ES, RL78_REG_CS,
    RL78_REG_PMC,
    RL78_REG_COUNT
} rl78_reg;

/* PSW bits */
#define RL78_PSW_CY   0x01u
#define RL78_PSW_ISP0 0x02u
#define RL78_PSW_ISP1 0x04u
#define RL78_PSW_RBS0 0x08u
#define RL78_PSW_AC   0x10u
#define RL78_PSW_RBS1 0x20u
#define RL78_PSW_Z    0x40u
#define RL78_PSW_IE   0x80u

uint32_t rl78_get_reg(rl78_cpu *cpu, rl78_reg r);
/* Setting PC also leaves the HALT/STOP state (the host re-positions execution). */
void rl78_set_reg(rl78_cpu *cpu, rl78_reg r, uint32_t v);
/* Register of an explicit bank (0..3), r in X..H or AX..HL. */
uint32_t rl78_get_bank_reg(rl78_cpu *cpu, int bank, rl78_reg r);
void rl78_set_bank_reg(rl78_cpu *cpu, int bank, rl78_reg r, uint32_t v);

/* Reset: PSW=06H, ES=0FH, CS=0, PMC=0, SP=0 (undefined on hardware),
   PC=(00000H/00001H).  Memory is kept; MDU registers cleared; pending
   interrupts cleared; counters are NOT cleared (see rl78_clear_counters). */
void rl78_reset(rl78_cpu *cpu);

/* ---- execution --------------------------------------------------------- */
typedef enum {
    RL78_STOP_LIMIT = 0,      /* max_insns executed */
    RL78_STOP_BREAKPOINT,     /* PC reached a breakpoint (not executed yet).  A run that
                                 starts at a breakpoint executes it (resume). */
    RL78_STOP_HALT,           /* HALT executed (policy RETURN) / still halted */
    RL78_STOP_STOPMODE,       /* STOP executed (policy RETURN) / still stopped */
    RL78_STOP_UNDEFINED,      /* undefined opcode at PC (not executed) */
    RL78_STOP_REQUEST         /* rl78_request_stop() was called (e.g. from a hook) */
} rl78_stop_reason;

/* Execute up to max_insns instructions.  Returns the stop reason.  The number
   of instructions actually executed is added to the instruction counter. */
rl78_stop_reason rl78_run(rl78_cpu *cpu, uint64_t max_insns);
/* Execute exactly one instruction (convenience; same as rl78_run(cpu,1)). */
rl78_stop_reason rl78_step(rl78_cpu *cpu);
/* Ask rl78_run to return after the current instruction (callable from hooks). */
void rl78_request_stop(rl78_cpu *cpu);

uint64_t rl78_insn_count(const rl78_cpu *cpu);
uint64_t rl78_clock_count(const rl78_cpu *cpu);
void rl78_clear_counters(rl78_cpu *cpu);
/* Add clocks (e.g. to model wait states from a hook). */
void rl78_add_clocks(rl78_cpu *cpu, uint64_t clocks);

/* Information about the last RL78_STOP_UNDEFINED. */
typedef struct {
    uint32_t pc;
    uint8_t bytes[6];
    char text[96]; /* human readable message */
} rl78_fault;
const rl78_fault *rl78_last_fault(const rl78_cpu *cpu);

/* ---- breakpoints (address reached -> stop before executing it) ----------
   Implemented by code patching, so there is no per-instruction cost.  The
   address must be the first byte of an instruction (or of its ES: prefix).
   Data reads of that byte still return the original value.  There is no
   fixed limit (a 1 MB bitmap is allocated on first use); any number of
   addresses may be set. */
#define RL78_MAX_BREAKPOINTS 0x100000 /* kept for source compatibility */
int rl78_add_breakpoint(rl78_cpu *cpu, uint32_t addr);    /* 0 ok, -1 out of memory */
int rl78_breakpoint_count(const rl78_cpu *cpu);
int rl78_remove_breakpoint(rl78_cpu *cpu, uint32_t addr); /* 0 ok, -1 not found */
void rl78_clear_breakpoints(rl78_cpu *cpu);

/* ---- interrupts --------------------------------------------------------
   Request a maskable vectored interrupt.  vector = address of the vector
   table entry (e.g. 0x0004), level = priority 0 (highest) .. 3 (lowest).
   The request stays pending until it can be acknowledged (IE=1 and
   level <= PSW.ISP, honouring the "interrupt request hold" instructions),
   then PSW and PC are pushed, IE is cleared, ISP <- max(level-1, 0) (per
   G13 manual 16.3.5) and PC <- (vector).  A pending request also releases
   HALT/STOP.  Returns 0, or -1 if the pending table is full. */
#define RL78_MAX_PENDING_IRQ 32
int rl78_request_interrupt(rl78_cpu *cpu, uint16_t vector, int level);
void rl78_cancel_interrupts(rl78_cpu *cpu);
int rl78_pending_interrupts(const rl78_cpu *cpu);
/* Acknowledge an interrupt immediately regardless of IE/ISP (like a
   non-maskable interrupt; ISP is not changed, IE is cleared). */
void rl78_force_interrupt(rl78_cpu *cpu, uint16_t vector);
int rl78_is_halted(const rl78_cpu *cpu); /* 0 running, 1 HALT, 2 STOP */

/* ---- hooks ---------------------------------------------------------------
   Accesses to [start, end) call the callbacks instead of plain memory.
   size is 1 or 2 (16-bit accesses are even-aligned).  A NULL read callback
   means "read plain memory"; a NULL write callback means "write plain
   memory".  Hooks take precedence over the built-in SFR handling. */
typedef uint32_t (*rl78_read_hook)(void *ctx, rl78_cpu *cpu, uint32_t addr, int size);
typedef void (*rl78_write_hook)(void *ctx, rl78_cpu *cpu, uint32_t addr, uint32_t val, int size);
#define RL78_MAX_HOOKS 32
int rl78_add_hook(rl78_cpu *cpu, uint32_t start, uint32_t end,
                  rl78_read_hook rd, rl78_write_hook wr, void *ctx);
void rl78_clear_hooks(rl78_cpu *cpu);

/* ---- snapshots ------------------------------------------------------------ */
/* A snapshot holds memory, registers, HALT/STOP state, pending interrupts and
   also the instruction and clock counters: restore puts the counters back to
   their values at capture time.  Breakpoints and hooks are not part of a
   snapshot (the current ones stay in effect). */
rl78_snapshot *rl78_snapshot_create(rl78_cpu *cpu);   /* captures state */
void rl78_snapshot_update(rl78_cpu *cpu, rl78_snapshot *snap);
void rl78_snapshot_restore(rl78_cpu *cpu, const rl78_snapshot *snap);
void rl78_snapshot_free(rl78_snapshot *snap);

/* ---- disassembler ----------------------------------------------------------
   Syntax follows the manual: "MOV A, [HL+12H]", "BR $0123H".  saddr operands
   print as 4-digit 16-bit addresses (0FE20H), sfr operands as 5-digit 20-bit
   addresses (0FFF10H) or fixed names (PSW, SPL, ...), relative branches print
   the absolute target.  Returns the instruction length (1..6), or 0 if the
   bytes do not form a defined instruction for that core (text then holds
   "(undefined)").  avail = number of valid bytes in buf. */
int rl78_disasm_buf(rl78_core core, const uint8_t *buf, size_t avail, uint32_t pc,
                    char *text, size_t textlen);
/* Disassemble from simulator memory (breakpoints hidden). */
int rl78_disasm(rl78_cpu *cpu, uint32_t pc, char *text, size_t textlen);

const char *rl78_stop_reason_name(rl78_stop_reason r);

#ifdef __cplusplus
}
#endif
#endif
