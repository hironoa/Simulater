/*
 * rl78_backend.c - RL78 の本物のバックエンド
 *
 * .abs（ELF + DWARF）を読み、RL78 CPU コア（rl78/）でスタートアップを実行して、初期設定終了アドレス
 * （既定 main の先頭）で止めた状態を「初期状態」として控える。対象関数は CC-RL の呼び出し規約で呼ぶ。
 *
 * 呼び出し規約（CC-RL）:
 *   引数は左から順に、空いている最優先のレジスタへ。入らなければスタック（低いアドレスから、2 バイト境界）
 *     1 バイト: A, X, C, B, E, D      2 バイト: AX, BC, DE
 *     4 バイト far ポインタ: A-DE, X-DE, C-DE, B-DE, X-BC（上位レジスタの下位 4 ビットがページ）
 *     4 バイトその他: BC-AX, DE-BC     5 バイト以上・8 バイト型: スタック
 *   戻り値: 1 バイト A、2 バイト AX、3 バイト C-AX、far ポインタ A-DE、4 バイト BC-AX、
 *           5 バイト以上は呼び出し側が用意した領域への near ポインタを第 1 引数として渡す
 *   戻り番地は 4 バイト（PC の下位、中位、上位、もう 1 バイト）。ここに番兵アドレスを積み、
 *   そこに置いたブレークポイントで戻りを検出する。もう 1 バイトには PSW を入れておく（RETI でも戻れるように）
 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "rl78.h"
#include "sim_cfg.h"
#include "sim_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STUBS 256
#define SIM_MAX_ARGS_RL78 8

typedef struct {
    uint32_t orig, stub;
    const char *oname, *sname;
    int patched;            /* ブレークポイントが足りず、元の関数の先頭を BR !!stub に書き換えた */
} stub_t;

typedef struct {
    rl78_elf elf;
    rl78_symtab st;
    rl78_map map;
    int have_map;
    rl78_warns w;
    sim_cfg cfg;
    char *abs_path, *map_path;

    rl78_cpu *cpu;
    rl78_config cc;
    char core_desc[96];
    rl78_snapshot *snap;

    uint32_t init_end;
    const char *init_end_name;
    uint32_t sentinel;
    uint32_t call_sp;
    uint32_t ram_lo, ram_hi;            /* [lo, hi) */
    uint32_t stack_lo, stack_hi;
    int have_stack;
    uint32_t heap_lo, heap_hi, heap_cur;
    char heap_desc[96];

    stub_t stubs[MAX_STUBS];
    int nstubs;

    uint64_t max_insns, startup_max;
    uint64_t tot_insns, tot_clocks, ncalls, startup_insns, startup_clocks;

    int map_sect_ok, map_sect_ng, map_sym_ok, map_sym_ng;
} be_t;

/* ================================================================ 設定の値 */

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;
    size_t n;
    char tmp[64];
    if (!s) return -1;
    while (*s == ' ') s++;
    n = strlen(s);
    if (!n || n >= sizeof tmp) return -1;
    memcpy(tmp, s, n + 1);
    /* 末尾の H（0FFF0H のようなルネサス流の 16 進） */
    if (n > 1 && (tmp[n - 1] == 'h' || tmp[n - 1] == 'H')) {
        tmp[n - 1] = 0;
        *out = strtoull(tmp, &end, 16);
        return *end ? -1 : 0;
    }
    *out = strtoull(tmp, &end, 0);
    return *end ? -1 : 0;
}

static int parse_onoff(const char *s, int *out)
{
    if (!s) return -1;
    if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "ON")) { *out = 1; return 0; }
    if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "false") || !strcmp(s, "OFF")) { *out = 0; return 0; }
    return -1;
}

/* "START-END" または "START:SIZE"（16 進） */
static int parse_range(const char *s, uint32_t *lo, uint32_t *hi)
{
    char a[64], b[64];
    const char *sep = strchr(s, '-');
    int is_size = 0;
    uint64_t x, y;
    size_t n;
    if (!sep) { sep = strchr(s, ':'); is_size = 1; }
    if (!sep) return -1;
    n = (size_t)(sep - s);
    if (n >= sizeof a || strlen(sep + 1) >= sizeof b) return -1;
    memcpy(a, s, n);
    a[n] = 0;
    strcpy(b, sep + 1);
    while (n > 0 && a[n - 1] == ' ') a[--n] = 0;
    if (parse_u64(a, &x) || parse_u64(b, &y)) return -1;
    *lo = (uint32_t)x;
    *hi = (uint32_t)(is_size ? x + y : y + 1);
    return *hi > *lo ? 0 : -1;
}

/* ================================================================ シンボル */

static const rl78_funcinfo *find_func(be_t *z, const char *name)
{
    size_t i;
    const rl78_funcinfo *hit = NULL;
    for (i = 0; i < z->st.nfuncs; i++) {
        if (strcmp(z->st.funcs[i].f.name, name) == 0) {
            if (z->st.funcs[i].f.is_external) return &z->st.funcs[i];
            if (!hit) hit = &z->st.funcs[i];
        }
    }
    return hit;
}

/* 関数名、ELF のシンボル名、アドレスのどれかを解決する */
static int resolve_code(be_t *z, const char *s, uint32_t *addr, const char **name)
{
    uint64_t v;
    const rl78_funcinfo *f;
    const rl78_sym *y;
    if (!s || !*s) return -1;
    if (isdigit((unsigned char)s[0]) && !parse_u64(s, &v)) { *addr = (uint32_t)v; *name = s; return 0; }
    f = find_func(z, s);
    if (f) { *addr = (uint32_t)f->f.addr; *name = f->f.name; return 0; }
    y = rl78_elf_symbol(&z->elf, s);
    if (y) { *addr = y->value; *name = s; return 0; }
    {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "_%s", s);
        y = rl78_elf_symbol(&z->elf, tmp);
        if (y) { *addr = y->value; *name = s; return 0; }
    }
    return -1;
}

static uint32_t sym_value(be_t *z, const char *name, int *found)
{
    const rl78_sym *y = rl78_elf_symbol(&z->elf, name);
    *found = y != NULL;
    return y ? y->value : 0;
}

/* ================================================================ メモリ配置 */

typedef struct { uint32_t lo, hi; } span_t;

