/* コンパイラごとのレジスタ割付けと型制約。 */
#ifndef RL78_ABI_H
#define RL78_ABI_H
#include "rl78.h"
#include "sim_backend.h"
enum { R_A, R_X, R_B, R_C, R_D, R_E };
typedef struct { int used[6]; } regs_t;
int rl78_ccrl_arg(rl78_cpu *cpu, regs_t *g, uint32_t size, int is_far, uint64_t v);
int rl78_ca78k0r_check(const sim_func *f, char *err, size_t errlen);
uint64_t rl78_ca78k0r_ret(rl78_cpu *cpu, const sim_type *t);
#endif
