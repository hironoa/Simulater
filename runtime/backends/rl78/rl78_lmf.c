/* rl78_lmf.c - CA78K0R LMF。実測仕様 docs/LMF_FORMAT.md に限定する。 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "sim_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int range(size_t len, uint64_t off, uint64_t n) { return off <= len && n <= len - off; }

typedef struct {
    const char *name, *file;
    uint32_t value;
    int sc;
    uint16_t typ, x;
    uint8_t cls, naux;
    const uint8_t *aux;
    uint32_t index;
    sim_type *tag;
    int state; /* 0=未読、1=復元中、2=正常、3=不正 */
    int reported;
} lsymbol;

/* 文字列は必ず表内の NUL まで。固定長名は別途複写する。 */
static const char *longname(const uint8_t *str, size_t len, uint32_t off)
{
    if (off < 4 || off >= len || !memchr(str + off, 0, len - off)) return NULL;
    return (const char *)str + off;
}
static const char *name_at(rl78_arena *ar, const uint8_t *p, size_t n,
                           const uint8_t *str, size_t len)
{
    const uint8_t *end;
    if (!u32(p)) return longname(str, len, u32(p + 4));
    end = memchr(p, 0, n);
    return ar_strndup(ar, (const char *)p, end ? (size_t)(end - p) : n);
}
static const char *cname(const char *s) { return s[0] == '_' ? s + 1 : s; }

/* タグ番号は補助を含む生のエントリ番号。補助を参照してもタグにはならない。 */
typedef struct {
    rl78_symtab *t;
    rl78_warns *w;
    lsymbol *syms, **by_index;
    uint32_t n, count;
    int char_unsigned;
} ltypes;

static int no_type(const lsymbol *s)
{
    const char *p=s->name;
    if ((!s->typ && (!s->x || s->x==2)) || !strncmp(p,"_@",2) || *p=='?') return 1;
    if (*p++!='L' || !*p) return 0;
    for (;*p;p++) if (*p<'0' || *p>'9') return 0;
    return 1;
}
/* Count symbols once even if both storage and type are unsupported, or a
   recursive type definition is visited through several variables. */
static void lmf_issue(ltypes *c,const lsymbol *symbol)
{
    lsymbol *s=&c->syms[symbol-c->syms];
    unsigned section=s->sc>0?0:s->sc==-1?1:s->sc==0?2:3;
    size_t i;
    rl78_lmf_group *g;
    if (s->reported) return;
    s->reported=1;
    if (no_type(s)) { c->w->lmf_untyped++; return; }
    for (i=0;i<c->w->nlmf_groups;i++) {
        g=&c->w->lmf_groups[i];
        if (g->cls==s->cls && g->typ==s->typ && g->derived==s->x && g->section==section) break;
    }
    if (i==c->w->nlmf_groups) {
        c->w->lmf_groups=sim_xrealloc(c->w->lmf_groups,(i+1)*sizeof *g);
        g=&c->w->lmf_groups[c->w->nlmf_groups++]; memset(g,0,sizeof *g);
        g->cls=s->cls; g->typ=s->typ; g->derived=s->x; g->section=section;
    }
    g=&c->w->lmf_groups[i]; g->count++;
    if (g->nnames<3) g->names[g->nnames++]=sim_xstrdup(s->name);
}
static int group_cmp(const void *a,const void *b)
{
    const rl78_lmf_group *x=a,*y=b;
    if (x->count!=y->count) return x->count>y->count?-1:1;
    if (x->cls!=y->cls) return x->cls<y->cls?-1:1;
    if (x->typ!=y->typ) return x->typ<y->typ?-1:1;
    if (x->derived!=y->derived) return x->derived<y->derived?-1:1;
    return x->section<y->section?-1:x->section>y->section;
}
void rl78_lmf_print_summary(const rl78_warns *w,FILE *out)
{
    static const char *sections[]={"normal","absolute","undefined","debug"};
    size_t i;
    fprintf(out,"LMF 型診断: %zu 種類\n",w->nlmf_groups);
    if (w->nlmf_groups) fprintf(out,"storage type derived section count\n");
    for (i=0;i<w->nlmf_groups;i++) {
        const rl78_lmf_group *g=&w->lmf_groups[i]; unsigned j;
        fprintf(out,"0x%02X 0x%04X 0x%04X %s %u",g->cls,g->typ,g->derived,sections[g->section],g->count);
        if (w->lmf_names) for (j=0;j<g->nnames;j++) fprintf(out," %s",g->names[j]);
        fputc('\n',out);
    }
    fprintf(out,"型情報なし（アセンブラなど）: %u 件\n",w->lmf_untyped);
    if (w->nlmf_groups) fprintf(out,"この表を開発者に伝えると対応を追加できます。名前の例は -v -v / --lmf-names で表示します。\n");
}