static int span_cmp(const void *a, const void *b)
{
    const span_t *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static int overlaps_section(be_t *z, uint32_t lo, uint32_t hi)
{
    int i;
    for (i = 0; i < z->elf.nsect; i++) {
        const rl78_sect *s = &z->elf.sect[i];
        if (!(s->flags & RL78_SHF_ALLOC) || !s->size) continue;
        if (s->addr < hi && s->addr + s->size > lo) return 1;
    }
    return 0;
}

static void layout(be_t *z)
{
    int found1, found2, i;
    uint32_t v1, v2;
    const char *s;
    /* RAM */
    v1 = sym_value(z, "__RAM_ADDR_START", &found1);
    v2 = sym_value(z, "__RAM_ADDR_END", &found2);
    if (found1 && v1 >= 0xF0000u && v1 < 0xFFEE0u) {
        z->ram_lo = v1;
        z->ram_hi = found2 && v2 > v1 && v2 <= 0xFFEE0u ? v2 : 0xFFEE0u;
    } else {
        uint32_t lo = 0xFFFFFFFFu;
        for (i = 0; i < z->elf.nsect; i++) {
            const rl78_sect *x = &z->elf.sect[i];
            if ((x->flags & RL78_SHF_ALLOC) && (x->flags & RL78_SHF_WRITE) && x->addr >= 0xF0000u && x->addr < 0xFFEE0u && x->addr < lo) lo = x->addr;
        }
        z->ram_lo = lo != 0xFFFFFFFFu ? (lo & ~0xFFu) : 0xFE900u;
        z->ram_hi = 0xFFEE0u;
    }
    if ((s = sim_cfg_get(&z->cfg, "ram")) && parse_range(s, &z->ram_lo, &z->ram_hi))
        rl78_warn(&z->w, "sim.cfg: ram = %s を読めません（例: ram = 0xFBF00-0xFFEDF）", s);
    /* スタック */
    v1 = sym_value(z, "__STACK_ADDR_START", &found1);
    v2 = sym_value(z, "__STACK_ADDR_END", &found2);
    if (found1 && found2 && v1 > v2) { z->stack_lo = v2; z->stack_hi = v1; z->have_stack = 1; }
    else {
        const rl78_sect *x = rl78_elf_section(&z->elf, ".stack_bss");
        if (x && x->size) { z->stack_lo = x->addr; z->stack_hi = x->addr + x->size; z->have_stack = 1; }
    }
}

static void choose_heap(be_t *z)
{
    const char *s = sim_cfg_get(&z->cfg, "heap");
    span_t sp[512];
    int n = 0, i;
    uint32_t best_lo = 0, best_len = 0, cur;
    if (s) {
        if (parse_range(s, &z->heap_lo, &z->heap_hi) == 0) {
            snprintf(z->heap_desc, sizeof z->heap_desc, "sim.cfg の heap");
            return;
        }
        rl78_warn(&z->w, "sim.cfg: heap = %s を読めません（例: heap = 0xFB000-0xFB3FF または heap = 0xFB000:0x400）", s);
    }
    for (i = 0; i < z->elf.nsect && n < 500; i++) {
        const rl78_sect *x = &z->elf.sect[i];
        if (!(x->flags & RL78_SHF_ALLOC) || !x->size) continue;
        if (x->addr + x->size <= z->ram_lo || x->addr >= z->ram_hi) continue;
        sp[n].lo = x->addr;
        sp[n].hi = x->addr + x->size;
        n++;
    }
    if (z->have_stack) { sp[n].lo = z->stack_lo; sp[n].hi = z->stack_hi; n++; }
    qsort(sp, (size_t)n, sizeof sp[0], span_cmp);
    cur = z->ram_lo;
    for (i = 0; i <= n; i++) {
        uint32_t lo = i < n ? sp[i].lo : z->ram_hi;
        if (lo > cur && lo - cur > best_len) { best_lo = cur; best_len = lo - cur; }
        if (i < n && sp[i].hi > cur) cur = sp[i].hi;
    }
    best_lo = (best_lo + 1u) & ~1u;
    if (best_len >= 256) {
        z->heap_lo = best_lo;
        z->heap_hi = best_lo + (best_len & ~1u);
        snprintf(z->heap_desc, sizeof z->heap_desc, "自動: 使われていない RAM");
        return;
    }
    if (z->have_stack && z->stack_hi - z->stack_lo >= 512) {
        uint32_t len = (z->stack_hi - z->stack_lo) / 4;
        if (len > 2048) len = 2048;
        z->heap_lo = (z->stack_lo + 1u) & ~1u;
        z->heap_hi = z->heap_lo + (len & ~1u);
        snprintf(z->heap_desc, sizeof z->heap_desc, "自動: スタックの底（使われていない RAM がないため）");
        return;
    }
    z->heap_lo = z->heap_hi = 0;
    snprintf(z->heap_desc, sizeof z->heap_desc, "なし（sim.cfg の heap で指定してください）");
}

static void choose_sentinel(be_t *z)
{
    const char *s = sim_cfg_get(&z->cfg, "sentinel");
    uint64_t v;
    uint32_t a;
    if (s && !parse_u64(s, &v)) { z->sentinel = (uint32_t)v & 0xFFFFEu; return; }
    for (a = 0xEFFF0u; a > 0x10000u; a -= 0x100u)
        if (!overlaps_section(z, a, a + 16u)) { z->sentinel = a; return; }
    z->sentinel = 0xEFFF0u;
}

/* ================================================================ 実行 */

static void disasm_at(be_t *z, uint32_t pc, char *out, size_t n)
{
    char t[96];
    uint8_t b[6];
    int len, i;
    sim_buf sb;
    len = rl78_disasm(z->cpu, pc, t, sizeof t);
    rl78_peek(z->cpu, pc, b, sizeof b);
    sim_buf_init(&sb);
    for (i = 0; i < (len > 0 ? len : 4); i++) sim_buf_printf(&sb, "%s%02X", i ? " " : "", b[i]);
    snprintf(out, n, "%s  %s", sim_buf_cstr(&sb), t);
    sim_buf_free(&sb);
}

static const char *func_name_at(be_t *z, uint32_t pc)
{
    size_t i;
    const char *best = NULL;
    uint32_t ba = 0;
    for (i = 0; i < z->st.nfuncs; i++) {
        const rl78_funcinfo *f = &z->st.funcs[i];
        uint32_t a = (uint32_t)f->f.addr;
        if (a <= pc && a >= ba && (!f->size || pc < a + f->size)) { ba = a; best = f->f.name; }
    }
    return best;
}

static void where(be_t *z, uint32_t pc, char *out, size_t n)
{
    char d[160];
    const char *fn = func_name_at(z, pc);
    disasm_at(z, pc, d, sizeof d);
    if (fn) snprintf(out, n, "PC=0x%05X（%s 内）: %s", pc, fn, d);
    else snprintf(out, n, "PC=0x%05X: %s", pc, d);
}

static const stub_t *stub_at(be_t *z, uint32_t pc)
{
    int i;
    for (i = 0; i < z->nstubs; i++) if (z->stubs[i].orig == pc && !z->stubs[i].patched) return &z->stubs[i];
    return NULL;
}

/* stop_at に着くまで実行する。成功 0 */
static int run_to(be_t *z, uint32_t stop_at, uint64_t limit, const char *what, char *err, size_t errlen)
{
    uint64_t remaining = limit;
    for (;;) {
        uint64_t i0 = rl78_insn_count(z->cpu), c0 = rl78_clock_count(z->cpu), di;
        rl78_stop_reason r = rl78_run(z->cpu, remaining);
        uint32_t pc;
        char wh[300];
        di = rl78_insn_count(z->cpu) - i0;
        z->tot_insns += di;
        z->tot_clocks += rl78_clock_count(z->cpu) - c0;
        remaining = di >= remaining ? 0 : remaining - di;
        pc = rl78_get_reg(z->cpu, RL78_REG_PC);
        switch (r) {
        case RL78_STOP_BREAKPOINT: {
            const stub_t *s;
            if (pc == stop_at) return 0;
            s = stub_at(z, pc);
            if (s) { rl78_set_reg(z->cpu, RL78_REG_PC, s->stub); continue; }
            where(z, pc, wh, sizeof wh);
            snprintf(err, errlen, "%s: 予期しないブレークポイント（%s）", what, wh);
            return -1;
        }
        case RL78_STOP_LIMIT:
            where(z, pc, wh, sizeof wh);
            snprintf(err, errlen, "%s: 命令数の上限 %llu を超えました。無限ループか暴走の可能性があります（%s）。"
                     "周辺機能の待ちならスタブ（sim.cfg の stub =）で置き換えてください",
                     what, (unsigned long long)limit, wh);
            return -1;
        case RL78_STOP_UNDEFINED: {
            const rl78_fault *f = rl78_last_fault(z->cpu);
            const char *fn = func_name_at(z, f->pc);
            snprintf(err, errlen, "%s: 未定義命令を実行しようとしました（PC=0x%05X%s%s%s、バイト列 %02X %02X %02X %02X）。暴走の可能性があります",
                     what, f->pc, fn ? "、" : "", fn ? fn : "", fn ? " 内" : "", f->bytes[0], f->bytes[1], f->bytes[2], f->bytes[3]);
            return -1;
        }
        case RL78_STOP_HALT: case RL78_STOP_STOPMODE:
            where(z, pc, wh, sizeof wh);
            snprintf(err, errlen, "%s: %s 命令で止まりました（%s）。sim.cfg の %s = ignore で読み飛ばせます", what,
                     r == RL78_STOP_HALT ? "HALT" : "STOP", wh, r == RL78_STOP_HALT ? "halt" : "stop");
            return -1;
        default:
            where(z, pc, wh, sizeof wh);
            snprintf(err, errlen, "%s: 実行が止まりました（%s、%s）", what, rl78_stop_reason_name(r), wh);
            return -1;
        }
    }
}

/* ================================================================ 呼び出し */

enum { R_A, R_X, R_B, R_C, R_D, R_E };

typedef struct {
    int used[6];
} regs_t;

static int take1(regs_t *g, int r) { if (g->used[r]) return 0; g->used[r] = 1; return 1; }

static void set8(be_t *z, int r, uint32_t v)
{
    static const rl78_reg m[6] = {RL78_REG_A, RL78_REG_X, RL78_REG_B, RL78_REG_C, RL78_REG_D, RL78_REG_E};
    rl78_set_reg(z->cpu, m[r], v & 0xFFu);
}

static int pair_free(regs_t *g, int hi, int lo) { return !g->used[hi] && !g->used[lo]; }

/* 引数をレジスタに置く。置けなければ 0（スタック渡し） */
static int put_reg_arg(be_t *z, regs_t *g, uint32_t size, int is_far, uint64_t v)
{
    if (size == 1) {
        static const int pr[6] = {R_A, R_X, R_C, R_B, R_E, R_D};
        int i;
        for (i = 0; i < 6; i++) if (take1(g, pr[i])) { set8(z, pr[i], (uint32_t)v); return 1; }
        return 0;
    }
    if (size == 2) {
        if (pair_free(g, R_A, R_X)) { g->used[R_A] = g->used[R_X] = 1; rl78_set_reg(z->cpu, RL78_REG_AX, (uint32_t)v & 0xFFFFu); return 1; }
        if (pair_free(g, R_B, R_C)) { g->used[R_B] = g->used[R_C] = 1; rl78_set_reg(z->cpu, RL78_REG_BC, (uint32_t)v & 0xFFFFu); return 1; }
        if (pair_free(g, R_D, R_E)) { g->used[R_D] = g->used[R_E] = 1; rl78_set_reg(z->cpu, RL78_REG_DE, (uint32_t)v & 0xFFFFu); return 1; }
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
                set8(z, page[i], (uint32_t)(v >> 16) & 0x0Fu);
                rl78_set_reg(z->cpu, i < 4 ? RL78_REG_DE : RL78_REG_BC, (uint32_t)v & 0xFFFFu);
                return 1;
            }
        }
        return 0;
    }
    if (size == 4) {
        if (pair_free(g, R_B, R_C) && pair_free(g, R_A, R_X)) {
            g->used[R_A] = g->used[R_X] = g->used[R_B] = g->used[R_C] = 1;
            rl78_set_reg(z->cpu, RL78_REG_AX, (uint32_t)v & 0xFFFFu);
            rl78_set_reg(z->cpu, RL78_REG_BC, (uint32_t)(v >> 16) & 0xFFFFu);
            return 1;
        }
        if (pair_free(g, R_D, R_E) && pair_free(g, R_B, R_C)) {
            g->used[R_B] = g->used[R_C] = g->used[R_D] = g->used[R_E] = 1;
            rl78_set_reg(z->cpu, RL78_REG_BC, (uint32_t)v & 0xFFFFu);
            rl78_set_reg(z->cpu, RL78_REG_DE, (uint32_t)(v >> 16) & 0xFFFFu);
            return 1;
        }
        return 0;
    }
    return 0;
}

