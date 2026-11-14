/* rl78_backend.c 専用。診断用の状態は起動中だけ保持する。 */
#ifndef RL78_STARTUP_H
#define RL78_STARTUP_H

static int redirect(be_t *z, const stub_t *s, char *err, size_t errlen)
{
    uint32_t sp, pc;
    unsigned i;
    if (!s->skip) { rl78_set_reg(z->cpu, RL78_REG_PC, s->stub); return 0; }
    sp = rl78_get_reg(z->cpu, RL78_REG_SP);
    if ((sp & 1u) || sp > 0xFFFCu || (0xF0000u | sp) < z->ram_lo || (0xF0000u | sp) + 4 > z->ram_hi) {
        snprintf(err, errlen, "skip %s: 戻り番地の SP=0x%04X が RAM の外です", s->oname, sp);
        return -1;
    }
    pc = 0;
    for (i = 0; i < 3; i++) pc |= (uint32_t)rl78_peek8(z->cpu, 0xF0000u | (sp + i)) << (8 * i);
    if (pc > 0xFFFFFu) { snprintf(err, errlen, "skip %s: 戻り番地が20ビットの範囲外です", s->oname); return -1; }
    if (s->has_value) rl78_abi_set_ret(z->cpu, z->ca78k0r, s->ret, s->value);
    rl78_set_reg(z->cpu, RL78_REG_SP, (sp + 4u) & 0xFFFFu);
    rl78_set_reg(z->cpu, RL78_REG_PC, pc);
    return 0;
}

static int setup_startup_settings(be_t *z, char *err, size_t errlen)
{
    int i, j;
    for (i = 0; i < z->cfg.n; i++) {
        sim_cfg_entry *e = &z->cfg.e[i];
        char *a, *b, *c;
        uint64_t v, addr, n = 1;
        if (strcmp(e->key, "skip") && strcmp(e->key, "poke")) continue;
        e->used = 1;
        a = sim_xstrdup(e->value);
        b = strchr(a, ':');
        if (b) *b++ = 0;
        if (!strcmp(e->key, "poke")) {
            uint8_t bytes[4];
            c = b ? strchr(b, ':') : NULL;
            if (c) *c++ = 0;
            if (!b || sim_parse_uint(a, 0xFFFFF, &addr) || sim_parse_uint(b, UINT32_MAX, &v) ||
                (c && sim_parse_uint(c, 4, &n)) || !n || addr + n > 0x100000u ||
                (n < 4 && v >= (UINT64_C(1) << (8 * n)))) goto bad;
            for (j = 0; j < (int)n; j++) bytes[j] = (uint8_t)(v >> (8 * j));
            /* CPU内蔵SFR（SP/PSW等）はバス経由で設定する。通常のメモリはraw load。 */
            if (addr >= 0xFFF00u || (addr >= 0xF0000u && addr < 0xF0800u)) {
                if (n == 2 && !(addr & 1)) rl78_write16(z->cpu, (uint32_t)addr, (uint16_t)v);
                else for (j = 0; j < (int)n; j++) rl78_write8(z->cpu, (uint32_t)addr + (unsigned)j, bytes[j]);
            } else rl78_load(z->cpu, (uint32_t)addr, bytes, (size_t)n);
        } else {
            const sim_func *f;
            stub_t *t;
            char *end = a + strlen(a);
            while (end > a && isspace((unsigned char)end[-1])) *--end = 0;
            { const rl78_funcinfo *fi = find_func(z, a); f = fi ? &fi->f : NULL; }
            if (!f) { snprintf(err, errlen, "sim.cfg:%d: skip の関数 '%s' が見つかりません", e->line, a); free(a); return -1; }
            for (j = 0; j < z->nstubs; j++) if (z->stubs[j].orig == f->addr) {
                snprintf(err, errlen, "sim.cfg:%d: skip %s は stub / skip と重複しています", e->line, a); free(a); return -1;
            }
            if (b && (sim_parse_uint(b, UINT32_MAX, &v) || rl78_abi_check_skip(z->ca78k0r, f->ret, v))) goto bad;
            if (z->nstubs >= MAX_STUBS || z->nstubs >= RL78_MAX_BREAKPOINTS - 2 ||
                rl78_add_breakpoint(z->cpu, (uint32_t)f->addr)) {
                snprintf(err, errlen, "sim.cfg: skip %s のブレークポイントが足りません", a); free(a); return -1;
            }
            t = &z->stubs[z->nstubs++];
            memset(t, 0, sizeof *t);
            t->orig = (uint32_t)f->addr; t->oname = f->name; t->sname = "skip";
            t->skip = 1; t->has_value = b != NULL; t->value = b ? v : 0; t->ret = f->ret;
        }
        free(a);
        continue;
    bad:
        snprintf(err, errlen, "sim.cfg:%d: %s = %s を読めません（poke は20ビット番地:値[:1〜4バイト]、skip の戻り値は型が確定した整数・nearポインタのみ。型不明・範囲外・未確認のABIは対応外）", e->line, e->key, e->value);
        free(a);
        return -1;
    }
    return 0;
}

