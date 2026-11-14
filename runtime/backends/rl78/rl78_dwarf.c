/*
 * rl78_dwarf.c - DWARF 2〜4（5 は見出しだけ）の .debug_info を読み、sim_backend.h の型・変数・関数にする
 *
 * 外部ツールは使わない。分からない属性・form・タグに出会っても落ちずに読み飛ばし、警告を残す。
 * form の大きさが分からない場合だけは、その CU の残りを読めないので CU ごと飛ばす。
 *
 * 型の扱い:
 *   - typedef は「名前だけ違う写し」を作る（表示名を残すため）。const/volatile/restrict は剥がす
 *   - ポインタの大きさは DW_AT_byte_size、なければ DW_AT_address_class（CC-RL は near データポインタに 4 を付ける）
 *   - ビットフィールドは「構造体先頭からのビット位置」（リトルエンディアン、LSB = 0）に換算する
 *       DWARF4 の DW_AT_data_bit_offset はそのまま
 *       DWARF2/3 の DW_AT_bit_offset は記憶単位（DW_AT_byte_size）の MSB から数えるので
 *         位置 = 単位の先頭バイト * 8 + 単位のビット数 - bit_offset - bit_size
 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "sim_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* タグ */
enum {
    TAG_array = 0x01, TAG_class = 0x02, TAG_enum = 0x04, TAG_formal_parameter = 0x05, TAG_label = 0x0a,
    TAG_lexical_block = 0x0b, TAG_member = 0x0d, TAG_pointer = 0x0f, TAG_reference = 0x10,
    TAG_compile_unit = 0x11, TAG_structure = 0x13, TAG_subroutine_type = 0x15, TAG_typedef = 0x16,
    TAG_union = 0x17, TAG_unspecified_parameters = 0x18, TAG_inlined_subroutine = 0x1d, TAG_subrange = 0x21,
    TAG_base = 0x24, TAG_const = 0x26, TAG_enumerator = 0x28, TAG_subprogram = 0x2e, TAG_variable = 0x34,
    TAG_volatile = 0x35, TAG_restrict = 0x37, TAG_unspecified_type = 0x3b, TAG_partial_unit = 0x3c,
    TAG_atomic = 0x47
};
/* 属性 */
enum {
    AT_location = 0x02, AT_name = 0x03, AT_byte_size = 0x0b, AT_bit_offset = 0x0c, AT_bit_size = 0x0d,
    AT_low_pc = 0x11, AT_high_pc = 0x12, AT_const_value = 0x1c, AT_lower_bound = 0x22, AT_producer = 0x25,
    AT_upper_bound = 0x2f, AT_abstract_origin = 0x31, AT_address_class = 0x33, AT_count = 0x37,
    AT_data_member_location = 0x38, AT_declaration = 0x3c, AT_encoding = 0x3e, AT_external = 0x3f,
    AT_specification = 0x47, AT_type = 0x49, AT_data_bit_offset = 0x6b
};
/* 属性の値の種類 */
enum { V_NONE, V_UNS, V_SIGNED, V_STR, V_BLOCK, V_REF, V_ADDR, V_FLAG, V_SECOFF };

typedef struct {
    uint16_t at, form;
    uint8_t vk;
    uint64_t u;
    const uint8_t *p;
    uint32_t len;
} dattr;

typedef struct {
    uint32_t off;
    uint16_t tag;
    int parent;
    int cu;
    uint32_t a0;
    uint16_t na;
} ddie;

typedef struct {
    uint32_t off, end;
    uint8_t ver, addr_size, off_size;
    const char *name;
} dcu;

typedef struct {
    uint64_t code;
    uint16_t tag;
    uint8_t children;
    int s0, ns;
} dabbr;
typedef struct { uint16_t at, form; int64_t ic; } dspec;

typedef struct {
    rl78_symtab *t;
    const rl78_elf *e;
    const rl78_dwarf_opts *o;
    rl78_warns *w;
    const uint8_t *info, *abbr, *str;
    size_t info_len, abbr_len, str_len;
    ddie *dies;
    int ndies, capdies;
    dattr *at;
    size_t nat, capat;
    dcu *cus;
    int ncus, capcus;
    sim_type **memo;
    uint8_t *state;          /* 0 未着手 1 作成中 2 済み */
    int *fchild, *nsib;      /* 最初の子、次の兄弟（なければ -1） */
    uint16_t seen_at[64];
    int nseen_at;
    uint16_t seen_tag[64];
    int nseen_tag;
    int warned_addr_class[16];
    /* 重複除けのハッシュ */
    uint32_t *vh, *fh;
    size_t vhcap, fhcap;
} dctx;

/* ================================================================ 読み出しの部品 */

typedef struct { const uint8_t *p, *e; int bad; } cur_t;

static uint64_t rd_n(cur_t *c, unsigned n)
{
    uint64_t v = 0;
    unsigned i;
    if ((size_t)(c->e - c->p) < n) { c->bad = 1; c->p = c->e; return 0; }
    for (i = 0; i < n; i++) v |= (uint64_t)c->p[i] << (8 * i);
    c->p += n;
    return v;
}

static uint64_t rd_uleb(cur_t *c)
{
    uint64_t v = 0;
    unsigned sh = 0;
    for (;;) {
        uint8_t b;
        if (c->p >= c->e) { c->bad = 1; return v; }
        b = *c->p++;
        if (sh < 64) v |= (uint64_t)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80)) return v;
    }
}

static int64_t rd_sleb(cur_t *c)
{
    int64_t v = 0;
    unsigned sh = 0;
    uint8_t b = 0;
    for (;;) {
        if (c->p >= c->e) { c->bad = 1; return v; }
        b = *c->p++;
        if (sh < 64) v |= (int64_t)((uint64_t)(b & 0x7f) << sh);
        sh += 7;
        if (!(b & 0x80)) break;
    }
    if (sh < 64 && (b & 0x40)) v |= (int64_t)(~0ULL << sh);
    return v;
}

/* ================================================================ 略語表 */

typedef struct {
    uint32_t off;
    dabbr *a;
    int na;
    dspec *s;
    int ns;
} abbr_tab;