static int is_far_ptr(const sim_type *t) { return t && t->kind == SIM_T_PTR && t->size >= 3; }

static void poke(be_t *z, uint32_t a, const uint8_t *p, size_t n) { rl78_load(z->cpu, a, p, n); }

static int b_call(sim_backend *b, const sim_func *f, const int64_t *args, int nargs, uint64_t *ret_raw,
                  char *err, size_t errlen)
{
    be_t *z = b->impl;
    regs_t g;
    uint32_t sp, base, stack_bytes = 0, retbuf = 0, off;
    uint32_t sizes[SIM_MAX_ARGS_RL78 + 1];
    int onstack[SIM_MAX_ARGS_RL78 + 1];
    int i, has_hidden = 0;
    uint32_t rsize = f->ret ? f->ret->size : 0;
    uint8_t blk[8];
    char what[300];
    const stub_t *st;
    if (nargs > SIM_MAX_ARGS_RL78) { snprintf(err, errlen, "引数が多すぎます"); return -1; }
    memset(&g, 0, sizeof g);
    z->ncalls++;
    /* レジスタを決まった値から始める（呼び出し後のレジスタは CC-RL では保証されない） */
    rl78_set_reg(z->cpu, RL78_REG_AX, 0);
    rl78_set_reg(z->cpu, RL78_REG_BC, 0);
    rl78_set_reg(z->cpu, RL78_REG_DE, 0);
    rl78_set_reg(z->cpu, RL78_REG_HL, 0);
    sp = z->call_sp & ~1u;
    if (f->ret && rsize > 4) {
        /* 5 バイト以上の戻り値: 呼び出し側の領域へのポインタを第 1 引数にする */
        sp = (sp - ((rsize + 1u) & ~1u)) & ~1u;
        retbuf = sp;
        has_hidden = 1;
        memset(blk, 0, sizeof blk);
        poke(z, retbuf, blk, rsize > 8 ? 8 : rsize);
        put_reg_arg(z, &g, 2, 0, retbuf & 0xFFFFu);
    }
    for (i = 0; i < nargs; i++) {
        const sim_type *t = f->param_types[i];
        uint32_t sz = t ? t->size : 2;
        if (sz == 0) sz = 2;
        sizes[i] = sz;
        onstack[i] = !put_reg_arg(z, &g, sz > 4 ? 99 : sz, is_far_ptr(t), (uint64_t)args[i]);
        if (onstack[i]) stack_bytes += (sz + 1u) & ~1u;
    }
    base = (sp - stack_bytes) & ~1u;
    off = 0;
    for (i = 0; i < nargs; i++) {
        uint32_t k, sz = sizes[i];
        if (!onstack[i]) continue;
        for (k = 0; k < 8; k++) blk[k] = (uint8_t)((uint64_t)args[i] >> (8 * k));
        if (is_far_ptr(f->param_types[i])) blk[3] = 0;
        poke(z, base + off, blk, sz > 8 ? 8 : sz);
        if (sz & 1u) { uint8_t zero = 0; poke(z, base + off + sz, &zero, 1); }
        off += (sz + 1u) & ~1u;
    }
    /* 戻り番地（番兵）と PSW */
    blk[0] = (uint8_t)z->sentinel;
    blk[1] = (uint8_t)(z->sentinel >> 8);
    blk[2] = (uint8_t)(z->sentinel >> 16);
    blk[3] = (uint8_t)rl78_get_reg(z->cpu, RL78_REG_PSW);
    poke(z, base - 4u, blk, 4);
    rl78_set_reg(z->cpu, RL78_REG_SP, base - 4u);
    st = stub_at(z, (uint32_t)f->addr);
    rl78_set_reg(z->cpu, RL78_REG_PC, st ? st->stub : (uint32_t)f->addr);
    snprintf(what, sizeof what, "関数 %s", f->name);
    if (run_to(z, z->sentinel, z->max_insns, what, err, errlen)) {
        /* 次の呼び出しに備えて、CPU を止まった場所から引き離す */
        rl78_set_reg(z->cpu, RL78_REG_SP, z->call_sp);
        return -1;
    }
    *ret_raw = 0;
    if (!f->ret || f->ret->kind == SIM_T_VOID) return 0;
    if (has_hidden) {
        uint8_t r[8];
        uint64_t v = 0;
        int k;
        rl78_peek(z->cpu, retbuf, r, rsize > 8 ? 8 : rsize);
        for (k = (int)(rsize > 8 ? 8 : rsize) - 1; k >= 0; k--) v = (v << 8) | r[k];
        *ret_raw = v;
    } else if (rsize == 1) {
        *ret_raw = rl78_get_reg(z->cpu, RL78_REG_A);
    } else if (rsize == 2) {
        *ret_raw = rl78_get_reg(z->cpu, RL78_REG_AX);
    } else if (rsize == 3) {
        *ret_raw = ((uint64_t)rl78_get_reg(z->cpu, RL78_REG_C) << 16) | rl78_get_reg(z->cpu, RL78_REG_AX);
    } else if (is_far_ptr(f->ret)) {
        *ret_raw = ((uint64_t)(rl78_get_reg(z->cpu, RL78_REG_A) & 0x0Fu) << 16) | rl78_get_reg(z->cpu, RL78_REG_DE);
    } else {
        *ret_raw = ((uint64_t)rl78_get_reg(z->cpu, RL78_REG_BC) << 16) | rl78_get_reg(z->cpu, RL78_REG_AX);
    }
    (void)i;
    return 0;
}