static void print_startup_settings(be_t *z, FILE *out)
{
    int i;
    for (i = 0; i < z->cfg.n; i++)
        if (!strcmp(z->cfg.e[i].key, "poke") || !strcmp(z->cfg.e[i].key, "skip"))
            fprintf(out, "%s = %s\n", z->cfg.e[i].key, z->cfg.e[i].value);
}

#define TRACE_WINDOW 256
#define TRACE_SFR_BYTES (0x800 + 0x100)
typedef struct {
    uint32_t pc, sp;
    uint64_t insn, repeats;
} trace_call;
typedef struct {
    uint32_t addr, pc;
    unsigned value;
    uint64_t count;
} trace_read;
typedef struct {
    uint32_t pc, next, read_addr, read_pc;
    unsigned read_value;
    uint64_t insn, read_insn;
    int taken, have_read;
} trace_branch;
typedef struct startup_trace {
    trace_call *head, *tail, pending;
    unsigned nh, nt;
    uint64_t calls;
    int have_pending;
    int conditional, transfer, insn_len;
    trace_branch branches[64], last_transfer;
    uint64_t nbranches;
    int have_transfer, ff_fault;
    uint32_t pc, base_sp, pcs[TRACE_WINDOW];
    uint64_t insn, steps, bus_count;
    struct { uint32_t addr, pc, value; uint64_t insn; } bus[2048];
    unsigned char written[TRACE_SFR_BYTES];
    int read_index[TRACE_SFR_BYTES];
    trace_read reads[TRACE_SFR_BYTES];
    unsigned nreads;
    const rl78_funcinfo **funcs;
    size_t nfuncs;
} startup_trace;

static int sfr_index(uint32_t a)
{
    if (a >= 0xF0000u && a < 0xF0800u) return (int)(a - 0xF0000u);
    if (a >= 0xFFF00u && a <= 0xFFFFFu) return (int)(0x800u + a - 0xFFF00u);
    return -1;
}

static void trace_hooks(be_t *z);
/* 書き込みフックは内蔵 SFR より優先されるため、一時解除して CPU 本来のバス動作に委譲する。
 * このバックエンドは他のフックを設置しない。診断終了時に全て解除する。 */