static sim_type *lmf_type(ltypes *c, const lsymbol *s, int function, unsigned depth);

static const char *type_name(rl78_arena *ar, const sim_type *t)
{
    sim_buf decl, next, out;
    int prev = 0;
    sim_buf_init(&decl);
    while (t && (t->kind == SIM_T_PTR || t->kind == SIM_T_ARRAY || t->kind == SIM_T_FUNC)) {
        sim_buf_init(&next);
        if (t->kind == SIM_T_PTR) {
            sim_buf_adds(&next, t->size == 4 ? "__far *" : "*");
            sim_buf_adds(&next, sim_buf_cstr(&decl));
        } else {
            if (prev == SIM_T_PTR) sim_buf_addc(&next, '(');
            sim_buf_adds(&next, sim_buf_cstr(&decl));
            if (prev == SIM_T_PTR) sim_buf_addc(&next, ')');
            if (t->kind == SIM_T_ARRAY) sim_buf_printf(&next, "[%u]", t->count);
            else sim_buf_adds(&next, "()");
        }
        prev = t->kind; t = t->target;
        sim_buf_free(&decl); decl = next;
    }
    sim_buf_init(&out);
    sim_buf_adds(&out, t && t->name ? t->name : "void");
    if (decl.len) { sim_buf_addc(&out, ' '); sim_buf_adds(&out, sim_buf_cstr(&decl)); }
    {
        const char *name = ar_strdup(ar, sim_buf_cstr(&out));
        sim_buf_free(&decl); sim_buf_free(&out); return name;
    }
}

static sim_type *tag_type(ltypes *c, lsymbol *s, unsigned depth)
{
    sim_type *t;
    sim_member *m;
    uint32_t j, end, nm = 0, k = 0;
    unsigned base = s->typ & 15u;
    if (s->state == 2) return s->tag;
    if (s->state || depth >= 64 || !s->aux || s->naux != 1 || s->x ||
        s->typ != base || !((s->cls == 10 && base == 8) ||
        (s->cls == 12 && base == 9) || (s->cls == 15 && base == 10))) return NULL;
    s->state = 1;
    end = u32(s->aux + 12);
    if (end > c->count || end <= s->index + 3 ||
        !c->by_index[end-2] || c->by_index[end-2]->cls != 0x66) goto bad;
    {
        lsymbol *eos = c->by_index[end-2];
        if (eos->naux != 1 || u32(eos->aux) != s->index ||
            eos->value != u16(s->aux+6) || u16(eos->aux+6) != eos->value) goto bad;
    }
    t = ar_alloc(&c->t->ar, sizeof *t);
    t->kind = base == 8 ? SIM_T_STRUCT : base == 9 ? SIM_T_UNION : SIM_T_INT;
    t->size = u16(s->aux+6);
    if (!t->size || (base == 10 && t->size != 1 && t->size != 2)) goto bad;
    {
        sim_buf name;
        sim_buf_init(&name);
        sim_buf_printf(&name, "%s %s", base == 8 ? "struct" : base == 9 ? "union" : "enum", cname(s->name));
        t->name = ar_strdup(&c->t->ar, sim_buf_cstr(&name)); sim_buf_free(&name);
    }
    for (j = s->index + 2; j < end-2;) {
        lsymbol *v = c->by_index[j];
        if (!v || v->index + 1u + v->naux > end-2) goto bad;
        if (base == 10) {
            if (v->cls != 0x10 || v->typ != 4 || v->x || v->naux) goto bad;
        } else if (v->cls != (base == 8 ? 8 : 11) && v->cls != 0x12) goto bad;
        nm++; j += 1u + v->naux;
    }
    if (base != 10) {
        m = ar_alloc(&c->t->ar, nm * sizeof *m);
        t->members = m; t->nmembers = (int)nm;
        for (j = s->index + 2; j < end-2;) {
            lsymbol *v = c->by_index[j];
            const sim_type *mt = lmf_type(c, v, 0, depth+1);
            m[k].name = cname(v->name); m[k].type = mt; m[k].bit_offset = -1;
            if (mt->kind == SIM_T_UNKNOWN) goto bad;
            if (v->cls == 0x12) {
                unsigned width = v->aux ? u16(v->aux+6) : 0;
                if (v->x || (mt->kind != SIM_T_UINT && mt->kind != SIM_T_INT) ||
                    !width || width > mt->size*8u || v->value > t->size*8u ||
                    width > t->size*8u-v->value) goto bad;
                m[k].bit_offset = (int32_t)v->value; m[k].bit_size = width;
            } else {
                if (v->value > t->size || mt->size > t->size-v->value ||
                    (base == 9 && v->value)) goto bad;
                m[k].offset = v->value;
            }
            k++; j += 1u + v->naux;
        }
    }
    s->tag = t; s->state = 2; return t;
bad:
    s->state = 3;
    lmf_issue(c,s);
    return NULL;
}