/* ================================================================ sim_backend の実装 */

static size_t b_var_count(sim_backend *b) { return ((be_t *)b->impl)->st.nvars; }
static const sim_var *b_var_at(sim_backend *b, size_t i)
{
    be_t *z = b->impl;
    return i < z->st.nvars ? &z->st.vars[i] : NULL;
}
static size_t b_func_count(sim_backend *b) { return ((be_t *)b->impl)->st.nfuncs; }
static const sim_func *b_func_at(sim_backend *b, size_t i)
{
    be_t *z = b->impl;
    return i < z->st.nfuncs ? &z->st.funcs[i].f : NULL;
}

static int range_ok(uint64_t addr, size_t n, char *err, size_t errlen)
{
    if (addr >= 0x100000u || n > 0x100000u || addr + n > 0x100000u) {
        snprintf(err, errlen, "アドレス 0x%llX（%zu バイト）は RL78 のアドレス空間（0x00000〜0xFFFFF）の外です", (unsigned long long)addr, n);
        return 0;
    }
    return 1;
}

static int b_read(sim_backend *b, uint64_t addr, void *buf, size_t n, char *err, size_t errlen)
{
    be_t *z = b->impl;
    if (!range_ok(addr, n, err, errlen)) return -1;
    rl78_peek(z->cpu, (uint32_t)addr, buf, n);
    return 0;
}

static int b_write(sim_backend *b, uint64_t addr, const void *buf, size_t n, char *err, size_t errlen)
{
    be_t *z = b->impl;
    size_t i;
    const uint8_t *p = buf;
    if (!range_ok(addr, n, err, errlen)) return -1;
    for (i = 0; i < n; i++) {
        uint32_t a = (uint32_t)addr + (uint32_t)i;
        /* SP/PSW/CS/ES/PMC は CPU のレジスタなのでバス経由で書く */
        if (a >= 0xFFFF8u) rl78_write8(z->cpu, a, p[i]);
        else rl78_load(z->cpu, a, p + i, 1);
    }
    return 0;
}

static int b_alloc(sim_backend *b, size_t size, uint64_t *addr, char *err, size_t errlen)
{
    be_t *z = b->impl;
    uint32_t a = (z->heap_cur + 1u) & ~1u;
    static const uint8_t zero[256];
    size_t k;
    if (!z->heap_hi || size > z->heap_hi - a) {
        if (!z->heap_hi) snprintf(err, errlen, "$ptr 用のヒープがありません。sim.cfg の heap = 開始-終了 で RAM の空きを指定してください");
        else snprintf(err, errlen, "$ptr 用のヒープ（0x%05X〜0x%05X）が足りません（%zu バイト要求）。sim.cfg の heap で広げられます",
                      z->heap_lo, z->heap_hi - 1, size);
        return -1;
    }
    for (k = 0; k < size; k += sizeof zero) rl78_load(z->cpu, a + (uint32_t)k, zero, size - k < sizeof zero ? size - k : sizeof zero);
    z->heap_cur = a + (uint32_t)size;
    *addr = a;
    return 0;
}

static int b_reset(sim_backend *b, char *err, size_t errlen)
{
    be_t *z = b->impl;
    (void)err;
    (void)errlen;
    rl78_snapshot_restore(z->cpu, z->snap);
    z->heap_cur = z->heap_lo;
    return 0;
}

