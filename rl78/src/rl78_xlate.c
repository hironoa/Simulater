/*
 * rl78_xlate.c - translate RL78 machine code to C, and run translated code.
 *
 * Translation (rl78_xlate_unit / rl78_xlate_main) writes C source.  Each RL78
 * function becomes one C function ("chunk"): the instructions reachable from
 * its entry, one rl78_xl_exec1() call per instruction (see rl78_xlate_rt.h).
 * The caller compiles the source with clang and hands the result back with
 * rl78_xl_attach(); rl78_run() then uses it wherever it can.
 *
 * Everything the translator works out about control flow is a hint only:
 *   - a block start ("leader") is where translated code may be entered,
 *   - a jump to an address that is not a leader of the chunk leaves the chunk,
 *   - a pc with no translated code at all is executed by the interpreter.
 * A wrong hint costs speed, never correctness: the instruction semantics come
 * from the interpreter's own opcode switch.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rl78_xlate_rt.h"

#define XL_MAX_INSNS 6000     /* per function; the rest is left to the interpreter */

/* ---- output ----------------------------------------------------------------- */

typedef struct {
    rl78_xl_put put;
    void *ctx;
} out_t;

#if defined(__GNUC__) || defined(__clang__)
static void outf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void outf(out_t *o, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    o->put(o->ctx, buf, (size_t)n);
}

/* fixed text of any length (outf is limited to its buffer) */
static void outs(out_t *o, const char *text) { o->put(o->ctx, text, strlen(text)); }

/* ---- control-flow hints (from the disassembly text) ---------------------------- */

typedef struct {
    int len;
    int falls;          /* execution may continue at pc + len */
    int has_target;     /* target is a known code address (branch or call) */
    uint32_t target;
    int is_call;        /* the instruction after it is a return address */
    int is_skip;        /* may also continue after the next instruction */
} flow_t;

static void flow_of(rl78_cpu *c, uint32_t pc, flow_t *f)
{
    char d[80];
    const char *p;
    memset(f, 0, sizeof *f);
    f->len = rl78__insn_len(c, pc);
    f->falls = 1;
    rl78_disasm(c, pc, d, sizeof d);
    if ((p = strchr(d, '$')) != NULL) {            /* relative: "$0123AH", "$!0123AH" */
        if (p[1] == '!') p++;
        f->has_target = 1;
        f->target = (uint32_t)strtoul(p + 1, NULL, 16) & 0xFFFFFu;
    }
    if (!strncmp(d, "CALL", 4)) {
        f->is_call = 1;
        if (!f->has_target && (p = strrchr(d, '!')) != NULL) {  /* "CALL !0123H", "CALL !!01234H" */
            f->has_target = 1;
            f->target = (uint32_t)strtoul(p + 1, NULL, 16) & 0xFFFFFu;
        }
    } else if (!strncmp(d, "BR ", 3)) {
        f->falls = 0;
        if (!f->has_target && (p = strrchr(d, '!')) != NULL) {
            f->has_target = 1;
            f->target = (uint32_t)strtoul(p + 1, NULL, 16) & 0xFFFFFu;
        }
    } else if (!strncmp(d, "RET", 3) || !strncmp(d, "BRK", 3)) {
        f->falls = 0;
    } else if (!strncmp(d, "SK", 2)) {
        f->is_skip = 1;
    }
}

/* ---- one function ------------------------------------------------------------- */

/* scratch maps, cleared after each function through the visit list */
typedef struct {
    uint8_t *mark;      /* bit 0: instruction of this function, bit 1: leader */
    uint32_t *list;
    size_t n, cap;
    uint32_t *work;
    size_t nwork, capwork;
    uint32_t *touched;  /* every address whose mark is non-zero */
    size_t ntouched, captouched;
} scan_t;

static int push_u32(uint32_t **v, size_t *n, size_t *cap, uint32_t x)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        uint32_t *nv = (uint32_t *)realloc(*v, nc * sizeof **v);
        if (!nv) return -1;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int set_mark(scan_t *s, uint32_t a, unsigned bit)
{
    a &= 0xFFFFFu;
    if (!s->mark[a] && push_u32(&s->touched, &s->ntouched, &s->captouched, a)) return -1;
    s->mark[a] |= (uint8_t)bit;
    return 0;
}

