#ifndef SIM_DEFINES_H
#define SIM_DEFINES_H
#include "sim_expr.h"
/* Read all selected sources before resolving any definitions or types. */
int sim_defines_load(sim_ctx *c, const sim_open_opts *o, char *err, size_t cap);
#endif