static uint64_t b_ptr_to_addr(sim_backend *b, const sim_type *pt, uint64_t v)
{
    (void)b;
    if (pt && pt->size <= 2) {
        if (pt->target && pt->target->kind == SIM_T_FUNC) return v & 0xFFFFu;  /* near 関数ポインタ */
        return 0xF0000u | (v & 0xFFFFu);                                       /* near データポインタ */
    }
    return v & 0xFFFFFu;                                                       /* far */
}

static void b_exec_stats(sim_backend *b, sim_exec_stats *s)
{
    be_t *z = b->impl;
    s->insns = z->tot_insns;
    s->clocks = z->tot_clocks;
    s->calls = z->ncalls;
    s->startup_insns = z->startup_insns;
}

static const char *sect_type_name(uint32_t t)
{
    switch (t) {
    case 0: return "NULL";
    case RL78_SHT_PROGBITS: return "PROGBITS";
    case RL78_SHT_SYMTAB: return "SYMTAB";
    case RL78_SHT_STRTAB: return "STRTAB";
    case RL78_SHT_NOBITS: return "NOBITS";
    default: return "?";
    }
}

static void b_print_info(sim_backend *b, FILE *out)
{
    be_t *z = b->impl;
    int i, v;
    fprintf(out, "ファイル: %s\n", z->abs_path);
    fprintf(out, "ELF: 32 ビット リトルエンディアン、e_machine %u%s、種類 %u（%s）、エントリ 0x%05X、e_flags 0x%X\n",
            z->elf.machine, z->elf.machine == 197 ? "（RL78）" : "（RL78 ではない）", z->elf.etype,
            z->elf.etype == 2 ? "EXEC" : "?", z->elf.entry, z->elf.flags);
    fprintf(out, "設定ファイル: %s\n", z->cfg.path ? z->cfg.path : "なし");
    fprintf(out, "DWARF: CU %d 個（", z->st.ncu);
    for (v = 2, i = 0; v <= 5; v++) if (z->st.cu_versions[v]) fprintf(out, "%sバージョン %d: %d", i++ ? "、" : "", v, z->st.cu_versions[v]);
    fprintf(out, "%s）、飛ばした CU %d 個%s%s%s\n", i ? "" : "なし", z->st.nskipped_cu, z->st.producer ? "、" : "",
            z->st.producer ? z->st.producer : "", z->st.dwarf_ok ? "" : "。デバッグ情報がないので .symtab / .map のアドレスだけ使います");
    fprintf(out, "コア: %s、HALT: %s、STOP: %s\n", z->core_desc,
            z->cc.halt_policy == RL78_SLEEP_IGNORE ? "読み飛ばす" : "止める", z->cc.stop_policy == RL78_SLEEP_IGNORE ? "読み飛ばす" : "止める");
    if (z->cc.mirror_end) fprintf(out, "ミラー: 0x%05X〜0x%05X <- 0x%05X〜（PMC.MAA=1 で +0x10000）\n", z->cc.mirror_start, z->cc.mirror_end - 1, z->cc.mirror_src);
    else fprintf(out, "ミラー: なし\n");
    fprintf(out, "RAM: 0x%05X〜0x%05X", z->ram_lo, z->ram_hi - 1);
    if (z->have_stack) fprintf(out, "、スタック: 0x%05X〜0x%05X", z->stack_lo, z->stack_hi - 1);
    fprintf(out, "、呼び出し時の SP: 0x%05X\n", z->call_sp);
    if (z->heap_hi) fprintf(out, "ヒープ（$ptr）: 0x%05X〜0x%05X（%s）\n", z->heap_lo, z->heap_hi - 1, z->heap_desc);
    else fprintf(out, "ヒープ（$ptr）: %s\n", z->heap_desc);
    fprintf(out, "戻り番地の番兵: 0x%05X\n", z->sentinel);
    fprintf(out, "初期設定終了: %s（0x%05X）。スタートアップ %llu 命令、%llu クロック（%.3f ms @ %.0f Hz）\n", z->init_end_name,
            z->init_end, (unsigned long long)z->startup_insns, (unsigned long long)z->startup_clocks,
            (double)z->startup_clocks / b->cpu_hz * 1e3, b->cpu_hz);
    fprintf(out, "1 回の呼び出しの命令数の上限: %llu\n", (unsigned long long)z->max_insns);
    if (!z->nstubs) fprintf(out, "スタブ: なし\n");
    for (i = 0; i < z->nstubs; i++)
        fprintf(out, "スタブ: %s（0x%05X）-> %s（0x%05X）%s\n", z->stubs[i].oname, z->stubs[i].orig, z->stubs[i].sname,
                z->stubs[i].stub, z->stubs[i].patched ? "（先頭を BR !! に書き換え）" : "");
    if (z->have_map)
        fprintf(out, "リンクマップ: %s（セクション %d 件一致 / %d 件不一致、シンボル %d 件一致 / %d 件不一致）\n", z->map_path,
                z->map_sect_ok, z->map_sect_ng, z->map_sym_ok, z->map_sym_ng);
    else
        fprintf(out, "リンクマップ: なし\n");
    fprintf(out, "セクション:\n");
    for (i = 0; i < z->elf.nsect; i++) {
        const rl78_sect *s = &z->elf.sect[i];
        if (!s->name[0]) continue;
        fprintf(out, "  %-16s %-8s 0x%05X %6u %s%s%s\n", s->name, sect_type_name(s->type), s->addr, s->size,
                (s->flags & RL78_SHF_ALLOC) ? "A" : "", (s->flags & RL78_SHF_WRITE) ? "W" : "", (s->flags & RL78_SHF_EXEC) ? "X" : "");
    }
    if (z->w.n) {
        fprintf(out, "警告: %d 件%s\n", z->w.n, z->w.verbose ? "" : "（--verbose で表示）");
    }
}

static void be_free(be_t *z)
{
    if (!z) return;
    if (z->snap) rl78_snapshot_free(z->snap);
    if (z->cpu) rl78_destroy(z->cpu);
    rl78_elf_free(&z->elf);
    rl78_symtab_free(&z->st);
    rl78_map_free(&z->map);
    rl78_warns_free(&z->w);
    sim_cfg_free(&z->cfg);
    free(z->abs_path);
    free(z->map_path);
    free(z);
}

static void b_close(sim_backend *b)
{
    be_free(b->impl);
    free(b);
}

/* ================================================================ 開く */