static int is_entry(const uint8_t *entries, uint32_t a) { return (entries[a >> 3] >> (a & 7u)) & 1; }

/* Code that may be translated: inside the write-protected code flash. */
static int in_rom(const rl78_cpu *c, uint32_t a) { return a + 8u <= c->cfg.rom_size; }

static int scan_function(rl78_cpu *c, uint32_t entry, const uint8_t *entries, scan_t *s)
{
    s->n = 0;
    s->nwork = 0;
    if (push_u32(&s->work, &s->nwork, &s->capwork, entry)) return -1;
    if (set_mark(s, entry, 2)) return -1;
    while (s->nwork) {
        uint32_t pc = s->work[--s->nwork];
        for (;;) {
            flow_t f;
            uint32_t next;
            if (s->mark[pc] & 1) break;                       /* already decoded */
            if (!in_rom(c, pc)) break;
            if (pc != entry && is_entry(entries, pc)) break;  /* another function: leave through the dispatcher */
            if (s->n >= XL_MAX_INSNS) break;
            flow_of(c, pc, &f);
            if (f.len <= 0) break;
            if (set_mark(s, pc, 1)) return -1;
            if (push_u32(&s->list, &s->n, &s->cap, pc)) return -1;
            next = pc + (uint32_t)f.len;
            if (f.has_target && !f.is_call) {
                /* a branch inside the function: its target starts a block */
                if (set_mark(s, f.target, 2)) return -1;
                if (push_u32(&s->work, &s->nwork, &s->capwork, f.target)) return -1;
            }
            if ((f.is_call || f.has_target) && set_mark(s, next, 2)) return -1;   /* return address / not-taken side */
            if (f.is_skip) {
                uint32_t after = next + (uint32_t)rl78__insn_len(c, next);
                if (set_mark(s, after, 2)) return -1;
                if (push_u32(&s->work, &s->nwork, &s->capwork, after)) return -1;
            }
            if (!f.falls) break;
            pc = next;
        }
    }
    qsort(s->list, s->n, sizeof *s->list, cmp_u32);
    return 0;
}

static void scan_clear(scan_t *s)
{
    size_t i;
    for (i = 0; i < s->ntouched; i++) s->mark[s->touched[i]] = 0;
    s->ntouched = 0;
}

/* 1 if rl78_xlate_unit() emits a C function for this entry (in its unit, which may be another file) */
static int has_function(rl78_cpu *c, const uint8_t *entries, uint32_t a)
{
    flow_t f;
    if (a >= RL78_MEM_SIZE || !is_entry(entries, a) || !in_rom(c, a)) return 0;
    flow_of(c, a, &f);
    return f.len > 0;
}

/* The functions of the unit being translated.  A function is written as two C functions:
     xl_iNNNNN  the body.  The CPU state it works on (registers pointer, flags, clock, counter, pc)
                comes in and goes out by value, so that a caller in the same C file can run it
                without storing the state in the CPU and loading it again; a small one that calls
                nothing is inlined into its callers and the call costs nothing at all.
     rl78_xl_fNNNNN  what the dispatcher (and a caller in another C file) calls: loads the state
                from the CPU, runs the body, stores it back. */
typedef struct {
    uint32_t entry;
    int inl;    /* small and calls nothing: inlined into its callers */
    int copy;   /* belongs to another C file; this file has a private copy of the body to inline (no rl78_xl_f) */
} unit_fn;
#define XL_INLINE_INSNS 24u     /* a function with at most this many instructions and no call is inlined */

static const unit_fn *unit_find(const unit_fn *fn, size_t nfn, uint32_t a)
{
    size_t i;
    for (i = 0; i < nfn; i++) if (fn[i].entry == a) return &fn[i];
    return NULL;
}