static int load_abbrev(dctx *x, uint32_t off, abbr_tab *t)
{
    cur_t c;
    int cap = 0, scap = 0;
    memset(t, 0, sizeof *t);
    t->off = off;
    if (off >= x->abbr_len) return -1;
    c.p = x->abbr + off;
    c.e = x->abbr + x->abbr_len;
    c.bad = 0;
    for (;;) {
        dabbr a;
        uint64_t code = rd_uleb(&c);
        if (c.bad) return -1;
        if (code == 0) break;
        a.code = code;
        a.tag = (uint16_t)rd_uleb(&c);
        a.children = c.p < c.e ? *c.p++ : 0;
        a.s0 = t->ns;
        a.ns = 0;
        for (;;) {
            uint64_t at = rd_uleb(&c), form = rd_uleb(&c);
            int64_t ic = 0;
            if (c.bad) return -1;
            if (at == 0 && form == 0) break;
            if (form == 0x21) ic = rd_sleb(&c); /* DW_FORM_implicit_const */
            if (t->ns == scap) { scap = scap ? scap * 2 : 64; t->s = sim_xrealloc(t->s, (size_t)scap * sizeof *t->s); }
            t->s[t->ns].at = (uint16_t)at;
            t->s[t->ns].form = (uint16_t)form;
            t->s[t->ns].ic = ic;
            t->ns++;
            a.ns++;
        }
        if (t->na == cap) { cap = cap ? cap * 2 : 64; t->a = sim_xrealloc(t->a, (size_t)cap * sizeof *t->a); }
        t->a[t->na++] = a;
    }
    return 0;
}

static const dabbr *find_abbr(const abbr_tab *t, uint64_t code)
{
    int i;
    if (code >= 1 && code <= (uint64_t)t->na && t->a[code - 1].code == code) return &t->a[code - 1];
    for (i = 0; i < t->na; i++) if (t->a[i].code == code) return &t->a[i];
    return NULL;
}

/* ================================================================ 属性の値 */

static const char *form_name(unsigned f)
{
    static char buf[32];
    snprintf(buf, sizeof buf, "0x%x", f);
    return buf;
}

/* form を 1 つ読む。大きさが分からない form なら -1 */
static int read_form(dctx *x, const dcu *u, cur_t *c, unsigned form, int64_t ic, dattr *a, int depth)
{
    a->form = (uint16_t)form;
    a->vk = V_NONE;
    a->u = 0;
    a->p = NULL;
    a->len = 0;
    switch (form) {
    case 0x01: a->vk = V_ADDR; a->u = rd_n(c, u->addr_size); break;              /* addr */
    case 0x03: a->len = (uint32_t)rd_n(c, 2); goto block;                           /* block2 */
    case 0x04: a->len = (uint32_t)rd_n(c, 4); goto block;                           /* block4 */
    case 0x05: a->vk = V_UNS; a->u = rd_n(c, 2); break;                             /* data2 */
    case 0x06: a->vk = V_UNS; a->u = rd_n(c, 4); break;                             /* data4 */
    case 0x07: a->vk = V_UNS; a->u = rd_n(c, 8); break;                             /* data8 */
    case 0x08: {                                                                    /* string */
        const uint8_t *s = c->p;
        while (c->p < c->e && *c->p) c->p++;
        if (c->p >= c->e) { c->bad = 1; return 0; }
        a->vk = V_STR; a->p = s; a->len = (uint32_t)(c->p - s);
        c->p++;
        break;
    }
    case 0x09: a->len = (uint32_t)rd_uleb(c); goto block;                           /* block */
    case 0x0a: a->len = (uint32_t)rd_n(c, 1); goto block;                           /* block1 */
    case 0x0b: a->vk = V_UNS; a->u = rd_n(c, 1); break;                             /* data1 */
    case 0x0c: a->vk = V_FLAG; a->u = rd_n(c, 1); break;                            /* flag */
    case 0x0d: a->vk = V_SIGNED; a->u = (uint64_t)rd_sleb(c); break;                /* sdata */
    case 0x0e: case 0x1f: case 0x1f21: {                                            /* strp, line_strp, GNU_strp_alt */
        uint64_t off = rd_n(c, u->off_size);
        a->vk = V_STR;
        if (form == 0x0e && x->str && off < x->str_len) {
            const uint8_t *s = x->str + off, *q = s;
            while (q < x->str + x->str_len && *q) q++;
            a->p = s;
            a->len = (uint32_t)(q - s);
        } else {
            a->vk = V_NONE;
        }
        break;
    }
    case 0x0f: a->vk = V_UNS; a->u = rd_uleb(c); break;                             /* udata */
    case 0x10: a->vk = V_REF; a->u = rd_n(c, u->ver <= 2 ? u->addr_size : u->off_size); break; /* ref_addr */
    case 0x11: a->vk = V_REF; a->u = u->off + rd_n(c, 1); break;                    /* ref1 */
    case 0x12: a->vk = V_REF; a->u = u->off + rd_n(c, 2); break;                    /* ref2 */
    case 0x13: a->vk = V_REF; a->u = u->off + rd_n(c, 4); break;                    /* ref4 */
    case 0x14: a->vk = V_REF; a->u = u->off + rd_n(c, 8); break;                    /* ref8 */
    case 0x15: a->vk = V_REF; a->u = u->off + rd_uleb(c); break;                    /* ref_udata */
    case 0x16: {                                                                    /* indirect */
        unsigned f2 = (unsigned)rd_uleb(c);
        if (depth > 4) return -1;
        return read_form(x, u, c, f2, 0, a, depth + 1);
    }
    case 0x17: a->vk = V_SECOFF; a->u = rd_n(c, u->off_size); break;               /* sec_offset */
    case 0x18: a->len = (uint32_t)rd_uleb(c); goto block;                           /* exprloc */
    case 0x19: a->vk = V_FLAG; a->u = 1; break;                                     /* flag_present */
    case 0x1a: case 0x1b: case 0x22: case 0x23: case 0x1f01: case 0x1f02:           /* strx addrx loclistx rnglistx GNU_*_index */
        (void)rd_uleb(c); break;
    case 0x1c: (void)rd_n(c, 4); break;                                             /* ref_sup4 */
    case 0x1d: case 0x1f20: (void)rd_n(c, u->off_size); break;                      /* strp_sup, GNU_ref_alt */
    case 0x1e: (void)rd_n(c, 8); (void)rd_n(c, 8); break;                           /* data16 */
    case 0x20: case 0x24: (void)rd_n(c, 8); break;                                  /* ref_sig8, ref_sup8 */
    case 0x21: a->vk = V_SIGNED; a->u = (uint64_t)ic; break;                        /* implicit_const */
    case 0x25: case 0x29: (void)rd_n(c, 1); break;                                  /* strx1 addrx1 */
    case 0x26: case 0x2a: (void)rd_n(c, 2); break;
    case 0x27: case 0x2b: (void)rd_n(c, 3); break;
    case 0x28: case 0x2c: (void)rd_n(c, 4); break;
    default: return -1;
    }
    return 0;
block:
    if ((size_t)(c->e - c->p) < a->len) { c->bad = 1; c->p = c->e; return 0; }
    a->vk = V_BLOCK;
    a->p = c->p;
    c->p += a->len;
    return 0;
}