static int setup_core(be_t *z, char *err, size_t errlen)
{
    const char *s = sim_cfg_get(&z->cfg, "core");
    rl78_core core = RL78_CORE_S3;
    int mdu = 1, on;
    int is_auto = 1;
    if (s && strcmp(s, "auto") != 0) {
        if (!strcmp(s, "S1") || !strcmp(s, "s1")) { core = RL78_CORE_S1; mdu = 0; }
        else if (!strcmp(s, "S2") || !strcmp(s, "s2")) { core = RL78_CORE_S2; mdu = 1; }
        else if (!strcmp(s, "S3") || !strcmp(s, "s3")) { core = RL78_CORE_S3; mdu = 0; }
        else { snprintf(err, errlen, "sim.cfg: core = %s は分かりません（auto / S1 / S2 / S3）", s); return -1; }
        is_auto = 0;
    }
    if ((s = sim_cfg_get(&z->cfg, "mdu"))) {
        if (parse_onoff(s, &on)) { snprintf(err, errlen, "sim.cfg: mdu = %s は on / off にしてください", s); return -1; }
        mdu = on;
    }
    rl78_default_config(&z->cc, core);
    z->cc.mdu = mdu;
    z->cc.halt_policy = RL78_SLEEP_IGNORE;
    z->cc.stop_policy = RL78_SLEEP_IGNORE;
    if ((s = sim_cfg_get(&z->cfg, "halt")) && !strcmp(s, "stop")) z->cc.halt_policy = RL78_SLEEP_RETURN;
    if ((s = sim_cfg_get(&z->cfg, "stop")) && !strcmp(s, "stop")) z->cc.stop_policy = RL78_SLEEP_RETURN;
    if ((s = sim_cfg_get(&z->cfg, "rom_size"))) {
        uint64_t v;
        if (parse_u64(s, &v)) { snprintf(err, errlen, "sim.cfg: rom_size = %s を読めません", s); return -1; }
        z->cc.rom_size = (uint32_t)v;
    }
    snprintf(z->core_desc, sizeof z->core_desc, "%s%s%s", is_auto ? "auto（S3 の命令" : (core == RL78_CORE_S1 ? "S1" : core == RL78_CORE_S2 ? "S2" : "S3"),
             is_auto ? (mdu ? " + 乗除算器 SFR）" : "）") : "", !is_auto && mdu ? " + 乗除算器 SFR" : "");
    /* ミラー */
    s = sim_cfg_get(&z->cfg, "mirror");
    if (s && !strcmp(s, "off")) {
        z->cc.mirror_end = 0;
    } else if (s && strcmp(s, "auto") != 0) {
        uint32_t lo, hi;
        char buf[128];
        const char *src = strchr(s, '@');
        snprintf(buf, sizeof buf, "%.*s", src ? (int)(src - s) : (int)strlen(s), s);
        if (parse_range(buf, &lo, &hi)) { snprintf(err, errlen, "sim.cfg: mirror = %s を読めません（例: mirror = 0xF3000-0xFBEFF または off）", s); return -1; }
        z->cc.mirror_start = lo;
        z->cc.mirror_end = hi;
        z->cc.mirror_src = lo - 0xF0000u;
        if (src) {
            uint64_t v;
            if (parse_u64(src + 1, &v)) { snprintf(err, errlen, "sim.cfg: mirror の @ の後（写し元）を読めません"); return -1; }
            z->cc.mirror_src = (uint32_t)v;
        }
    } else if (core == RL78_CORE_S1) {
        z->cc.mirror_start = 0xF8000u;
        z->cc.mirror_end = z->ram_lo < 0xFDF00u && z->ram_lo > 0xF8000u ? z->ram_lo : 0xFDF00u;
        z->cc.mirror_src = 0;
    } else {
        /* .const の置き場所からミラーの先頭を推す（その手前はデータフラッシュ） */
        uint32_t lo = 0xF2000u;
        const rl78_sect *c = rl78_elf_section(&z->elf, ".const");
        if (c && c->size && c->addr >= 0x1000u && c->addr < 0x10000u) lo = 0xF0000u + (c->addr & ~0xFFFu);
        if (lo < 0xF1000u) lo = 0xF1000u;
        z->cc.mirror_start = lo;
        z->cc.mirror_end = z->ram_lo > lo ? z->ram_lo : 0;
        z->cc.mirror_src = lo - 0xF0000u;
    }
    z->cpu = rl78_create(&z->cc);
    if (!z->cpu) { snprintf(err, errlen, "メモリが足りません"); return -1; }
    return 0;
}

static void load_image(be_t *z)
{
    int i, nload = 0;
    const char *s = sim_cfg_get(&z->cfg, "ram_fill");
    uint64_t fill = 0;
    if (s && parse_u64(s, &fill)) rl78_warn(&z->w, "sim.cfg: ram_fill = %s を読めません", s);
    {
        /* RAM の初期値（実機では不定） */
        static uint8_t buf[4096];
        uint32_t a;
        memset(buf, (int)(fill & 0xFFu), sizeof buf);
        for (a = z->ram_lo; a < z->ram_hi; a += sizeof buf) {
            uint32_t n = z->ram_hi - a < sizeof buf ? z->ram_hi - a : (uint32_t)sizeof buf;
            rl78_load(z->cpu, a, buf, n);
        }
    }
    for (i = 0; i < z->elf.nseg; i++) {
        const rl78_seg *g = &z->elf.seg[i];
        uint32_t dst;
        if (g->type != 1 || !g->filesz) continue; /* PT_LOAD */
        if ((uint64_t)g->offset + g->filesz > z->elf.len) {
            rl78_warn(&z->w, "ELF: プログラムヘッダ %d の内容がファイルの外にあるので読み込みません", i);
            continue;
        }
        dst = g->paddr ? g->paddr : g->vaddr;
        if (dst + g->filesz > 0x100000u) { rl78_warn(&z->w, "ELF: プログラムヘッダ %d（0x%X）が 1MB を超えます", i, dst); continue; }
        rl78_load(z->cpu, dst, z->elf.data + g->offset, g->filesz);
        nload++;
    }
    if (nload == 0) {
        /* プログラムヘッダがなければセクションから */
        for (i = 0; i < z->elf.nsect; i++) {
            const rl78_sect *x = &z->elf.sect[i];
            if (!(x->flags & RL78_SHF_ALLOC) || x->type == RL78_SHT_NOBITS || !x->size) continue;
            if (x->addr + x->size > 0x100000u) continue;
            rl78_load(z->cpu, x->addr, z->elf.data + x->offset, x->size);
        }
    }
}

static int add_stub(be_t *z, const char *orig, const char *stub, char *err, size_t errlen)
{
    uint32_t a1, a2;
    const char *n1, *n2;
    if (resolve_code(z, orig, &a1, &n1)) { snprintf(err, errlen, "sim.cfg: stub の置き換え元 '%s' が見つかりません", orig); return -1; }
    if (resolve_code(z, stub, &a2, &n2)) { snprintf(err, errlen, "sim.cfg: stub の置き換え先 '%s' が見つかりません", stub); return -1; }
    if (a1 == a2) return 0;
    {
        int i;
        for (i = 0; i < z->nstubs; i++) if (z->stubs[i].orig == a1) { z->stubs[i].stub = a2; z->stubs[i].sname = n2; return 0; }
    }
    if (z->nstubs >= MAX_STUBS) { snprintf(err, errlen, "スタブが多すぎます（%d 個まで）", MAX_STUBS); return -1; }
    z->stubs[z->nstubs].orig = a1;
    z->stubs[z->nstubs].stub = a2;
    z->stubs[z->nstubs].oname = n1;
    z->stubs[z->nstubs].sname = n2;
    z->stubs[z->nstubs].patched = 0;
    z->nstubs++;
    return 0;
}