static void emit_function(rl78_cpu *c, uint32_t entry, const uint8_t *entries, const unit_fn *fn, size_t nfn,
                          scan_t *s, out_t *o, rl78_xl_leaders *ld)
{
    size_t i;
    const uint8_t *img = c->mem;
    const unit_fn *self = unit_find(fn, nfn, entry);
    if (self && self->copy) ld = NULL;  /* the block starts are listed by the file the function belongs to */
    outf(o, "static %sxl_state xl_i%05X(rl78_cpu *C, xl_state s_, uint64_t lim)\n{\n",
         self && self->inl ? "inline __attribute__((always_inline)) " : "", (unsigned)entry);
    outf(o, "    uint8_t *const M = C->mem; uint8_t *R = s_.R; unsigned F = s_.F;\n");
    outf(o, "    uint64_t cyc = s_.cyc, n = s_.n; uint32_t pc = s_.pc; int st = 0;\n");
    outf(o, "dyn:\n    switch (pc) {\n");
    for (i = 0; i < s->n; i++)
        if (s->mark[s->list[i]] & 2) outf(o, "    case 0x%05X: goto L_%05X;\n", (unsigned)s->list[i], (unsigned)s->list[i]);
    outf(o, "    default: goto out;\n    }\n");
    for (i = 0; i < s->n; i++) {
        uint32_t pc = s->list[i], next;
        flow_t f;
        int k, nl;
        flow_of(c, pc, &f);
        next = pc + (uint32_t)f.len;
        nl = rl78__insn_len(c, next);
        if (s->mark[pc] & 2) {
            /* block start: all of the block must fit under the limit, and a breakpoint here is the interpreter's */
            size_t j = i + 1;
            uint32_t q = next;
            while (j < s->n && s->list[j] == q && !(s->mark[q] & 2)) { q += (uint32_t)rl78__insn_len(c, q); j++; }
            outf(o, "L_%05X:\n    if (n + %u > lim || M[0x%05Xu + RL78_TRAP_OFFSET]) { pc = 0x%05X; st = RL78_XL_MISS; goto out; }\n",
                 (unsigned)pc, (unsigned)(j - i), (unsigned)pc, (unsigned)pc);
            if (ld) rl78_xl_leaders_add(ld, pc, entry, 1);
        } else if (ld) {
            rl78_xl_leaders_add(ld, pc, entry, 0);
        }
        outf(o, "    st = rl78_xl_exec1(C, M, &R, &F, &cyc, &pc, &n, 0x%05X", (unsigned)pc);
        for (k = 0; k < 6; k++) outf(o, ", 0x%02X", (unsigned)img[pc + (uint32_t)k]);
        outf(o, ", %d);\n    if (st) goto out;\n", nl);
        /* CALL of a translated function: run it from here.  Going back to the dispatcher costs a
           table lookup, an indirect call and the entry switch for every call and every return; in
           code that calls a small function every few instructions that is most of the time.
           Whatever the callee returns (it came back, it left for another address, it stopped), the
           code after the call sorts it out as if the dispatcher had run it.  The callee checks the
           limit and the breakpoint at its entry.
           (A call of code inside this function, recursion for one, stays in this C function: below.) */
        if (f.is_call && f.has_target && (s->mark[f.target] & 3) != 3 && unit_find(fn, nfn, f.target)) {
            /* the callee is in this C file: hand the state over by value */
            const unit_fn *callee = unit_find(fn, nfn, f.target);
            outf(o, "    if (pc == 0x%05X%s) {\n"
                    "        xl_state s_ = {cyc, n, R, pc, F, 0};\n",
                 (unsigned)f.target, callee->inl ? "" : " && C->xl_depth < RL78_XL_MAX_DEPTH");
            if (!callee->inl) outf(o, "        C->xl_depth++;\n");
            outf(o, "        s_ = xl_i%05X(C, s_, lim);\n", (unsigned)f.target);
            if (!callee->inl) outf(o, "        C->xl_depth--;\n");
            outf(o, "        R = s_.R; F = s_.F; cyc = s_.cyc; n = s_.n; pc = s_.pc;\n"
                    "        if (s_.st) { st = s_.st; goto out; }\n"
                    "    }\n");
        } else if (f.is_call && f.has_target && (s->mark[f.target] & 3) != 3 && has_function(c, entries, f.target)) {
            /* the callee is in another C file: the state goes through the CPU */
            outf(o, "    if (pc == 0x%05X && C->xl_depth < RL78_XL_MAX_DEPTH) {\n"
                    "        int rl78_xl_f%05X(rl78_cpu *C, uint64_t *np, uint64_t lim);\n"
                    "        C->pc = pc; C->psw = (uint8_t)F; C->cycles = cyc; C->rb = R;\n"
                    "        C->xl_depth++;\n"
                    "        st = rl78_xl_f%05X(C, &n, lim);\n"
                    "        C->xl_depth--;\n"
                    "        F = C->psw; R = C->rb; cyc = C->cycles; pc = C->pc;\n"
                    "        if (st) goto out;\n"
                    "    }\n", (unsigned)f.target, (unsigned)f.target, (unsigned)f.target);
        }
#define HERE(t) ((s->mark[t] & 3) == 3)
        if (f.has_target && HERE(f.target)) outf(o, "    if (pc == 0x%05X) goto L_%05X;\n", (unsigned)f.target, (unsigned)f.target);
        if (f.is_skip && HERE(next + (uint32_t)nl))
            outf(o, "    if (pc == 0x%05X) goto L_%05X;\n", (unsigned)(next + (uint32_t)nl), (unsigned)(next + (uint32_t)nl));
#undef HERE
        if (i + 1 < s->n && s->list[i + 1] == next) outf(o, "    if (pc != 0x%05X) goto dyn;\n", (unsigned)next);
        else outf(o, "    goto dyn;\n");
    }
    outf(o, "out:\n    s_.cyc = cyc; s_.n = n; s_.R = R; s_.pc = pc; s_.F = F; s_.st = st;\n    return s_;\n}\n\n");
    if (self && self->copy) return;
    outf(o, "int rl78_xl_f%05X(rl78_cpu *C, uint64_t *np, uint64_t lim)\n{\n"
            "    xl_state s_ = {C->cycles, *np, C->rb, C->pc, C->psw, 0};\n"
            "    s_ = xl_i%05X(C, s_, lim);\n"
            "    C->pc = s_.pc; C->psw = (uint8_t)s_.F; C->cycles = s_.cyc; C->rb = s_.R; *np = s_.n;\n"
            "    return s_.st;\n}\n\n", (unsigned)entry, (unsigned)entry);
}