static sim_type *lmf_type(ltypes *c, const lsymbol *s, int function, unsigned depth)
{
    static const struct { unsigned size; sim_kind kind; const char *name; } base[16] = {
        {0,SIM_T_UNKNOWN,"?"}, {0,SIM_T_VOID,"void"}, {1,SIM_T_INT,"char"},
        {2,SIM_T_INT,"short"}, {2,SIM_T_INT,"int"}, {4,SIM_T_INT,"long"},
        {4,SIM_T_FLOAT,"float"}, {4,SIM_T_FLOAT,"double"}, {0,SIM_T_UNKNOWN,"?"},
        {0,SIM_T_UNKNOWN,"?"}, {0,SIM_T_UNKNOWN,"?"}, {0,SIM_T_UNKNOWN,"?"},
        {1,SIM_T_UINT,"unsigned char"}, {2,SIM_T_UINT,"unsigned short"},
        {2,SIM_T_UINT,"unsigned int"}, {4,SIM_T_UINT,"unsigned long"}
    };
    sim_type *t, *d;
    unsigned b = s->typ & 15u, q = s->typ & ~15u;
    unsigned x = function ? s->x >> 2 : s->x, levels[8], dims[8], pk[8];
    unsigned n = 0, ai = 0, k, ptrbits = 0;
    if (!s->typ && (!s->x || s->x==2)) { lmf_issue(c,s); goto unknown_label; }
    if (depth >= 64 || s->naux > 1 || s->cls == 0x27 ||
        (q != 0 && q != 0x100 && q != 0x1000 && q != 0x1100 && q != 0x5000 && q != 0x5100 &&
         !(function && s->typ == 0xE001))) goto unknown;
    if (b >= 8 && b <= 10) {
        uint32_t ref = s->aux ? u32(s->aux) : c->count;
        lsymbol *tag = ref < c->count ? c->by_index[ref] : NULL;
        if (!tag || (tag->typ & 15u) != b || !(t = tag_type(c, tag, depth+1))) goto unknown;
    } else {
        if (base[b].kind == SIM_T_UNKNOWN) goto unknown;
        t = ar_alloc(&c->t->ar, sizeof *t);
        t->kind = base[b].kind; t->size = base[b].size; t->name = base[b].name;
        if (b == 2 && c->char_unsigned) { t->kind = SIM_T_UINT; t->name = "unsigned char"; }
    }
    /* 関数補助には寸法/ポインタ種別がない。派生した戻り値は推測しない。 */
    if (function && x) goto unknown;
    if (!function && s->aux) ptrbits = u16(s->aux+18);
    while (x) {
        unsigned lv = x & 3u;
        if (!lv || !s->aux) goto unknown;
        levels[n] = lv; dims[n] = 0; pk[n] = (ptrbits >> (2*n)) & 3u;
        if (lv == 1) {
            if (pk[n] != 1 && pk[n] != 2) goto unknown;
        } else {
            if (pk[n]) goto unknown;
            if (lv == 3) {
                if (ai == 4 || !(dims[n] = u16(s->aux+8+2*ai++))) goto unknown;
            } else if (!n || levels[n-1] != 1) goto unknown;
        }
        n++; x >>= 2;
    }
    if (ptrbits >> (2*n)) goto unknown;
    if (!function && s->aux && s->cls != 0x12)
        for (k = ai; k < 4; k++) if (u16(s->aux+8+2*k)) goto unknown;
    for (k = n; k > 0;) {
        unsigned lv = levels[--k];
        d = ar_alloc(&c->t->ar, sizeof *d);
        d->target = t;
        if (lv == 1) { d->kind = SIM_T_PTR; d->size = pk[k] == 2 ? 4 : 2; }
        else if (lv == 2) d->kind = SIM_T_FUNC;
        else {
            if (!t->size || t->size > UINT32_MAX/dims[k]) goto unknown;
            d->kind = SIM_T_ARRAY; d->count = dims[k]; d->size = t->size*dims[k];
        }
        t = d;
    }
    if (!function && s->aux && s->cls != 0x12 && t->size != u16(s->aux+6)) goto unknown;
    /* タグの共有型に修飾名を書き込まない。 */
    d = ar_alloc(&c->t->ar, sizeof *d); *d = *t;
    d->name = type_name(&c->t->ar, t);
    if (q == 0x5000 || q == 0x5100 || q == 0x1000 || q == 0x1100) {
        sim_buf name;
        sim_buf_init(&name);
        sim_buf_printf(&name, "%s %s", (q & 0x5000) == 0x5000 ? "const" : "volatile", d->name);
        d->name = ar_strdup(&c->t->ar, sim_buf_cstr(&name)); sim_buf_free(&name);
    }
    return d;
unknown:
    lmf_issue(c,s);
unknown_label:
    t = ar_alloc(&c->t->ar, sizeof *t); t->kind = SIM_T_UNKNOWN; t->name = "?";
    t->size = !function && s->aux && s->cls != 0x12 ? u16(s->aux+6) : 0;
    /* 生の整数として読み書きできるのはメモリ上の変数だけ。引数（記憶クラス 9 / 0x11）と戻り値は
       型不明のまま返し、呼び出し規約の対応外チェックでエラーにする（整数の規約で呼ばない）。 */
    if (!function && !depth && s->cls != 9 && s->cls != 0x11 &&
        (t->size==1 || t->size==2 || t->size==4)) { t->kind=SIM_T_UINT; t->name=t->size==1?"uint8":t->size==2?"uint16":"uint32"; }
    return t;
}

