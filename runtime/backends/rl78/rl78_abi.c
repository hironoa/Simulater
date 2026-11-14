/* rl78_abi.c - CC-RL と CA78K0R の呼び出し規約。 */
#include "rl78_abi.h"
#include <stdio.h>

static int take1(regs_t *g, int r) { if (g->used[r]) return 0; g->used[r] = 1; return 1; }

static void set8(rl78_cpu *cpu, int r, uint32_t v)
{
    static const rl78_reg m[6] = {RL78_REG_A, RL78_REG_X, RL78_REG_B, RL78_REG_C, RL78_REG_D, RL78_REG_E};
    rl78_set_reg(cpu, m[r], v & 0xFFu);
}

static int pair_free(regs_t *g, int hi, int lo) { return !g->used[hi] && !g->used[lo]; }

/* 引数をレジスタに置く。置けなければ 0（スタック渡し） */
int rl78_ccrl_arg(rl78_cpu *cpu, regs_t *g, uint32_t size, int is_far, uint64_t v)
{
    if (size == 1) {
        static const int pr[6] = {R_A, R_X, R_C, R_B, R_E, R_D};
        int i;
        for (i = 0; i < 6; i++) if (take1(g, pr[i])) { set8(cpu, pr[i], (uint32_t)v); return 1; }
        return 0;
    }
    if (size == 2) {
        if (pair_free(g, R_A, R_X)) { g->used[R_A] = g->used[R_X] = 1; rl78_set_reg(cpu, RL78_REG_AX, (uint32_t)v & 0xFFFFu); return 1; }
        if (pair_free(g, R_B, R_C)) { g->used[R_B] = g->used[R_C] = 1; rl78_set_reg(cpu, RL78_REG_BC, (uint32_t)v & 0xFFFFu); return 1; }
        if (pair_free(g, R_D, R_E)) { g->used[R_D] = g->used[R_E] = 1; rl78_set_reg(cpu, RL78_REG_DE, (uint32_t)v & 0xFFFFu); return 1; }
        return 0;
    }
    if (size == 4 && is_far) {
        /* A-DE, X-DE, C-DE, B-DE, X-BC */
        static const int page[5] = {R_A, R_X, R_C, R_B, R_X};
        int i;
        for (i = 0; i < 5; i++) {
            int lo_hi = i < 4 ? R_D : R_B, lo_lo = i < 4 ? R_E : R_C;
            if (!g->used[page[i]] && pair_free(g, lo_hi, lo_lo)) {
                g->used[page[i]] = g->used[lo_hi] = g->used[lo_lo] = 1;
                set8(cpu, page[i], (uint32_t)(v >> 16) & 0x0Fu);
                rl78_set_reg(cpu, i < 4 ? RL78_REG_DE : RL78_REG_BC, (uint32_t)v & 0xFFFFu);
                return 1;
            }
        }
        return 0;
    }
    if (size == 4) {
        if (pair_free(g, R_B, R_C) && pair_free(g, R_A, R_X)) {
            g->used[R_A] = g->used[R_X] = g->used[R_B] = g->used[R_C] = 1;
            rl78_set_reg(cpu, RL78_REG_AX, (uint32_t)v & 0xFFFFu);
            rl78_set_reg(cpu, RL78_REG_BC, (uint32_t)(v >> 16) & 0xFFFFu);
            return 1;
        }
        if (pair_free(g, R_D, R_E) && pair_free(g, R_B, R_C)) {
            g->used[R_B] = g->used[R_C] = g->used[R_D] = g->used[R_E] = 1;
            rl78_set_reg(cpu, RL78_REG_BC, (uint32_t)v & 0xFFFFu);
            rl78_set_reg(cpu, RL78_REG_DE, (uint32_t)(v >> 16) & 0xFFFFu);
            return 1;
        }
        return 0;
    }
    return 0;
}


static int scalar(const sim_type *t)
{
    return t && (t->kind == SIM_T_INT || t->kind == SIM_T_UINT || t->kind == SIM_T_PTR || t->kind == SIM_T_BOOL) &&
           (t->size == 1 || t->size == 2 || t->size == 4) &&
           !(t->kind == SIM_T_PTR && (t->size != 2 || (t->target && t->target->kind == SIM_T_FUNC)));
}
int rl78_ca78k0r_check(const sim_func *f, char *err, size_t errlen)
{
    int i;
    if (f->variadic) { snprintf(err,errlen,"CA78K0R: 可変引数は対応外です"); return -1; }
    if (f->ret && f->ret->kind != SIM_T_VOID && !scalar(f->ret)) {
        snprintf(err,errlen,"CA78K0R: 関数 %s の戻り値の型は対応外です（構造体・型不明など）",f->name); return -1;
    }
    for (i=0;i<f->nparams;i++) {
        const sim_type *t = f->param_types[i];
        if (!scalar(t) || (t->kind == SIM_T_BOOL)) {
            snprintf(err,errlen,"CA78K0R: 関数 %s の第 %d 引数の型は未確認のため対応外です",f->name,i+1); return -1;
        }
        if (i == 0 && t->size > 2) {
            snprintf(err,errlen,"CA78K0R: 第1引数の4バイト値は AX/BC の順序が未確認のため対応外です"); return -1;
        }
    }
    return 0;
}
uint64_t rl78_ca78k0r_ret(rl78_cpu *cpu, const sim_type *t)
{
    uint64_t lo = rl78_get_reg(cpu,RL78_REG_BC);
    if (t->kind == SIM_T_BOOL) return rl78_get_reg(cpu,RL78_REG_PSW) & 1u;
    if (t->size == 1) return lo & 0xFFu;
    if (t->size == 2) return lo;
    return lo | (uint64_t)rl78_get_reg(cpu,RL78_REG_DE) << 16;
}