/* ---- public: translation --------------------------------------------------------- */

void rl78_xl_leaders_init(rl78_xl_leaders *ld) { memset(ld, 0, sizeof *ld); }

void rl78_xl_leaders_free(rl78_xl_leaders *ld)
{
    free(ld->v);
    memset(ld, 0, sizeof *ld);
}

int rl78_xl_leaders_add(rl78_xl_leaders *ld, uint32_t pc, uint32_t entry, int leader)
{
    if (ld->n == ld->cap) {
        size_t nc = ld->cap ? ld->cap * 2 : 1024;
        rl78_xl_addr *nv = (rl78_xl_addr *)realloc(ld->v, nc * sizeof *nv);
        if (!nv) { ld->failed = 1; return -1; }
        ld->v = nv;
        ld->cap = nc;
    }
    ld->v[ld->n].pc = pc;
    ld->v[ld->n].entry = entry;
    ld->v[ld->n].leader = leader;
    ld->n++;
    return 0;
}

int rl78_xlate_unit(rl78_cpu *c, const uint32_t *unit, size_t nunit, const uint8_t *entries,
                    rl78_xl_put put, void *ctx, rl78_xl_leaders *ld, size_t *ninsns)
{
    out_t o;
    scan_t s;
    size_t i, total = 0, nfn = 0, nown = 0, cap;
    unit_fn *fn;
    int rc = 0;
    const int s1 = c->cfg.core == RL78_CORE_S1;
    const int s3 = c->cfg.core == RL78_CORE_S3 || c->cfg.core == RL78_CORE_AUTO;
    uint32_t plain_lo, plain_hi;
    o.put = put;
    o.ctx = ctx;
    memset(&s, 0, sizeof s);
    s.mark = (uint8_t *)calloc(RL78_MEM_SIZE + 16u, 1);
    if (!s.mark) return -1;
    rl78_xl_plain_range(c, &plain_lo, &plain_hi);
    outf(&o, "/* generated by the RL78 translator: do not edit */\n#define RL78_XL_UNIT 1\n#define RL78_XL_S1 %d\n#define RL78_XL_S3 %d\n"
             "#define RL78_XL_PLAIN_LO 0x%05Xu\n#define RL78_XL_PLAIN_HI 0x%05Xu\n"
             "#include \"rl78_xlate_rt.h\"\n\n", s1, s3, (unsigned)plain_lo, (unsigned)plain_hi);
    /* first pass: which functions this file gets, and which of them are small leaves.  The list
       grows while it is walked: a small leaf of another file that a function here calls is added
       as a private copy, so that the call can be inlined across files too (most calls in real
       code go to another file, the files being cut by address). */
    cap = nunit ? nunit : 1;
    fn = (unit_fn *)malloc(cap * sizeof *fn);
    if (!fn) { free(s.mark); return -1; }
    for (i = 0; i < nunit; i++) {
        uint32_t e = unit[i] & 0xFFFFFu;
        if (!has_function(c, entries, e) || unit_find(fn, nfn, e)) continue;
        fn[nfn].entry = e; fn[nfn].inl = 0; fn[nfn].copy = 0; nfn++;
    }
    nown = nfn;
    for (i = 0; i < nfn && !rc; i++) {
        size_t k, ncalls = 0, ntarget = 0;
        uint32_t *target = NULL;
        if (scan_function(c, fn[i].entry, entries, &s)) { rc = -1; break; }
        if (i < nown) target = (uint32_t *)malloc((s.n ? s.n : 1) * sizeof *target);
        for (k = 0; k < s.n; k++) {
            flow_t f;
            flow_of(c, s.list[k], &f);
            if (!f.is_call) continue;
            ncalls++;
            if (target && f.has_target && (s.mark[f.target] & 3) != 3) target[ntarget++] = f.target;
        }
        fn[i].inl = !ncalls && s.n <= XL_INLINE_INSNS;
        if (i >= nown && !fn[i].inl) fn[i].entry = 0xFFFFFFFFu;     /* not worth a copy after all: drop it */
        scan_clear(&s);
        for (k = 0; k < ntarget && !rc; k++) {
            if (unit_find(fn, nfn, target[k]) || !has_function(c, entries, target[k])) continue;
            if (nfn == cap) {
                unit_fn *nf = (unit_fn *)realloc(fn, cap * 2 * sizeof *fn);
                if (!nf) { rc = -1; break; }
                fn = nf; cap *= 2;
            }
            fn[nfn].entry = target[k]; fn[nfn].inl = 0; fn[nfn].copy = 1; nfn++;
        }
        free(target);
    }
    for (i = 0; i < nfn && !rc; i++)
        if (fn[i].entry != 0xFFFFFFFFu)
            outf(&o, "static %sxl_state xl_i%05X(rl78_cpu *C, xl_state s_, uint64_t lim);\n",
                 fn[i].inl ? "inline __attribute__((always_inline)) " : "", (unsigned)fn[i].entry);
    outf(&o, "\n");
    for (i = 0; i < nfn && !rc; i++) {
        uint32_t e = fn[i].entry;
        if (e == 0xFFFFFFFFu) continue;
        if (scan_function(c, e, entries, &s)) { rc = -1; break; }
        if (s.n) emit_function(c, e, entries, fn, nfn, &s, &o, ld);
        if (!fn[i].copy) total += s.n;
        scan_clear(&s);
    }
    free(fn);
    if (ld && ld->failed) rc = -1;
    if (ninsns) *ninsns = total;
    free(s.mark);
    free(s.list);
    free(s.work);
    free(s.touched);
    return rc;
}