static int setup_stubs(be_t *z, char *err, size_t errlen)
{
    int i, on = 0;
    const char *s = sim_cfg_get(&z->cfg, "stub_auto");
    if (s && parse_onoff(s, &on)) { snprintf(err, errlen, "sim.cfg: stub_auto = %s は on / off にしてください", s); return -1; }
    if (on) {
        size_t k;
        for (k = 0; k < z->st.nfuncs; k++) {
            const char *nm = z->st.funcs[k].f.name;
            if (strncmp(nm, "AMSTB_", 6) == 0 && nm[6] && find_func(z, nm + 6))
                if (add_stub(z, nm + 6, nm, err, errlen)) return -1;
        }
    }
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        char a[256], b[256];
        const char *colon;
        if (strcmp(e->key, "stub") != 0) continue;
        e->used = 1;
        colon = strchr(e->value, ':');
        if (!colon) { snprintf(err, errlen, "sim.cfg:%d: stub = 元の関数:スタブ関数 の形にしてください", e->line); return -1; }
        snprintf(a, sizeof a, "%.*s", (int)(colon - e->value), e->value);
        snprintf(b, sizeof b, "%s", colon + 1);
        {
            char *p = a + strlen(a);
            while (p > a && p[-1] == ' ') *--p = 0;
            p = b;
            while (*p == ' ') memmove(p, p + 1, strlen(p));
        }
        if (add_stub(z, a, b, err, errlen)) return -1;
    }
    /* ブレークポイント（番兵 1 つと初期設定終了 1 つを残す）。足りなければ BR !!stub を書き込む */
    for (i = 0; i < z->nstubs; i++) {
        stub_t *t = &z->stubs[i];
        if (i < RL78_MAX_BREAKPOINTS - 2) {
            if (rl78_add_breakpoint(z->cpu, t->orig) == 0) continue;
        }
        {
            uint8_t br[4];
            const rl78_funcinfo *fi = NULL;
            size_t k;
            for (k = 0; k < z->st.nfuncs; k++) if (z->st.funcs[k].f.addr == t->orig) fi = &z->st.funcs[k];
            if (!fi || (fi->size && fi->size < 4)) {
                snprintf(err, errlen, "スタブ %s -> %s: ブレークポイントが足りず、元の関数が短いので書き換えもできません", t->oname, t->sname);
                return -1;
            }
            br[0] = 0xEC; /* BR !!addr20 */
            br[1] = (uint8_t)t->stub;
            br[2] = (uint8_t)(t->stub >> 8);
            br[3] = (uint8_t)(t->stub >> 16);
            rl78_load(z->cpu, t->orig, br, 4);
            t->patched = 1;
        }
    }
    return 0;
}

static void check_map(be_t *z)
{
    int i;
    for (i = 0; i < z->map.nsect; i++) {
        const rl78_mapsect *m = &z->map.sect[i];
        const rl78_sect *s = rl78_elf_section(&z->elf, m->name);
        if (!s) continue;
        if (s->addr == m->start && s->size == m->size) z->map_sect_ok++;
        else {
            z->map_sect_ng++;
            rl78_warn(&z->w, "リンクマップと ELF でセクション %s の位置が違います（map 0x%X+%u、ELF 0x%X+%u）", m->name, m->start, m->size, s->addr, s->size);
        }
    }
    for (i = 0; i < z->map.nsym; i++) {
        const rl78_mapsym *m = &z->map.sym[i];
        const char *name, *func;
        int st, found = 0, same = 0;
        size_t k;
        rl78_split_symname(&z->st, m->name, &name, &func, &st);
        for (k = 0; k < z->st.nvars; k++) if (!strcmp(z->st.vars[k].name, name)) { found = 1; if (z->st.vars[k].addr == m->addr) same = 1; }
        for (k = 0; k < z->st.nfuncs; k++) if (!strcmp(z->st.funcs[k].f.name, name)) { found = 1; if (z->st.funcs[k].f.addr == m->addr) same = 1; }
        if (!found) continue;
        if (same) z->map_sym_ok++;
        else {
            z->map_sym_ng++;
            rl78_warn(&z->w, "リンクマップのシンボル %s（0x%X）がデバッグ情報のアドレスと違います", m->name, m->addr);
        }
    }
}

static const char *const KNOWN_KEYS[] = {
    "abs", "map", "core", "mdu", "mirror", "rom_size", "ram", "heap", "sentinel", "call_sp", "init_end", "func",
    "stub", "stub_auto", "cpu_hz", "max_insns", "startup_max_insns", "halt", "stop", "ram_fill", "bitfield_order", NULL
};

