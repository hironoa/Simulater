/* tasm.h - minimal RL78 assembler for tests (independent encoding table). */
#ifndef TASM_H
#define TASM_H

#include <stddef.h>
#include <stdint.h>

/* Assemble one instruction at address pc.  Returns length (1..6) or -1 on
   error (message in err).  Labels are not available. */
int tasm_one(const char *text, uint32_t pc, uint8_t *out, char *err, size_t errn);

/* Assemble a multi-line program into mem (1 MB array).  Supports labels
   ("name:"), ".org ADDR", ".db b,b,...", ".dw w,...", comments after ';'.
   Returns 0 on success, -1 on error (message in err).  *end receives the
   address after the last emitted byte. */
int tasm_program(const char *src, uint8_t *mem, uint32_t *end, char *err, size_t errn);

/* Look up a label from the last tasm_program call (-1 if unknown). */
long tasm_label(const char *name);

/* Number of expanded encoding-table entries; iterate them (for table tests). */
int tasm_table_size(void);
/* Entry i: mnemonic+operand pattern text, byte template text, and whether it
   is an S3-only extension. */
void tasm_table_entry(int i, const char **pattern, const char **bytes);

#endif