static int cmp_addr(const void *a, const void *b)
{
    const rl78_xl_addr *x = (const rl78_xl_addr *)a, *y = (const rl78_xl_addr *)b;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    if (x->leader != y->leader) return y->leader - x->leader;   /* a block start wins over "inside" */
    return x->entry < y->entry ? -1 : x->entry > y->entry;
}

int rl78_xlate_main(rl78_cpu *c, rl78_xl_leaders *ld, rl78_xl_put put, void *ctx)
{
    out_t o;
    size_t i, nlead = 0, ninside = 0;
    const int s1 = c->cfg.core == RL78_CORE_S1;
    const int s3 = c->cfg.core == RL78_CORE_S3 || c->cfg.core == RL78_CORE_AUTO;
    uint32_t plain_lo, plain_hi;
    uint32_t last = 0xFFFFFFFFu;
    o.put = put;
    o.ctx = ctx;
    qsort(ld->v, ld->n, sizeof *ld->v, cmp_addr);
    rl78_xl_plain_range(c, &plain_lo, &plain_hi);
    outf(&o, "/* generated by the RL78 translator: do not edit */\n#define RL78_XL_S1 %d\n#define RL78_XL_S3 %d\n"
             "#define RL78_XL_PLAIN_LO 0x%05Xu\n#define RL78_XL_PLAIN_HI 0x%05Xu\n"
             "#include \"rl78_xlate_rt.h\"\n\n", s1, s3, (unsigned)plain_lo, (unsigned)plain_hi);
    {
        /* declare each chunk once: collect distinct entries */
        uint32_t *es = (uint32_t *)malloc((ld->n ? ld->n : 1) * sizeof *es);
        size_t ne = 0;
        if (!es) return -1;
        for (i = 0; i < ld->n; i++) es[ne++] = ld->v[i].entry;
        qsort(es, ne, sizeof *es, cmp_u32);
        for (i = 0; i < ne; i++)
            if (i == 0 || es[i] != es[i - 1]) outf(&o, "int rl78_xl_f%05X(rl78_cpu *C, uint64_t *np, uint64_t lim);\n", (unsigned)es[i]);
        free(es);
    }
    /* one row per address: the first after sorting (a block start if any chunk has one there) */
    outf(&o, "\nstatic const rl78_xl_leader xl_leaders[] = {\n");
    for (i = 0; i < ld->n; i++) {
        if (ld->v[i].pc == last) continue;
        last = ld->v[i].pc;
        if (!ld->v[i].leader) continue;
        outf(&o, "    {0x%05Xu, rl78_xl_f%05X},\n", (unsigned)ld->v[i].pc, (unsigned)ld->v[i].entry);
        nlead++;
    }
    outf(&o, "    {0, 0}\n};\nstatic const uint32_t xl_inside[] = {\n");
    last = 0xFFFFFFFFu;
    for (i = 0; i < ld->n; i++) {
        /* inside some chunk: a breakpoint there would be run over by that chunk, whatever other chunks say */
        if (ld->v[i].leader || ld->v[i].pc == last) continue;
        last = ld->v[i].pc;
        outf(&o, "0x%05Xu,%s", (unsigned)ld->v[i].pc, (ninside % 8) == 7 ? "\n" : "");
        ninside++;
    }
    outf(&o, "0\n};\n\n");
    outs(&o,
         "#ifdef RL78_XL_DLL\n"
         "static const rl78_xl_host *xl_host;\n"
         "uint8_t rl78__rd8_slow(rl78_cpu *c, uint32_t a) { return xl_host->rd8_slow(c, a); }\n"
         "void rl78__wr8_slow(rl78_cpu *c, uint32_t a, uint8_t v) { xl_host->wr8_slow(c, a, v); }\n"
         "uint16_t rl78__rd16_slow(rl78_cpu *c, uint32_t a) { return xl_host->rd16_slow(c, a); }\n"
         "void rl78__wr16_slow(rl78_cpu *c, uint32_t a, uint16_t v) { xl_host->wr16_slow(c, a, v); }\n"
         "void rl78__write_macr(rl78_cpu *c, uint32_t v) { xl_host->write_macr(c, v); }\n"
         "/* the compiler may call these even in freestanding code */\n"
         "void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }\n"
         "void *memset(void *d, int v, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)v; return d; }\n"
         "#define XL_SET_HOST(h) (xl_host = (h))\n"
         "#else\n"
         "#define XL_SET_HOST(h) ((void)(h))\n"
         "#endif\n\n");
    outf(&o,
         "static const rl78_xl_module xl_module = {\n"
         "    RL78_XL_ABI, (uint32_t)sizeof(rl78_cpu), RL78_XL_S1, RL78_XL_S3, %uu, xl_leaders, %uu, xl_inside,\n"
         "    RL78_XL_PLAIN_LO, RL78_XL_PLAIN_HI\n};\n\n",
         (unsigned)nlead, (unsigned)ninside);
    outs(&o,
         "#if defined(_WIN32) && defined(RL78_XL_DLL)\n__declspec(dllexport)\n#elif defined(RL78_XL_DLL)\n__attribute__((visibility(\"default\")))\n#endif\n"
         "const rl78_xl_module *rl78_xl_module_get(const rl78_xl_host *host)\n{\n    XL_SET_HOST(host);\n    return &xl_module;\n}\n");
    return 0;
}