static void note_unknown_attr(dctx *x, unsigned at)
{
    int i;
    /* DWARF 2〜5 の標準属性と、よくある GNU 拡張（0x2101〜0x2137）は黙って持つだけにする */
    if (at <= 0x8c || (at >= 0x2101 && at <= 0x2137)) return;
    for (i = 0; i < x->nseen_at; i++) if (x->seen_at[i] == at) return;
    if (x->nseen_at < 64) x->seen_at[x->nseen_at++] = (uint16_t)at;
    rl78_warn(x->w, "DWARF: 知らない属性 0x%x（処理系独自の拡張）を読み飛ばしました", at);
}

static void note_unknown_tag(dctx *x, unsigned tag, uint32_t off)
{
    int i;
    for (i = 0; i < x->nseen_tag; i++) if (x->seen_tag[i] == tag) return;
    if (x->nseen_tag < 64) x->seen_tag[x->nseen_tag++] = (uint16_t)tag;
    rl78_warn(x->w, "DWARF: 型として知らないタグ 0x%x（.debug_info+0x%x）。型は不明として扱います", tag, (unsigned)off);
}

/* ================================================================ DIE の読み込み */

static int load_cus(dctx *x)
{
    abbr_tab *tabs = NULL;
    int ntabs = 0, i;
    uint32_t off = 0;
    int nok = 0;
    while (off + 11 <= x->info_len) {
        cur_t c;
        uint64_t len;
        unsigned off_size = 4, ver, addr_size, unit_type = 1;
        uint64_t abbr_off;
        uint32_t end;
        dcu u;
        const abbr_tab *tab = NULL;
        int stack[256], sp = 0, ok = 1, first_die;
        c.p = x->info + off;
        c.e = x->info + x->info_len;
        c.bad = 0;
        len = rd_n(&c, 4);
        if (len == 0xffffffffu) { len = rd_n(&c, 8); off_size = 8; }
        if (len == 0 || len > (uint64_t)(c.e - c.p)) {
            rl78_warn(x->w, "DWARF: .debug_info+0x%x の CU の長さ（0x%llx）が不正です。以降を読みません", off, (unsigned long long)len);
            break;
        }
        end = (uint32_t)((size_t)(c.p - x->info) + len);
        ver = (unsigned)rd_n(&c, 2);
        if (ver >= 5) {
            unit_type = (unsigned)rd_n(&c, 1);
            addr_size = (unsigned)rd_n(&c, 1);
            abbr_off = rd_n(&c, off_size);
            if (unit_type == 2 || unit_type == 6) { (void)rd_n(&c, 8); (void)rd_n(&c, off_size); } /* type unit */
            else if (unit_type == 4 || unit_type == 5) (void)rd_n(&c, 8);                       /* skeleton / split */
        } else {
            abbr_off = rd_n(&c, off_size);
            addr_size = (unsigned)rd_n(&c, 1);
        }
        if (ver < 2 || ver > 5 || (addr_size != 2 && addr_size != 4 && addr_size != 8)) {
            rl78_warn(x->w, "DWARF: .debug_info+0x%x の CU（バージョン %u、アドレス %u バイト）は読めないので飛ばします", off, ver, addr_size);
            x->t->nskipped_cu++;
            off = end;
            continue;
        }
        if (ver == 5) rl78_warn(x->w, "DWARF: バージョン 5 の CU（.debug_info+0x%x）は一部の form（strx など）の名前を読めません", off);
        x->t->cu_versions[ver]++;
        /* 略語表 */
        for (i = 0; i < ntabs; i++) if (tabs[i].off == abbr_off) { tab = &tabs[i]; break; }
        if (!tab) {
            tabs = sim_xrealloc(tabs, (size_t)(ntabs + 1) * sizeof *tabs);
            if (load_abbrev(x, (uint32_t)abbr_off, &tabs[ntabs])) {
                rl78_warn(x->w, "DWARF: .debug_abbrev+0x%llx を読めません。CU（.debug_info+0x%x）を飛ばします", (unsigned long long)abbr_off, off);
                free(tabs[ntabs].a);
                free(tabs[ntabs].s);
                x->t->nskipped_cu++;
                off = end;
                continue;
            }
            tab = &tabs[ntabs++];
        }
        u.off = off;
        u.end = end;
        u.ver = (uint8_t)ver;
        u.addr_size = (uint8_t)addr_size;
        u.off_size = (uint8_t)off_size;
        u.name = NULL;
        if (x->ncus == x->capcus) { x->capcus = x->capcus ? x->capcus * 2 : 64; x->cus = sim_xrealloc(x->cus, (size_t)x->capcus * sizeof *x->cus); }
        x->cus[x->ncus] = u;
        first_die = x->ndies;
        c.e = x->info + end;
        while (c.p < c.e) {
            uint32_t doff = (uint32_t)(c.p - x->info);
            uint64_t code = rd_uleb(&c);
            const dabbr *a;
            ddie d;
            int k;
            if (c.bad) { ok = 0; break; }
            if (code == 0) { if (sp > 0) sp--; continue; }
            a = find_abbr(tab, code);
            if (!a) {
                rl78_warn(x->w, "DWARF: .debug_info+0x%x の略語番号 %llu が略語表にありません。CU の残りを飛ばします", doff, (unsigned long long)code);
                ok = 0;
                break;
            }
            d.off = doff;
            d.tag = a->tag;
            d.parent = sp > 0 ? stack[sp - 1] : -1;
            d.cu = x->ncus;
            d.a0 = (uint32_t)x->nat;
            d.na = (uint16_t)a->ns;
            if (x->nat + (size_t)a->ns > x->capat) {
                x->capat = (x->capat + (size_t)a->ns) * 2 + 256;
                x->at = sim_xrealloc(x->at, x->capat * sizeof *x->at);
            }
            for (k = 0; k < a->ns; k++) {
                dattr *at = &x->at[x->nat];
                const dspec *s = &tab->s[a->s0 + k];
                if (read_form(x, &u, &c, s->form, s->ic, at, 0)) {
                    rl78_warn(x->w, "DWARF: .debug_info+0x%x に大きさの分からない form %s（属性 0x%x）があります。CU の残りを飛ばします",
                              doff, form_name(s->form), s->at);
                    ok = 0;
                    break;
                }
                at->at = s->at;
                note_unknown_attr(x, s->at);
                x->nat++;
            }
            if (!ok || c.bad) { ok = 0; break; }
            d.na = (uint16_t)(x->nat - d.a0);
            if (x->ndies == x->capdies) { x->capdies = x->capdies ? x->capdies * 2 : 1024; x->dies = sim_xrealloc(x->dies, (size_t)x->capdies * sizeof *x->dies); }
            x->dies[x->ndies++] = d;
            if (a->children) {
                if (sp < (int)(sizeof stack / sizeof stack[0])) stack[sp++] = x->ndies - 1;
                else { ok = 0; break; }
            }
        }
        if (!ok) x->t->nskipped_cu++;
        if (x->ndies > first_die) {
            /* CU の名前 */
            const ddie *cd = &x->dies[first_die];
            for (i = 0; i < cd->na; i++) {
                const dattr *at = &x->at[cd->a0 + i];
                if (at->at == AT_name && at->vk == V_STR) x->cus[x->ncus].name = ar_strndup(&x->t->ar, (const char *)at->p, at->len);
                if (at->at == AT_producer && at->vk == V_STR && !x->t->producer)
                    x->t->producer = ar_strndup(&x->t->ar, (const char *)at->p, at->len);
            }
        }
        x->ncus++;
        nok++;
        off = end;
    }
    for (i = 0; i < ntabs; i++) { free(tabs[i].a); free(tabs[i].s); }
    free(tabs);
    return nok;
}