int rl78_lmf_load(rl78_elf *e, rl78_symtab *t, rl78_warns *w,
                  const char *path, int char_unsigned, char *err, size_t errlen)
{
    const uint8_t *d, *str;
    uint32_t symptr, count, slen, i, n = 0;
    uint64_t stroff, shoff;
    lsymbol *syms = NULL, **by_index = NULL;
    ltypes types;
    const char *file = "";
    size_t active = (size_t)-1;
    memset(e, 0, sizeof *e);
    e->data = (uint8_t *)sim_read_file(path, &e->len);
    if (!e->data) { snprintf(err, errlen, "%s を読めません", path); return -1; }
    d = e->data;
#define BAD(why) do { snprintf(err, errlen, "LMF: %s: %s", path, why); goto fail; } while (0)
    if (e->len < 20 || u16(d) != 0x1148) BAD("ヘッダが途中で切れています");
    e->nsect = u16(d + 2); shoff = 20u + u16(d + 16);
    if (!range(e->len, 20, u16(d+16)) || !range(e->len, shoff, (uint64_t)e->nsect * 52)) BAD("セクションヘッダが範囲外です");
    symptr = u32(d+8); count = u32(d+12);
    if (!range(e->len, symptr, (uint64_t)count * 20)) BAD("シンボル表の件数または位置が範囲外です");
    stroff = (uint64_t)symptr + (uint64_t)count * 20;
    slen = 0; str = d;
    if (count || (symptr && stroff < e->len)) {
        if (!range(e->len, stroff, 4)) BAD("文字列表の長さがありません");
        slen = u32(d + (size_t)stroff);
        if (slen < 4 || !range(e->len, stroff, slen)) BAD("文字列表が範囲外です");
        str = d + (size_t)stroff;
    }
    e->sect = sim_xcalloc((size_t)e->nsect, sizeof *e->sect);
    e->seg = sim_xcalloc((size_t)e->nsect, sizeof *e->seg);
    for (i = 0; i < (uint32_t)e->nsect; i++) {
        const uint8_t *p = d + (size_t)shoff + i * 52;
        rl78_sect *s = &e->sect[i];
        uint32_t fs = u32(p+20), ptr = u32(p+28);
        const uint8_t *end = memchr(p, 0, 8);
        s->name = ar_strndup(&t->ar, (const char *)p, end ? (size_t)(end-p) : 8);
        s->addr = u32(p+8); s->size = u32(p+16); s->offset = ptr;
        if (s->addr > 0xFFFFF || s->size > 0x100000u - s->addr || fs > s->size ||
            !range(e->len, ptr, fs)) BAD("セクションの内容またはアドレスが範囲外です");
        if (!range(e->len, u32(p+24), (uint64_t)u32(p+40)*12) ||
            !range(e->len, u32(p+36), (uint64_t)u16(p+44)*8)) BAD("セクションの補助表が範囲外です");
        s->type = fs ? RL78_SHT_PROGBITS : RL78_SHT_NOBITS;
        s->flags = RL78_SHF_ALLOC | (p[46] == 0x20 ? 0 : RL78_SHF_WRITE);
        if (p[46] == 0x20 && (p[47] == 0x20 || strstr(s->name, "CODE"))) s->flags |= RL78_SHF_EXEC;
        if (fs) {
            rl78_seg *g = &e->seg[e->nseg++];
            g->type = 1; g->offset = ptr; g->filesz = fs; g->paddr = g->vaddr = s->addr; g->memsz = s->size;
        }
    }
    syms = sim_xcalloc(count ? count : 1, sizeof *syms);
    by_index = sim_xcalloc(count ? count : 1, sizeof *by_index);
    e->sym = sim_xcalloc(count ? count : 1, sizeof *e->sym);
    for (i = 0; i < count;) {
        const uint8_t *p = d + symptr + (size_t)i * 20;
        lsymbol *s = &syms[n++];
        s->index = i; by_index[i] = s;
        s->name = name_at(&t->ar, p, 8, str, slen);
        if (!s->name) BAD("シンボル名の文字列が範囲外または未終端です");
        s->value = u32(p+8); s->sc = (int16_t)u16(p+12); s->typ = u16(p+14);
        s->cls = p[16]; s->naux = p[17]; s->x = u16(p+18);
        if ((uint32_t)s->naux >= count-i) BAD("補助シンボルの件数が範囲外です");
        if (s->sc > e->nsect || s->sc < -2) BAD("シンボルのセクション番号が範囲外です");
        s->aux = s->naux ? p+20 : NULL;
        if (s->cls == 0x67) {
            if (!s->aux) BAD(".file の補助エントリがありません");
            file = name_at(&t->ar, s->aux, 14, str, slen);
            if (!file) BAD("ソースファイル名が範囲外または未終端です");
        }
        s->file = file;
        if (s->cls == 0x26 || s->cls == 0x27 || s->cls == 0x20 || s->cls == 3) {
            rl78_sym *raw = &e->sym[e->nsym++];
            raw->name = s->name; raw->value = s->value; raw->bind = s->cls == 3 ? 0 : 1;
            raw->shndx = s->sc > 0 ? (uint16_t)s->sc : 0;
        }
        i += 1u + s->naux;
    }
    types.t = t; types.w = w; types.syms = syms; types.by_index = by_index;
    types.n = n; types.count = count; types.char_unsigned = char_unsigned;
    for (i = 0; i < n; i++) {
        lsymbol *s = &syms[i];
        int known = 1;
        if (s->name[0]!='.' && (s->sc>0 || s->sc==-1) && !s->typ && (!s->x || s->x==2)) lmf_issue(&types,s);
        if (s->cls == 0x67) { active = (size_t)-1; continue; }
        if (s->cls == 0x65 && !strcmp(s->name, ".ef")) { active = (size_t)-1; continue; }
        if ((s->cls == 0x26 || s->cls == 3) && (s->x & 3) == 2) {
            rl78_funcinfo *fi = rl78_symtab_add_func(t);
            uint32_t j, np = 0, k = 0;
            const char **pn;
            const sim_type **pt;
            fi->f.name = cname(s->name); fi->f.file = s->file; fi->f.addr = s->value;
            fi->f.is_external = s->cls == 0x26;
            fi->f.ret = lmf_type(&types,s,1,0); fi->size = s->aux ? u32(s->aux+4) : 0;
            for (j = i+1; j < n && syms[j].cls != 0x67; j++) {
                if (syms[j].cls == 0x65 && !strcmp(syms[j].name,".ef")) break;
                if ((syms[j].x & 3) == 2 && (syms[j].cls == 3 || syms[j].cls == 0x26)) break;
                if (syms[j].cls == 9 || syms[j].cls == 0x11) np++;
            }
            pn = ar_alloc(&t->ar, (size_t)np*sizeof *pn); pt = ar_alloc(&t->ar, (size_t)np*sizeof *pt);
            fi->f.nparams = (int)np; fi->f.param_names = pn; fi->f.param_types = pt;
            for (j = i+1; k < np; j++) if (syms[j].cls == 9 || syms[j].cls == 0x11) {
                pn[k] = cname(syms[j].name); pt[k++] = lmf_type(&types,&syms[j],0,0);
            }
            active = t->nfuncs-1; t->dwarf_ok = 1;
            continue;
        }
        /* デバッグ型のない公開コードラベルも、セクションがコードと分かる場合は関数として使える。 */
        if (s->cls == 0x26 && !s->typ && !s->x && s->sc > 0 &&
            (e->sect[s->sc-1].flags & RL78_SHF_EXEC)) {
            rl78_funcinfo *fi = rl78_symtab_add_func(t);
            fi->f.name = cname(s->name); fi->f.file = s->file; fi->f.addr = s->value;
            fi->f.is_external = 1;
            continue;
        }
        switch (s->cls) {
        case 8: case 10: case 11: case 12: case 15: case 16: case 18: case 0x66:
            continue;
        case 1: case 4: case 9: case 0x11: case 0x64: case 0x65: case 0x77: case 0x78: continue;
        case 3: case 0x20: case 0x26: case 0x27: break;
        default:
            known = 0;
            lmf_issue(&types,s);
        }
        if ((s->sc > 0 || s->sc==-1) && s->value <= 0xFFFFF && s->name[0] != '.') {
            sim_var *v = rl78_symtab_add_var(t);
            const rl78_sect *sec = s->sc>0?&e->sect[s->sc-1]:NULL;
            const char *owner = s->cls == 3 ? s->file : "";
            uint32_t j, best = 0; int found = 0;
            for (j = 0; j < n; j++) if (syms[j].cls == 0x78 && syms[j].sc == s->sc && syms[j].value <= s->value &&
                                       (!found || syms[j].value >= best)) {
                owner = syms[j].file; best = syms[j].value; found = 1;
            }
            v->name = cname(s->name); v->file = owner; v->addr = s->value;
            v->func = s->cls == 3 && active != (size_t)-1 ? t->funcs[active].f.name : NULL;
            v->is_external = s->cls != 3;
            v->is_const = (sec && !(sec->flags & RL78_SHF_WRITE)) || ((s->typ & 0xF000) == 0x5000);
            v->type = lmf_type(&types,s,0,0);
            if (!known) {
                sim_type *u = ar_alloc(&t->ar,sizeof *u); u->kind = SIM_T_UNKNOWN; u->name = "?";
                u->size = s->aux ? u16(s->aux+6) : 0;
                if (u->size==1 || u->size==2 || u->size==4) { u->kind=SIM_T_UINT; u->name=u->size==1?"uint8":u->size==2?"uint16":"uint32"; }
                v->type = u;
            }
        }
    }
    qsort(w->lmf_groups,w->nlmf_groups,sizeof *w->lmf_groups,group_cmp);
    {
        size_t k; int verbose=w->verbose;
        w->verbose=0;
        for (k=0;k<w->nlmf_groups;k++) {
            const rl78_lmf_group *g=&w->lmf_groups[k];
            rl78_warn(w,"LMF: 記憶クラス 0x%02X / 型コード 0x%04X / 派生型 0x%04X: %u 件",g->cls,g->typ,g->derived,g->count);
        }
        w->verbose=verbose;
        if (verbose || w->lmf_names) rl78_lmf_print_summary(w,stderr);
    }
    free(by_index); free(syms); return 0;
fail:
    free(by_index); free(syms); rl78_elf_free(e); return -1;
#undef BAD
}