static uint32_t trace_read_hook(void *ctx, rl78_cpu *cpu, uint32_t addr, int size)
{
    be_t *z = ctx;
    startup_trace *t = z->trace;
    uint32_t v;
    int k;
    /* peek はコア本来の SFR/ミラー読出しを行い、フックは呼ばない。読出しに副作用はない。 */
    v = rl78_peek8(cpu, addr);
    if (size == 2) v |= (uint32_t)rl78_peek8(cpu, addr + 1) << 8;
    for (k = 0; k < size; k++) {
        int ix = sfr_index(addr + (unsigned)k), ri;
        trace_read *r;
        uint64_t bi = t->bus_count++ % 2048;
        t->bus[bi].addr = addr + (unsigned)k; t->bus[bi].pc = t->pc;
        t->bus[bi].value = (v >> (8 * k)) & 255u; t->bus[bi].insn = t->insn;
        if (ix < 0) continue;
        ri = t->read_index[ix];
        if (ri < 0) {
            ri = (int)t->nreads++;
            t->read_index[ix] = ri;
            r = &t->reads[ri];
            r->addr = addr + (unsigned)k; r->pc = t->pc; r->value = (v >> (8 * k)) & 255u;
        }
        if (ri >= 0) {
            r = &t->reads[ri];
            if (!t->written[ix]) r->count++;
        }
    }
    return v;
}
static void trace_write_hook(void *ctx, rl78_cpu *cpu, uint32_t addr, uint32_t v, int size)
{
    be_t *z = ctx;
    int k;
    for (k = 0; k < size; k++) {
        int ix = sfr_index(addr + (unsigned)k);
        if (ix >= 0) z->trace->written[ix] = 1;
    }
    rl78_clear_hooks(cpu);
    if (addr >= 0xFFFF0u && addr < 0xFFFF4u &&
        rl78_peek8(cpu, z->trace->pc) == 0xCE && rl78_peek8(cpu, z->trace->pc + 1) == 0xFB &&
        (rl78_peek8(cpu, z->trace->pc + 2) == 5 || rl78_peek8(cpu, z->trace->pc + 2) == 6)) {
        uint8_t b = (uint8_t)v;
        rl78_load(cpu, addr, &b, 1);
    } else if (size == 2) rl78_write16(cpu, addr, (uint16_t)v); else rl78_write8(cpu, addr, (uint8_t)v);
    trace_hooks(z);
}
static void trace_hooks(be_t *z)
{
    rl78_add_hook(z->cpu, 0, 0x100000, trace_read_hook, trace_write_hook, z);
}
static int trace_func_cmp(const void *a, const void *b)
{
    const rl78_funcinfo *x = *(const rl78_funcinfo *const *)a, *y = *(const rl78_funcinfo *const *)b;
    return x->f.addr < y->f.addr ? -1 : x->f.addr > y->f.addr;
}
static const rl78_funcinfo *trace_func(startup_trace *t, uint32_t pc)
{
    size_t lo = 0, hi = t->nfuncs;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (t->funcs[m]->f.addr <= pc) lo = m + 1; else hi = m;
    }
    return lo ? t->funcs[lo - 1] : NULL;
}
static void trace_location(be_t *z, uint32_t pc)
{
    const rl78_funcinfo *f = trace_func(z->trace, pc);
    printf("0x%05X %s+0x%X", pc, f ? f->f.name : "?", f ? (unsigned)(pc - f->f.addr) : 0);
}
static void trace_flush(startup_trace *t)
{
    if (!t->have_pending) return;
    if (t->calls < t->nh) t->head[t->calls] = t->pending;
    t->tail[t->calls % t->nt] = t->pending;
    t->calls++;
    t->have_pending = 0;
}
static void trace_before(be_t *z, uint32_t pc)
{
    startup_trace *t = z->trace;
    const rl78_funcinfo *f = trace_func(t, pc);
    uint32_t sp = rl78_get_reg(z->cpu, RL78_REG_SP);
    t->pc = pc; t->insn = z->tot_insns + 1;
    {
        char d[128], op[16];
        t->insn_len = rl78_disasm(z->cpu, pc, d, sizeof d);
        op[0] = 0; sscanf(d, "%15s", op);
        t->conditional = !strcmp(op, "BC") || !strcmp(op, "BNC") || !strcmp(op, "BZ") ||
            !strcmp(op, "BNZ") || !strcmp(op, "BH") || !strcmp(op, "BNH") ||
            !strcmp(op, "BT") || !strcmp(op, "BF") || !strcmp(op, "BTCLR") || !strncmp(op, "SK", 2);
        t->transfer = t->conditional || !strcmp(op, "BR") || !strncmp(op, "CALL", 4);
    }

    if (sp >= (z->ram_lo & 0xFFFFu) && sp <= 0xFEE0u && sp > t->base_sp) t->base_sp = sp;
    if (!f || f->f.addr != pc) return;
    if (t->have_pending && t->pending.pc == pc && t->pending.sp == sp) { t->pending.repeats++; return; }
    trace_flush(t);
    t->pending.pc = pc; t->pending.sp = sp; t->pending.insn = t->insn; t->pending.repeats = 1;
    t->have_pending = 1;
}
static void trace_after(be_t *z, uint32_t next)
{
    startup_trace *t = z->trace;
    trace_branch b;
    t->pcs[t->steps++ % TRACE_WINDOW] = t->pc;
    if (!t->transfer) return;
    memset(&b, 0, sizeof b);
    b.pc = t->pc; b.next = next; b.insn = t->insn;
    b.taken = next != ((t->pc + (unsigned)t->insn_len) & 0xFFFFFu);
    if (t->bus_count) {
        uint64_t ix = (t->bus_count - 1) % 2048;
        b.have_read = 1; b.read_addr = t->bus[ix].addr; b.read_value = t->bus[ix].value;
        b.read_pc = t->bus[ix].pc; b.read_insn = t->bus[ix].insn;
    }
    if (b.taken) { t->last_transfer = b; t->have_transfer = 1; }
    if (t->conditional) t->branches[t->nbranches++ % 64] = b;
}
static void trace_print_branch(be_t *z, const trace_branch *b)
{
    char d[160];
    disasm_at(z, b->pc, d, sizeof d);
    printf("  #%llu 0x%05X -> 0x%05X %s %s\n", (unsigned long long)b->insn,
           b->pc, b->next, b->taken ? "taken" : "not-taken", d);
    if (b->have_read)
        printf("    last read: 0x%05X = 0x%02X (PC=0x%05X, #%llu)\n",
               b->read_addr, b->read_value, b->read_pc, (unsigned long long)b->read_insn);
    else printf("    last read: none\n");
}
static void trace_begin(be_t *z, const sim_open_opts *o)
{
    startup_trace *t = sim_xcalloc(1, sizeof *t);
    size_t i;
    z->trace = t;
    t->nh = o->trace_head ? o->trace_head : 50;
    t->nt = o->trace_tail ? o->trace_tail : 100;
    t->head = sim_xcalloc(t->nh, sizeof *t->head); t->tail = sim_xcalloc(t->nt, sizeof *t->tail);
    t->nfuncs = z->st.nfuncs;
    t->funcs = sim_xcalloc(t->nfuncs, sizeof *t->funcs);
    for (i = 0; i < t->nfuncs; i++) t->funcs[i] = &z->st.funcs[i];
    qsort(t->funcs, t->nfuncs, sizeof *t->funcs, trace_func_cmp);
    for (i = 0; i < TRACE_SFR_BYTES; i++) t->read_index[i] = -1;
    /* poke は利用者が初期値を与えた番地なので未設定として扱わない。 */
    for (i = 0; i < (size_t)z->cfg.n; i++) if (!strcmp(z->cfg.e[i].key, "poke")) {
        char *a = sim_xstrdup(z->cfg.e[i].value), *b = strchr(a, ':'), *c;
        uint64_t addr = 0, n = 1, k;
        *b++ = 0; c = strchr(b, ':');
        sim_parse_uint(a, 0xFFFFF, &addr);
        if (c) sim_parse_uint(c + 1, 4, &n);
        for (k = 0; k < n; k++) { int ix = sfr_index((uint32_t)(addr + k)); if (ix >= 0) t->written[ix] = 1; }
        free(a);
    }
    trace_hooks(z);
}
static void trace_print_call(be_t *z, const trace_call *c)
{
    unsigned depth = c->sp && c->sp <= z->trace->base_sp ? (z->trace->base_sp - c->sp) / 4 : 0;
    printf("  #%llu %*s", (unsigned long long)c->insn, (int)(depth > 32 ? 64 : depth * 2), "");
    trace_location(z, c->pc);
    printf(" SP=0x%04X ×%llu\n", c->sp, (unsigned long long)c->repeats);
}
static void trace_end(be_t *z, int failed)
{
    startup_trace *t = z->trace;
    uint64_t i, start;
    uint32_t pc = rl78_get_reg(z->cpu, RL78_REG_PC), lo = 0xFFFFF, hi = 0;
    unsigned n = t->steps < TRACE_WINDOW ? (unsigned)t->steps : TRACE_WINDOW, repeats = 0;
    if (!failed) trace_before(z, pc);
    trace_flush(t);
    rl78_clear_hooks(z->cpu);
    printf("スタートアップ診断: %s（%llu 命令）\n呼び出し履歴（SP差/4による深さの推定。局所変数・PUSHでも深く見えます）:\n", failed ? "失敗" : "成功", (unsigned long long)z->tot_insns);
    for (i = 0; i < t->calls && i < t->nh; i++) trace_print_call(z, &t->head[i]);
    start = t->calls > t->nt ? t->calls - t->nt : 0;
    if (start > t->nh) printf("  … %llu 件省略 …\n", (unsigned long long)(start - t->nh));
    if (start < t->nh) start = t->nh;
    for (i = start; i < t->calls; i++) trace_print_call(z, &t->tail[i % t->nt]);
    printf("停止位置: "); trace_location(z, pc); printf("\n");
    for (i = RL78_REG_AX; i < RL78_REG_COUNT; i++) {
        static const char *names[] = {"AX", "BC", "DE", "HL", "PC", "SP", "PSW", "ES", "CS", "PMC"};
        printf("%s=0x%X%s", names[i - RL78_REG_AX], rl78_get_reg(z->cpu, (rl78_reg)i), i == RL78_REG_COUNT - 1 ? "\n" : " ");
    }
    printf("直前の命令（実行順、末尾32件。未実行の停止命令は別記）:\n");
    for (i = t->steps > 32 ? t->steps - 32 : 0; i < t->steps; i++) {
        char d[160]; uint32_t a = t->pcs[i % TRACE_WINDOW];
        disasm_at(z, a, d, sizeof d); printf("  0x%05X %s\n", a, d);
    }
    { char d[160]; disasm_at(z, pc, d, sizeof d); printf("停止命令: 0x%05X %s\n", pc, d); }
    if (failed && t->ff_fault) {
        printf("FFH 経路: 最後の分岐・CALL（停止命令までの間に通常命令を含む場合があります）\n");
        if (t->have_transfer) trace_print_branch(z, &t->last_transfer);
        printf("直近の条件分岐（最大64件。last read は直近の観測で、条件との因果は未確定）:\n");
        for (i = t->nbranches > 64 ? t->nbranches - 64 : 0; i < t->nbranches; i++)
            trace_print_branch(z, &t->branches[i % 64]);
    }
    printf("バックトレース（推定: SPから2バイト単位で最大64枠を探索、4バイトの戻り番地を復元。局所変数・PUSHを誤認する場合があります）:\n");
    for (i = 0; i < 64; i++) {
        uint32_t a = 0xF0000u | rl78_get_reg(z->cpu, RL78_REG_SP), ret;
        int k, valid = 0;
        a += (unsigned)i * 2;
        if (a < z->ram_lo || a + 4 > z->ram_hi || (z->have_stack && a + 4 > z->stack_hi)) break;
        ret = rl78_peek8(z->cpu, a) | (uint32_t)rl78_peek8(z->cpu, a + 1) << 8 | (uint32_t)rl78_peek8(z->cpu, a + 2) << 16;
        if (ret > 0xFFFFF || !ret || !func_name_at(z, ret)) continue;
        for (k = 1; k <= 4 && (uint32_t)k <= ret; k++) {
            char d[96];
            if (rl78_disasm(z->cpu, ret - (unsigned)k, d, sizeof d) == k && !strncmp(d, "CALL", 4)) valid = 1;
        }
        if (!valid) continue;
        printf("  SP+0x%02X -> ", (unsigned)i * 2); trace_location(z, ret); printf("（CALL直後の候補）\n");
        i++; /* この4バイトと重なる候補は探さない */
    }
    for (i = 0; i < n; i++) {
        uint32_t a = t->pcs[(t->steps - n + i) % TRACE_WINDOW];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        if (a == pc) repeats++;
    }
    if (failed && n >= 64 && hi - lo <= 256 && repeats >= 3) {
        printf("ループ候補: 0x%05X〜0x%05X（直近%u件、同一PC %u回） ", lo, hi, n, repeats); trace_location(z, lo); printf("\n");
        uint32_t shown[64];
        unsigned ns = 0, omitted = 0;
        uint64_t end = t->bus_count > 2048 ? t->bus_count - 2048 : 0;
        printf("ループ内で読んだメモリ・SFR（この番地の値が変わるのを待っている可能性があります）:\n");
        for (i = t->bus_count; i > end; i--) {
            uint64_t bi = (i - 1) % 2048;
            unsigned j;
            if (t->bus[bi].insn + n <= t->insn || t->bus[bi].pc < lo || t->bus[bi].pc > hi) continue;
            for (j = 0; j < ns; j++) if (shown[j] == t->bus[bi].addr) break;
            if (j < ns) continue;
            if (ns == 64) { omitted = 1; continue; }
            shown[ns++] = t->bus[bi].addr;
            printf("  0x%05X = 0x%02X 読出PC=0x%05X\n", t->bus[bi].addr, t->bus[bi].value, t->bus[bi].pc);
        }
        if (omitted) printf("  … 読出番地は64件まで表示 …\n");
    }
    printf("未設定SFRの読み出し（プログラムの書き込み前・poke未指定、初出順）:\n"
           "  周辺機能・リセット値は模擬せず通常0です。CPU内蔵レジスタ等は例外です。実機の値と違うと分岐が変わりえます。RAMは対象外。\n");
    for (i = 0; i < t->nreads; i++) {
        trace_read *r = &t->reads[i];
        if (!r->count) continue;
        printf("  0x%05X = 0x%02X 読出PC=", r->addr, r->value); trace_location(z, r->pc); printf(" %llu回\n", (unsigned long long)r->count);
    }
    free(t->head); free(t->tail); free(t->funcs); free(t); z->trace = NULL;
}
#endif