static const dattr *attr(const dctx *x, int di, unsigned at)
{
    const ddie *d = &x->dies[di];
    unsigned i;
    for (i = 0; i < d->na; i++) if (x->at[d->a0 + i].at == at) return &x->at[d->a0 + i];
    return NULL;
}

static int attr_u(const dctx *x, int di, unsigned at, uint64_t *v)
{
    const dattr *a = attr(x, di, at);
    if (!a || (a->vk != V_UNS && a->vk != V_SIGNED && a->vk != V_FLAG && a->vk != V_ADDR)) return 0;
    *v = a->u;
    return 1;
}

static const char *attr_str(dctx *x, int di, unsigned at)
{
    const dattr *a = attr(x, di, at);
    if (!a || a->vk != V_STR) return NULL;
    return ar_strndup(&x->t->ar, (const char *)a->p, a->len);
}

static int find_die(const dctx *x, uint64_t off)
{
    int lo = 0, hi = x->ndies - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (x->dies[mid].off == off) return mid;
        if (x->dies[mid].off < off) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int attr_ref(const dctx *x, int di, unsigned at)
{
    const dattr *a = attr(x, di, at);
    if (!a || a->vk != V_REF) return -1;
    return find_die(x, a->u);
}

/* 子の DIE を順に。prev=-1 で先頭から。なければ -1 */
static int next_child(const dctx *x, int parent, int prev)
{
    return prev < 0 ? x->fchild[parent] : x->nsib[prev];
}

static void link_tree(dctx *x)
{
    int i;
    int *last = sim_xmalloc(((size_t)x->ndies + 1) * sizeof *last);
    x->fchild = sim_xmalloc(((size_t)x->ndies + 1) * sizeof *x->fchild);
    x->nsib = sim_xmalloc(((size_t)x->ndies + 1) * sizeof *x->nsib);
    for (i = 0; i < x->ndies; i++) { x->fchild[i] = -1; x->nsib[i] = -1; last[i] = -1; }
    for (i = 0; i < x->ndies; i++) {
        int p = x->dies[i].parent;
        if (p < 0) continue;
        if (last[p] < 0) x->fchild[p] = i;
        else x->nsib[last[p]] = i;
        last[p] = i;
    }
    free(last);
}

/* ================================================================ 型 */

static sim_type *new_type(dctx *x, sim_kind k, const char *name, uint32_t size)
{
    sim_type *t = ar_alloc(&x->t->ar, sizeof *t);
    t->kind = k;
    t->name = name;
    t->size = size;
    return t;
}

static const sim_type *ty(dctx *x, int di, int depth);

static const sim_type *ty_ref(dctx *x, int di, int depth)
{
    int r = attr_ref(x, di, AT_type);
    if (r < 0) return NULL; /* void */
    return ty(x, r, depth + 1);
}

/* 型の DIE が const 修飾の連なりなら 1（typedef は通り抜けない） */
static int is_const_chain(dctx *x, int di)
{
    int guard = 0;
    while (di >= 0 && guard++ < 32) {
        unsigned tag = x->dies[di].tag;
        if (tag == TAG_const) return 1;
        if (tag == TAG_volatile || tag == TAG_restrict || tag == TAG_atomic) { di = attr_ref(x, di, AT_type); continue; }
        if (tag == TAG_array) { di = attr_ref(x, di, AT_type); continue; } /* const 要素の配列も const */
        if (tag == TAG_typedef) { di = attr_ref(x, di, AT_type); continue; }
        return 0;
    }
    return 0;
}

static uint32_t eval_member_loc(const dattr *a)
{
    cur_t c;
    if (!a) return 0;
    if (a->vk == V_UNS || a->vk == V_SIGNED) return (uint32_t)a->u;
    if (a->vk != V_BLOCK || !a->len) return 0;
    c.p = a->p;
    c.e = a->p + a->len;
    c.bad = 0;
    switch (*c.p++) {
    case 0x23: return (uint32_t)rd_uleb(&c);              /* DW_OP_plus_uconst */
    case 0x10: return (uint32_t)rd_uleb(&c);              /* DW_OP_constu */
    case 0x11: return (uint32_t)rd_sleb(&c);              /* DW_OP_consts */
    case 0x08: return c.p < c.e ? *c.p : 0;               /* DW_OP_const1u */
    case 0x0a: return (uint32_t)rd_n(&c, 2);              /* DW_OP_const2u */
    case 0x0c: return (uint32_t)rd_n(&c, 4);              /* DW_OP_const4u */
    default: return 0;
    }
}

static const char *tagged_name(dctx *x, const char *kw, const char *tag)
{
    sim_buf b;
    const char *r;
    if (!tag) return NULL;
    sim_buf_init(&b);
    sim_buf_printf(&b, "%s %s", kw, tag);
    r = ar_strdup(&x->t->ar, sim_buf_cstr(&b));
    sim_buf_free(&b);
    return r;
}

static uint32_t ptr_size(dctx *x, int di)
{
    uint64_t v;
    if (attr_u(x, di, AT_byte_size, &v) && v > 0 && v <= 8) return (uint32_t)v;
    if (attr_u(x, di, AT_address_class, &v)) {
        if (v < 16 && x->o->addr_class_size[v]) return (uint32_t)x->o->addr_class_size[v];
        if (v == 4) return 2; /* CC-RL: near データポインタ（実物の .abs で確認） */
        if (v < 16 && !x->warned_addr_class[v]) {
            x->warned_addr_class[v] = 1;
            rl78_warn(x->w, "DWARF: ポインタの DW_AT_address_class %llu の大きさが分かりません。far（4 バイト）として扱います"
                            "（sim.cfg の addr_class.%llu = 2 で near にできます）", (unsigned long long)v, (unsigned long long)v);
        }
        return 4;
    }
    return 2;
}

static const sim_type *ty(dctx *x, int di, int depth)
{
    unsigned tag;
    sim_type *t;
    uint64_t v;
    if (di < 0) return NULL;
    if (x->state[di]) return x->memo[di]; /* 済み、または作成中（循環: 作りかけのものを返す） */
    if (depth > 200) return NULL;
    tag = x->dies[di].tag;
    switch (tag) {
    case TAG_const: case TAG_volatile: case TAG_restrict: case TAG_atomic: {
        const sim_type *u;
        x->state[di] = 1;
        u = ty_ref(x, di, depth);
        x->memo[di] = (sim_type *)u;
        x->state[di] = 2;
        return u;
    }
    case TAG_base: {
        uint64_t enc = 0;
        sim_kind k = SIM_T_UINT;
        attr_u(x, di, AT_encoding, &enc);
        switch (enc) {
        case 0x02: k = SIM_T_BOOL; break;
        case 0x04: k = SIM_T_FLOAT; break;
        case 0x05: case 0x06: case 0x0d: k = SIM_T_INT; break;  /* signed, signed_char, signed_fixed */
        case 0x03: k = SIM_T_UNKNOWN; break;                      /* complex_float */
        default: k = SIM_T_UINT; break;
        }
        v = 0;
        attr_u(x, di, AT_byte_size, &v);
        t = new_type(x, k, attr_str(x, di, AT_name), (uint32_t)v);
        if (k == SIM_T_FLOAT && v != 4 && v != 8) t->kind = SIM_T_UNKNOWN;
        break;
    }
    case TAG_unspecified_type:
        t = new_type(x, SIM_T_VOID, attr_str(x, di, AT_name), 0);
        break;
    case TAG_pointer: case TAG_reference:
        t = new_type(x, SIM_T_PTR, NULL, ptr_size(x, di));
        x->memo[di] = t;
        x->state[di] = 1;
        t->target = ty_ref(x, di, depth);
        x->state[di] = 2;
        return t;
    case TAG_subroutine_type:
        t = new_type(x, SIM_T_FUNC, NULL, 0);
        break;
    case TAG_typedef: {
        const sim_type *u;
        t = new_type(x, SIM_T_UNKNOWN, attr_str(x, di, AT_name), 0);
        x->memo[di] = t;
        x->state[di] = 1;
        u = ty_ref(x, di, depth);
        if (u) {
            const char *nm = t->name;
            *t = *u;
            t->name = nm;
        } else {
            t->kind = SIM_T_VOID;
        }
        x->state[di] = 2;
        return t;
    }
    case TAG_structure: case TAG_union: case TAG_class: {
        int c, n = 0, k = 0;
        sim_member *m;
        int is_union = tag == TAG_union;
        v = 0;
        attr_u(x, di, AT_byte_size, &v);
        t = new_type(x, is_union ? SIM_T_UNION : SIM_T_STRUCT,
                     tagged_name(x, is_union ? "union" : "struct", attr_str(x, di, AT_name)), (uint32_t)v);
        x->memo[di] = t;
        x->state[di] = 1;
        for (c = next_child(x, di, -1); c >= 0; c = next_child(x, di, c)) if (x->dies[c].tag == TAG_member) n++;
        m = n ? ar_alloc(&x->t->ar, (size_t)n * sizeof *m) : NULL;
        t->members = m;
        t->nmembers = n;
        for (c = next_child(x, di, -1); c >= 0 && k < n; c = next_child(x, di, c)) {
            uint64_t bs = 0, bo = 0, us = 0, dbo = 0;
            uint32_t loc;
            if (x->dies[c].tag != TAG_member) continue;
            m[k].name = attr_str(x, c, AT_name);
            if (!m[k].name) m[k].name = "";
            m[k].type = ty_ref(x, c, depth);
            loc = eval_member_loc(attr(x, c, AT_data_member_location));
            m[k].offset = loc;
            m[k].bit_offset = -1;
            m[k].bit_size = 0;
            if (attr_u(x, c, AT_bit_size, &bs) && bs > 0) {
                int32_t pos;
                if (attr_u(x, c, AT_data_bit_offset, &dbo)) {
                    pos = (int32_t)dbo;
                } else {
                    attr_u(x, c, AT_bit_offset, &bo);
                    if (!attr_u(x, c, AT_byte_size, &us) || us == 0) us = m[k].type ? m[k].type->size : 1;
                    if (x->o->bitfield_lsb) pos = (int32_t)(loc * 8u + (uint32_t)bo);
                    else pos = (int32_t)(loc * 8u + (uint32_t)us * 8u - (uint32_t)bo - (uint32_t)bs);
                }
                if (pos < 0) pos = 0;
                m[k].bit_offset = pos;
                m[k].bit_size = (uint32_t)bs;
                m[k].offset = (uint32_t)pos / 8u;
            }
            k++;
        }
        x->state[di] = 2;
        return t;
    }
    case TAG_array: {
        int c, nd = 0, i;
        uint32_t counts[16];
        const sim_type *et;
        sim_type *outer = new_type(x, SIM_T_ARRAY, NULL, 0);
        x->memo[di] = outer;
        x->state[di] = 1;
        et = ty_ref(x, di, depth);
        for (c = next_child(x, di, -1); c >= 0; c = next_child(x, di, c)) {
            uint64_t cnt = 0, ub, lb = 0;
            if (x->dies[c].tag != TAG_subrange && x->dies[c].tag != TAG_enum) continue;
            if (attr_u(x, c, AT_count, &cnt)) {
            } else if (attr_u(x, c, AT_upper_bound, &ub)) {
                const dattr *ua = attr(x, c, AT_upper_bound);
                attr_u(x, c, AT_lower_bound, &lb);
                if ((ua->vk == V_SIGNED && (int64_t)ub < 0) || ub == 0xffffffffu || ub == 0xffffu) cnt = 0;
                else cnt = ub - lb + 1;
            }
            if (nd < 16) counts[nd++] = (uint32_t)cnt;
        }
        if (nd == 0) counts[nd++] = 0;
        /* 内側から組み立てる。一番外は outer */
        for (i = nd - 1; i >= 0; i--) {
            sim_type *a = i == 0 ? outer : new_type(x, SIM_T_ARRAY, NULL, 0);
            a->kind = SIM_T_ARRAY;
            a->target = et;
            a->count = counts[i];
            a->size = et ? et->size * counts[i] : 0;
            et = a;
        }
        if (attr_u(x, di, AT_byte_size, &v) && v) outer->size = (uint32_t)v;
        x->state[di] = 2;
        return outer;
    }
    case TAG_enum: {
        const sim_type *u = NULL;
        int c, neg = 0;
        v = 0;
        attr_u(x, di, AT_byte_size, &v);
        if (attr_ref(x, di, AT_type) >= 0) u = ty_ref(x, di, depth);
        for (c = next_child(x, di, -1); c >= 0; c = next_child(x, di, c)) {
            const dattr *cv = attr(x, c, AT_const_value);
            if (cv && cv->vk == V_SIGNED && (int64_t)cv->u < 0) neg = 1;
        }
        t = new_type(x, u ? (u->kind == SIM_T_INT ? SIM_T_INT : SIM_T_UINT) : (neg ? SIM_T_INT : SIM_T_UINT),
                     tagged_name(x, "enum", attr_str(x, di, AT_name)), (uint32_t)(v ? v : (u ? u->size : 2)));
        if (!t->name) t->name = "enum";
        break;
    }
    default:
        note_unknown_tag(x, tag, x->dies[di].off);
        t = new_type(x, SIM_T_UNKNOWN, attr_str(x, di, AT_name), 0);
        if (attr_u(x, di, AT_byte_size, &v)) t->size = (uint32_t)v;
        break;
    }
    x->memo[di] = t;
    x->state[di] = 2;
    return t;
}

/* ================================================================ 変数と関数 */

static uint32_t hstr(const char *s)
{
    uint32_t h = 2166136261u;
    while (s && *s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* (名前, 関数, アドレス) が同じ変数がもうあれば 1。なければ登録して 0 */
static int dup_var(dctx *x, const sim_var *nv, size_t idx)
{
    uint32_t h = hstr(nv->name) ^ (hstr(nv->func) * 31u) ^ (uint32_t)nv->addr * 2654435761u;
    size_t i;
    if (x->t->nvars * 2 + 2 > x->vhcap) {
        size_t nc = x->vhcap ? x->vhcap * 2 : 1024, k;
        uint32_t *nh = sim_xcalloc(nc, sizeof *nh);
        for (k = 0; k < x->vhcap; k++) {
            if (!x->vh[k]) continue;
            {
                const sim_var *v = &x->t->vars[x->vh[k] - 1];
                uint32_t h2 = hstr(v->name) ^ (hstr(v->func) * 31u) ^ (uint32_t)v->addr * 2654435761u;
                size_t j = h2 & (nc - 1);
                while (nh[j]) j = (j + 1) & (nc - 1);
                nh[j] = x->vh[k];
            }
        }
        free(x->vh);
        x->vh = nh;
        x->vhcap = nc;
    }
    for (i = h & (x->vhcap - 1); x->vh[i]; i = (i + 1) & (x->vhcap - 1)) {
        const sim_var *v = &x->t->vars[x->vh[i] - 1];
        if (v->addr == nv->addr && strcmp(v->name, nv->name) == 0 &&
            ((!v->func && !nv->func) || (v->func && nv->func && strcmp(v->func, nv->func) == 0)))
            return 1;
    }
    x->vh[i] = (uint32_t)idx + 1;
    return 0;
}

/* "func(void)" のような名前から引数部分を除く */
static const char *clean_func_name(dctx *x, const char *n)
{
    const char *p;
    if (!n) return NULL;
    p = strchr(n, '(');
    if (!p) return n;
    while (p > n && p[-1] == ' ') p--;
    return ar_strndup(&x->t->ar, n, (size_t)(p - n));
}

static int origin_of(dctx *x, int di)
{
    int r = attr_ref(x, di, AT_specification);
    if (r < 0) r = attr_ref(x, di, AT_abstract_origin);
    return r;
}

static const char *name_of(dctx *x, int di)
{
    const char *n = attr_str(x, di, AT_name);
    int o, guard = 0;
    for (o = origin_of(x, di); !n && o >= 0 && guard < 8; o = origin_of(x, o), guard++) n = attr_str(x, o, AT_name);
    return n;
}

static int flag_of(dctx *x, int di, unsigned at)
{
    uint64_t v = 0;
    int o, guard = 0;
    if (attr_u(x, di, at, &v)) return v != 0;
    for (o = origin_of(x, di); o >= 0 && guard < 8; o = origin_of(x, o), guard++)
        if (attr_u(x, o, at, &v)) return v != 0;
    return 0;
}

static int type_die_of(dctx *x, int di)
{
    int r = attr_ref(x, di, AT_type), o, guard = 0;
    for (o = origin_of(x, di); r < 0 && o >= 0 && guard < 8; o = origin_of(x, o), guard++) r = attr_ref(x, o, AT_type);
    return r;
}

/* DW_AT_location が静的なアドレスなら 1 */
static int static_addr(dctx *x, int di, uint64_t *addr)
{
    const dattr *a = attr(x, di, AT_location);
    const dcu *u = &x->cus[x->dies[di].cu];
    cur_t c;
    if (!a || a->vk != V_BLOCK || a->len < 1) return 0;
    c.p = a->p;
    c.e = a->p + a->len;
    c.bad = 0;
    if (*c.p != 0x03) return 0; /* DW_OP_addr */
    c.p++;
    *addr = rd_n(&c, u->addr_size);
    if (c.bad) return 0;
    while (c.p < c.e) {
        uint8_t op = *c.p++;
        if (op == 0x23) *addr += rd_uleb(&c);   /* DW_OP_plus_uconst */
        else return 0;                          /* それ以外の計算はしない */
        if (c.bad) return 0;
    }
    return 1;
}

static int enclosing_subprogram(const dctx *x, int di)
{
    int p = x->dies[di].parent;
    while (p >= 0) {
        unsigned tag = x->dies[p].tag;
        if (tag == TAG_subprogram) return p;
        if (tag == TAG_compile_unit || tag == TAG_partial_unit) return -1;
        p = x->dies[p].parent;
    }
    return -1;
}

static const sim_type g_unknown_param = {SIM_T_UNKNOWN, "?", 0, NULL, 0, NULL, 0};

static void add_func(dctx *x, int di)
{
    uint64_t lo, hi = 0;
    rl78_funcinfo *fi;
    const dattr *ha;
    int c, n = 0, k = 0, src;
    const char *nm;
    size_t i;
    if (!attr_u(x, di, AT_low_pc, &lo)) return;
    if (flag_of(x, di, AT_declaration) && !attr(x, di, AT_low_pc)) return;
    nm = clean_func_name(x, name_of(x, di));
    if (!nm || !nm[0]) return;
    for (i = 0; i < x->t->nfuncs; i++)
        if (x->t->funcs[i].f.addr == lo && strcmp(x->t->funcs[i].f.name, nm) == 0) return;
    fi = rl78_symtab_add_func(x->t);
    fi->from_dwarf = 1;
    fi->f.name = nm;
    fi->f.file = x->cus[x->dies[di].cu].name ? x->cus[x->dies[di].cu].name : "";
    fi->f.addr = lo;
    ha = attr(x, di, AT_high_pc);
    if (ha && (ha->vk == V_UNS || ha->vk == V_SIGNED)) hi = lo + ha->u;      /* DWARF4: 長さ */
    else if (ha && ha->vk == V_ADDR) hi = ha->u;
    fi->size = hi > lo ? (uint32_t)(hi - lo) : 0;
    fi->f.is_external = flag_of(x, di, AT_external);
    {
        int td = type_die_of(x, di);
        fi->f.ret = td >= 0 ? ty(x, td, 0) : NULL;
        if (fi->f.ret && fi->f.ret->kind == SIM_T_VOID) fi->f.ret = NULL;
    }
    /* 引数: この DIE の子。なければ宣言側の子 */
    src = di;
    for (c = next_child(x, di, -1); c >= 0; c = next_child(x, di, c)) if (x->dies[c].tag == TAG_formal_parameter) n++;
    if (n == 0 && origin_of(x, di) >= 0) {
        src = origin_of(x, di);
        for (c = next_child(x, src, -1); c >= 0; c = next_child(x, src, c)) if (x->dies[c].tag == TAG_formal_parameter) n++;
    }
    if (n) {
        const char **pn = ar_alloc(&x->t->ar, (size_t)n * sizeof *pn);
        const sim_type **pt = ar_alloc(&x->t->ar, (size_t)n * sizeof *pt);
        for (c = next_child(x, src, -1); c >= 0 && k < n; c = next_child(x, src, c)) {
            if (x->dies[c].tag == TAG_unspecified_parameters) fi->f.variadic = 1;
            if (x->dies[c].tag != TAG_formal_parameter) continue;
            pn[k] = name_of(x, c);
            if (!pn[k]) {
                char tmp[32];
                snprintf(tmp, sizeof tmp, "arg%d", k + 1);
                pn[k] = ar_strdup(&x->t->ar, tmp);
            }
            {
                int td = type_die_of(x, c);
                pt[k] = td >= 0 ? ty(x, td, 0) : &g_unknown_param;
                if (!pt[k]) pt[k] = &g_unknown_param;
            }
            k++;
        }
        fi->f.nparams = k;
        fi->f.param_names = (const char *const *)pn;
        fi->f.param_types = (const sim_type *const *)pt;
    }
    for (c = next_child(x, src, -1); c >= 0; c = next_child(x, src, c))
        if (x->dies[c].tag == TAG_unspecified_parameters) fi->f.variadic = 1;
}

static void add_label(dctx *x, int di)
{
    uint64_t lo;
    const char *nm;
    rl78_funcinfo *fi;
    size_t i;
    if (!flag_of(x, di, AT_external) || !attr_u(x, di, AT_low_pc, &lo)) return;
    nm = attr_str(x, di, AT_name);
    if (!nm || !nm[0]) return;
    if (nm[0] == '_') nm++;  /* アセンブラのラベルは C の名前に _ が付いている */
    for (i = 0; i < x->t->nfuncs; i++) if (x->t->funcs[i].f.addr == lo) return;
    fi = rl78_symtab_add_func(x->t);
    fi->f.name = nm;
    fi->f.file = x->cus[x->dies[di].cu].name ? x->cus[x->dies[di].cu].name : "";
    fi->f.addr = lo;
    fi->f.is_external = 1;
    fi->from_dwarf = 1;
}

static void add_var(dctx *x, int di)
{
    uint64_t addr;
    sim_var nv;
    int sp, td;
    if (!static_addr(x, di, &addr)) return;
    memset(&nv, 0, sizeof nv);
    nv.name = name_of(x, di);
    if (!nv.name || !nv.name[0]) return;
    nv.addr = addr;
    nv.file = x->cus[x->dies[di].cu].name ? x->cus[x->dies[di].cu].name : "";
    sp = enclosing_subprogram(x, di);
    if (sp >= 0) {
        nv.func = clean_func_name(x, name_of(x, sp));
        if (!nv.func) return;
        nv.is_external = 0;
    } else {
        nv.is_external = flag_of(x, di, AT_external);
    }
    td = type_die_of(x, di);
    nv.type = td >= 0 ? ty(x, td, 0) : NULL;
    if (!nv.type) {
        sim_type *u = new_type(x, SIM_T_UNKNOWN, "?", 0);
        nv.type = u;
    }
    nv.is_const = td >= 0 && is_const_chain(x, td);
    if (dup_var(x, &nv, x->t->nvars)) return;
    *rl78_symtab_add_var(x->t) = nv;
}

int rl78_dwarf_read(rl78_symtab *t, const rl78_elf *e, const rl78_dwarf_opts *o, rl78_warns *w)
{
    dctx x;
    const rl78_sect *si = rl78_elf_section(e, ".debug_info");
    const rl78_sect *sa = rl78_elf_section(e, ".debug_abbrev");
    const rl78_sect *ss = rl78_elf_section(e, ".debug_str");
    int i, n;
    size_t nv0 = t->nvars, nf0 = t->nfuncs;
    if (!si || !sa || !si->size) return 0;
    memset(&x, 0, sizeof x);
    x.t = t;
    x.e = e;
    x.o = o;
    x.w = w;
    x.info = e->data + si->offset;
    x.info_len = si->size;
    x.abbr = e->data + sa->offset;
    x.abbr_len = sa->size;
    if (ss && ss->type != RL78_SHT_NOBITS) { x.str = e->data + ss->offset; x.str_len = ss->size; }
    n = load_cus(&x);
    t->ncu = x.ncus;
    link_tree(&x);
    x.memo = sim_xcalloc((size_t)x.ndies + 1, sizeof *x.memo);
    x.state = sim_xcalloc((size_t)x.ndies + 1, 1);
    for (i = 0; i < x.ndies; i++) {
        switch (x.dies[i].tag) {
        case TAG_subprogram: add_func(&x, i); break;
        case TAG_label: add_label(&x, i); break;
        default: break;
        }
    }
    for (i = 0; i < x.ndies; i++) if (x.dies[i].tag == TAG_variable) add_var(&x, i);
    if (t->nvars > nv0 || t->nfuncs > nf0) t->dwarf_ok = 1;
    free(x.memo);
    free(x.state);
    free(x.dies);
    free(x.at);
    free(x.cus);
    free(x.vh);
    free(x.fh);
    free(x.fchild);
    free(x.nsib);
    return n;
}

/* ================================================================ 表 */

void rl78_symtab_init(rl78_symtab *t) { memset(t, 0, sizeof *t); }

void rl78_symtab_free(rl78_symtab *t)
{
    free(t->vars);
    free(t->funcs);
    ar_free(&t->ar);
    memset(t, 0, sizeof *t);
}

sim_var *rl78_symtab_add_var(rl78_symtab *t)
{
    if (t->nvars == t->capvars) {
        t->capvars = t->capvars ? t->capvars * 2 : 256;
        t->vars = sim_xrealloc(t->vars, t->capvars * sizeof *t->vars);
    }
    memset(&t->vars[t->nvars], 0, sizeof t->vars[0]);
    return &t->vars[t->nvars++];
}

rl78_funcinfo *rl78_symtab_add_func(rl78_symtab *t)
{
    if (t->nfuncs == t->capfuncs) {
        t->capfuncs = t->capfuncs ? t->capfuncs * 2 : 256;
        t->funcs = sim_xrealloc(t->funcs, t->capfuncs * sizeof *t->funcs);
    }
    memset(&t->funcs[t->nfuncs], 0, sizeof t->funcs[0]);
    return &t->funcs[t->nfuncs++];
}

void rl78_split_symname(rl78_symtab *t, const char *sym, const char **name, const char **func, int *is_static)
{
    const char *p = sym, *at1, *at2;
    if (*p == '_') p++;
    at1 = strchr(p, '@');
    *func = NULL;
    *is_static = 0;
    if (!at1) { *name = ar_strdup(&t->ar, p); return; }
    *name = ar_strndup(&t->ar, p, (size_t)(at1 - p));
    *is_static = 1;
    at2 = strchr(at1 + 1, '@');
    if (at2 && at2[1]) *func = ar_strdup(&t->ar, at2 + 1);
}

static int has_var(const rl78_symtab *t, const char *name, const char *func, uint64_t addr)
{
    size_t i;
    for (i = 0; i < t->nvars; i++) {
        const sim_var *v = &t->vars[i];
        if (v->addr == addr && strcmp(v->name, name) == 0) return 1;
        if (v->addr == addr && !func && !v->func) return 1;
    }
    return 0;
}

static int has_func_at(const rl78_symtab *t, uint64_t addr)
{
    size_t i;
    for (i = 0; i < t->nfuncs; i++) if (t->funcs[i].f.addr == addr) return 1;
    return 0;
}

static sim_type *unknown_sized(rl78_symtab *t, uint32_t size)
{
    sim_type *u = ar_alloc(&t->ar, sizeof *u);
    u->kind = SIM_T_UNKNOWN;
    u->name = "?";
    u->size = size;
    return u;
}

static void add_sym_var(rl78_symtab *t, const char *sym, uint32_t addr, uint32_t size, int global)
{
    const char *name, *func;
    int st;
    sim_var *v;
    rl78_split_symname(t, sym, &name, &func, &st);
    if (!name[0] || has_var(t, name, func, addr)) return;
    v = rl78_symtab_add_var(t);
    v->name = name;
    v->func = func;
    v->file = "";
    v->addr = addr;
    v->type = unknown_sized(t, size);
    v->is_external = global && !st && !func;
    v->is_const = 0;
}

static void add_sym_func(rl78_symtab *t, const char *sym, uint32_t addr, uint32_t size, int global)
{
    const char *name, *func;
    int st;
    rl78_funcinfo *fi;
    size_t i;
    if (has_func_at(t, addr)) {
        /* DWARF 側に大きさがなければ補う */
        for (i = 0; i < t->nfuncs; i++) if (t->funcs[i].f.addr == addr && !t->funcs[i].size) t->funcs[i].size = size;
        return;
    }
    rl78_split_symname(t, sym, &name, &func, &st);
    if (!name[0]) return;
    fi = rl78_symtab_add_func(t);
    fi->f.name = name;
    fi->f.file = "";
    fi->f.addr = addr;
    fi->f.is_external = global && !st;
    fi->size = size;
}

void rl78_symtab_from_elf(rl78_symtab *t, const rl78_elf *e, rl78_warns *w)
{
    int i;
    (void)w;
    for (i = 0; i < e->nsym; i++) {
        const rl78_sym *s = &e->sym[i];
        int global = s->bind == 1 || s->bind == 2;
        const rl78_sect *sec = s->shndx > 0 && s->shndx < e->nsect ? &e->sect[s->shndx] : NULL;
        if (!s->name[0] || s->name[0] == '.' || s->name[0] == '$') continue;
        if (s->type == 1) {                                       /* STT_OBJECT */
            add_sym_var(t, s->name, s->value, s->size, global);
        } else if (s->type == 2) {                                /* STT_FUNC */
            add_sym_func(t, s->name, s->value, s->size, global);
        } else if (s->type == 0 && global && sec && (sec->flags & RL78_SHF_EXEC)) {
            add_sym_func(t, s->name, s->value, 0, global);        /* アセンブラのラベル */
        }
    }
}