/* ---- public: running translated code ---------------------------------------------- */

static void recount_bp_inside(rl78_cpu *c)
{
    int i, n = 0;
    /* An address can start a block in one chunk and sit inside a block of another (two functions
       branching into the same code at different places): the second chunk would run over a breakpoint
       there, so "inside" is kept apart from the dispatch index. */
    if (c->xl_inside)
        for (i = 0; i < c->nbps; i++)
            if ((c->xl_inside[c->bplist[i] >> 3] >> (c->bplist[i] & 7u)) & 1) n++;
    c->xl_bp_inside = n;
}

void rl78__xl_breakpoint_changed(rl78_cpu *c, uint32_t addr, int added)
{
    (void)addr;
    (void)added;
    recount_bp_inside(c);
}

const rl78_xl_host *rl78_xl_host_get(void)
{
    static const rl78_xl_host host = {rl78__rd8_slow, rl78__wr8_slow, rl78__rd16_slow, rl78__wr16_slow, rl78__write_macr};
    return &host;
}

int rl78_set_call_profile(rl78_cpu *c, int on)
{
    if (!on) { free(c->callprof); c->callprof = NULL; return 0; }
    if (!c->callprof) c->callprof = (uint8_t *)calloc(RL78_MEM_SIZE / 8u, 1);
    return c->callprof ? 0 : -1;
}