sim_backend *sim_backend_open(const sim_open_opts *o, char *err, size_t errlen)
{
    be_t *z = sim_xcalloc(1, sizeof *z);
    sim_backend *b;
    const char *s;
    char cerr[1024];
    rl78_dwarf_opts dopt;
    int i;
    z->w.verbose = o->verbose;
    sim_cfg_init(&z->cfg);
    /* 設定ファイル: --cfg、なければ .abs と同じフォルダの sim.cfg、どちらもなければカレントの sim.cfg */
    {
        char *cp = NULL;
        int rc;
        if (o->cfg_path) cp = sim_xstrdup(o->cfg_path);
        else if (o->abs_path) {
            const char *bn = sim_basename(o->abs_path);
            char *dir = bn > o->abs_path ? sim_xstrndup(o->abs_path, (size_t)(bn - o->abs_path)) : sim_xstrdup("");
            cp = sim_path_join(dir[0] ? dir : ".", "sim.cfg");
            free(dir);
        } else cp = sim_xstrdup("sim.cfg");
        rc = sim_cfg_load(&z->cfg, cp, cerr, sizeof cerr);
        if (rc == -2) { snprintf(err, errlen, "%s", cerr); free(cp); be_free(z); return NULL; }
        if (rc == -1 && o->cfg_path) { snprintf(err, errlen, "設定ファイル %s を読めません", cp); free(cp); be_free(z); return NULL; }
        free(cp);
    }
    if (o->abs_path) z->abs_path = sim_xstrdup(o->abs_path);
    else if ((s = sim_cfg_get(&z->cfg, "abs"))) z->abs_path = sim_cfg_path(&z->cfg, s);
    else {
        snprintf(err, errlen, "ロードモジュールが指定されていません。--abs <X.abs> を付けるか、sim.cfg に abs = <X.abs> を書いてください");
        be_free(z);
        return NULL;
    }
    if (rl78_elf_load(&z->elf, z->abs_path, err, errlen)) { be_free(z); return NULL; }
    if (z->elf.machine != 197)
        rl78_warn(&z->w, "ELF の e_machine が %u です（RL78 は 197）。RL78 として読みます", z->elf.machine);
    /* シンボル */
    memset(&dopt, 0, sizeof dopt);
    if ((s = sim_cfg_get(&z->cfg, "bitfield_order")) && !strcmp(s, "lsb")) dopt.bitfield_lsb = 1;
    for (i = 0; i < z->cfg.n; i++) {
        if (strncmp(z->cfg.e[i].key, "addr_class.", 11) == 0) {
            int cls = atoi(z->cfg.e[i].key + 11), sz = atoi(z->cfg.e[i].value);
            z->cfg.e[i].used = 1;
            if (cls >= 0 && cls < 16 && (sz == 2 || sz == 4)) dopt.addr_class_size[cls] = sz;
            else rl78_warn(&z->w, "sim.cfg:%d: addr_class.N = 2 か 4 にしてください", z->cfg.e[i].line);
        }
    }
    rl78_symtab_init(&z->st);
    rl78_dwarf_read(&z->st, &z->elf, &dopt, &z->w);
    rl78_symtab_from_elf(&z->st, &z->elf, &z->w);
    /* リンクマップ */
    if ((s = sim_cfg_get(&z->cfg, "map"))) z->map_path = sim_cfg_path(&z->cfg, s);
    else {
        size_t n = strlen(z->abs_path);
        const char *dot = strrchr(sim_basename(z->abs_path), '.');
        size_t stem = dot ? (size_t)(dot - z->abs_path) : n;
        z->map_path = sim_xmalloc(stem + 5);
        memcpy(z->map_path, z->abs_path, stem);
        strcpy(z->map_path + stem, ".map");
    }
    if (rl78_map_load(&z->map, z->map_path) == 0) {
        z->have_map = 1;
        check_map(z);
        rl78_symtab_from_map(&z->st, &z->map, &z->w);
    } else if (sim_cfg_get(&z->cfg, "map")) {
        rl78_warn(&z->w, "リンクマップ %s を読めません", z->map_path);
    }
    if (!z->st.dwarf_ok) rl78_warn(&z->w, "デバッグ情報（DWARF）がないか読めませんでした。変数は 'name:バイト数' で指定してください");

    layout(z);
    if (setup_core(z, err, errlen)) { be_free(z); return NULL; }
    choose_sentinel(z);
    choose_heap(z);
    z->heap_cur = z->heap_lo;
    load_image(z);
    rl78_reset(z->cpu);
    {
        uint8_t loop[2] = {0xEF, 0xFE}; /* BR $$（番兵の中身。ここで止めるので実行はされない） */
        rl78_load(z->cpu, z->sentinel, loop, 2);
    }
    z->max_insns = 100000000ULL;
    z->startup_max = 200000000ULL;
    if ((s = sim_cfg_get(&z->cfg, "max_insns"))) { uint64_t v; if (!parse_u64(s, &v) && v) z->max_insns = v; else rl78_warn(&z->w, "sim.cfg: max_insns = %s を読めません", s); }
    if ((s = sim_cfg_get(&z->cfg, "startup_max_insns"))) { uint64_t v; if (!parse_u64(s, &v) && v) z->startup_max = v; else rl78_warn(&z->w, "sim.cfg: startup_max_insns = %s を読めません", s); }

    if (setup_stubs(z, err, errlen)) { be_free(z); return NULL; }
    /* 初期設定終了アドレス */
    s = sim_cfg_get(&z->cfg, "init_end");
    if (resolve_code(z, s ? s : "main", &z->init_end, &z->init_end_name)) {
        if (s) snprintf(err, errlen, "sim.cfg: init_end = %s が見つかりません", s);
        else snprintf(err, errlen, "初期設定終了アドレスが分かりません（main がありません）。sim.cfg の init_end = 関数名 または 0xアドレス で指定してください");
        be_free(z);
        return NULL;
    }
    if (rl78_add_breakpoint(z->cpu, z->sentinel) || rl78_add_breakpoint(z->cpu, z->init_end)) {
        snprintf(err, errlen, "ブレークポイントを置けません");
        be_free(z);
        return NULL;
    }
    {
        char what[300];
        snprintf(what, sizeof what, "スタートアップ（リセットから %s まで）", z->init_end_name);
        if (run_to(z, z->init_end, z->startup_max, what, err, errlen)) {
            size_t n = strlen(err);
            snprintf(err + n, errlen - n, "。sim.cfg の init_end で止める場所を変えられます");
            be_free(z);
            return NULL;
        }
    }
    z->startup_insns = z->tot_insns;
    z->startup_clocks = z->tot_clocks;
    if (!stub_at(z, z->init_end) && z->init_end != z->sentinel) rl78_remove_breakpoint(z->cpu, z->init_end);
    /* SP は 16 ビット（F0000H〜FFFFFH の下位）。呼び出しはスタートアップが main を呼んだときの SP から積む */
    z->call_sp = 0xF0000u | (rl78_get_reg(z->cpu, RL78_REG_SP) & 0xFFFEu);
    if ((s = sim_cfg_get(&z->cfg, "call_sp"))) {
        uint64_t v;
        if (!parse_u64(s, &v)) z->call_sp = 0xF0000u | ((uint32_t)v & 0xFFFEu);
        else rl78_warn(&z->w, "sim.cfg: call_sp = %s を読めません", s);
    }
    if (z->call_sp < z->ram_lo + 64u || z->call_sp > 0xFFEE0u) {
        rl78_warn(&z->w, "初期設定終了時の SP（0x%05X）が RAM の外なので、呼び出しには 0x%05X を使います", z->call_sp,
                  z->have_stack ? (z->stack_hi & 0xFFFFEu) : 0xFFE20u);
        z->call_sp = z->have_stack ? (z->stack_hi & 0xFFFFEu) : 0xFFE20u;
    }
    z->snap = rl78_snapshot_create(z->cpu);
    if (!z->snap) { snprintf(err, errlen, "メモリが足りません"); be_free(z); return NULL; }

    for (i = 0; i < z->cfg.n; i++) {
        int k, known = 0;
        if (z->cfg.e[i].used) continue;
        for (k = 0; KNOWN_KEYS[k]; k++) if (!strcmp(KNOWN_KEYS[k], z->cfg.e[i].key)) known = 1;
        if (!known) rl78_warn(&z->w, "%s:%d: 知らない設定 '%s'", z->cfg.path ? z->cfg.path : "sim.cfg", z->cfg.e[i].line, z->cfg.e[i].key);
    }

    b = sim_xcalloc(1, sizeof *b);
    b->name = "rl78";
    b->ptr_size = 2;
    b->big_endian = 0;
    s = sim_cfg_get(&z->cfg, "func");
    b->default_func = s;
    b->var_count = b_var_count;
    b->var_at = b_var_at;
    b->func_count = b_func_count;
    b->func_at = b_func_at;
    b->read = b_read;
    b->write = b_write;
    b->call = b_call;
    b->alloc = b_alloc;
    b->reset = b_reset;
    b->lookup_var = NULL;
    b->close = b_close;
    b->ptr_to_addr = b_ptr_to_addr;
    b->exec_stats = b_exec_stats;
    b->print_info = b_print_info;
    b->cpu_hz = 32000000.0;
    if ((s = sim_cfg_get(&z->cfg, "cpu_hz"))) {
        double hz = atof(s);
        if (hz > 0) b->cpu_hz = hz;
        else rl78_warn(&z->w, "sim.cfg: cpu_hz = %s を読めません", s);
    }
    b->impl = z;
    if (z->w.n && !z->w.verbose) fprintf(stderr, "sim: 読み込みの警告が %d 件あります（--verbose で表示）\n", z->w.n);
    return b;
}