const uint8_t *rl78_call_profile(const rl78_cpu *c) { return c->callprof; }

/* The bytes translated code depends on for the instruction at a (as the memory is now: attach before
   anything changes the code): the instruction itself, and the instruction after it, because the
   translation of a skip instruction has the length of the next instruction built in, whether or not
   that next instruction is translated. */
static void mark_insn_bytes(rl78_cpu *c, uint32_t a)
{
    int len = rl78__insn_len(c, a), k;
    len += rl78__insn_len(c, (a + (uint32_t)len) & 0xFFFFFu);
    for (k = 0; k < len && a + (uint32_t)k < RL78_MEM_SIZE; k++) {
        uint32_t b = a + (uint32_t)k;
        c->xl_bytes[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }
}

int rl78_xl_covers(const rl78_cpu *c, uint32_t addr)
{
    addr &= 0xFFFFFu;
    return c->xl_bytes ? (c->xl_bytes[addr >> 3] >> (addr & 7u)) & 1 : 0;
}

/* The pages translated code may access without the page tables: the run of plain pages (read and
   written straight in the memory image) that ends below the fixed SFRs.  That is the RAM, with the
   saddr area and the register banks; the mirror, the code flash and pages with hooks are not plain.
   Only addresses from 0xF0000 up qualify, so the range can never reach the code flash. */
void rl78_xl_plain_range(const rl78_cpu *c, uint32_t *lo, uint32_t *hi)
{
    uint32_t pg = 0xFFFu;
    while (pg > 0xF00u && rl78__pages_plain(c, (pg - 1u) << 8, pg << 8)) pg--;
    *lo = pg << 8;
    *hi = 0xFFF00u;
}

int rl78_xl_attach(rl78_cpu *c, const rl78_xl_module *m, char *err, size_t errlen)
{
    uint32_t i;
    const int s1 = c->cfg.core == RL78_CORE_S1;
    const int s3 = c->cfg.core == RL78_CORE_S3 || c->cfg.core == RL78_CORE_AUTO;
    if (!m) {
        free(c->xl_index);
        c->xl_index = NULL;
        free(c->xl_inside);
        c->xl_inside = NULL;
        free(c->xl_bytes);
        c->xl_bytes = NULL;
        c->xl_mod = NULL;
        c->xl_bp_inside = 0;
        c->xl_map_bad = 0;
        c->xl_plain_lo = c->xl_plain_hi = 0;
        return 0;
    }
#define FAIL(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)
    if (m->abi != RL78_XL_ABI) FAIL("translated code: ABI %u, simulator %u", (unsigned)m->abi, (unsigned)RL78_XL_ABI);
    if (m->cpu_size != (uint32_t)sizeof(rl78_cpu))
        FAIL("translated code: CPU state is %u bytes, simulator %u", (unsigned)m->cpu_size, (unsigned)sizeof(rl78_cpu));
    if ((int)m->core_s1 != s1 || (int)m->core_s3 != s3) FAIL("translated code: made for another CPU core setting");
    if (c->cfg.rom_size == 0) FAIL("translated code needs a write-protected code flash (rom_size)");
    if (!rl78__pages_plain(c, m->plain_lo, m->plain_hi)) FAIL("translated code: made for another memory map");
    if (!c->xl_index) {
        c->xl_index = (uint32_t *)calloc(RL78_MEM_SIZE, sizeof *c->xl_index);
        if (!c->xl_index) FAIL("out of memory");
    } else {
        memset(c->xl_index, 0, RL78_MEM_SIZE * sizeof *c->xl_index);
    }
    if (!c->xl_inside) {
        c->xl_inside = (uint8_t *)calloc(RL78_MEM_SIZE / 8u, 1);
        if (!c->xl_inside) FAIL("out of memory");
    } else {
        memset(c->xl_inside, 0, RL78_MEM_SIZE / 8u);
    }
    if (!c->xl_bytes) {
        c->xl_bytes = (uint8_t *)calloc(RL78_MEM_SIZE / 8u + 2u, 1);
        if (!c->xl_bytes) FAIL("out of memory");
    } else {
        memset(c->xl_bytes, 0, RL78_MEM_SIZE / 8u + 2u);
    }
#undef FAIL
    for (i = 0; i < m->ninside; i++) {
        uint32_t a = m->inside[i] & 0xFFFFFu;
        c->xl_inside[a >> 3] |= (uint8_t)(1u << (a & 7u));
        mark_insn_bytes(c, a);
    }
    for (i = 0; i < m->nleaders; i++) mark_insn_bytes(c, m->leaders[i].pc & 0xFFFFFu);
    for (i = 0; i < m->nleaders; i++) c->xl_index[m->leaders[i].pc & 0xFFFFFu] = i + 1u;
    c->xl_mod = m;
    c->xl_plain_lo = m->plain_lo;
    c->xl_plain_hi = m->plain_hi;
    c->xl_map_bad = 0;
    c->xl_miss = 0;
    recount_bp_inside(c);
    return 0;
}

uint64_t rl78_xl_insn_count(const rl78_cpu *c) { return c->xl_insns; }

void rl78_xl_set_enabled(rl78_cpu *c, int on) { c->xl_off = !on; }

/* Run translated code from c->pc for at most lim instructions.  Returns the
   number executed.  *why is a stop reason, or -1 when the caller should look
   again (0 executed: no translated code for this pc right now). */
uint64_t rl78__xl_run(rl78_cpu *c, uint64_t lim, int *why)
{
    uint64_t n = 0;
    const rl78_xl_leader *ld = c->xl_mod->leaders;
    const uint32_t *idx = c->xl_index;
    *why = -1;
    c->exit_req = 0;
    c->hold_req = 0;
    c->hold_n = 0;
    c->xl_depth = 0;
    for (;;) {
        uint32_t k = idx[c->pc & 0xFFFFFu];
        int st;
        if (k == 0) break;
        st = ld[k - 1u].fn(c, &n, lim);
        if (st == RL78_XL_CONT) continue;
        if (st >= RL78_XL_STOP) *why = st - RL78_XL_STOP - 1;
        break;
    }
    c->hold_irq = (n > 0 && c->hold_n == n);
    c->xl_insns += n;
    return n;
}
