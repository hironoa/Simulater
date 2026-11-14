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

/* 基本型コードと派生型の検証を分け、実物で判明したコードを追加できるようにする。
 * 未観測の char/short/unsigned long コードも現段階では復元しない。 */
static sim_type *lmf_type(rl78_symtab *t, const lsymbol *s, int function, rl78_warns *w)
{
    static const struct { unsigned code, size; sim_kind kind; const char *name; } base[] = {
        {1,0,SIM_T_VOID,"void"}, {4,2,SIM_T_INT,"int"}, {5,4,SIM_T_INT,"long"},
        {12,1,SIM_T_UINT,"unsigned char"}, {13,2,SIM_T_UINT,"unsigned short"},
        {14,2,SIM_T_UINT,"unsigned int"}
    };
    sim_type *b = ar_alloc(&t->ar, sizeof *b), *d;
    unsigned i, x = function ? s->x >> 2 : s->x;
    unsigned quals = s->typ & ~15u;
    int ok = 0;
    b->kind = SIM_T_UNKNOWN; b->name = "?";
    if (!function && s->aux) b->size = u16(s->aux + 6);
    if (!s->typ && !s->x && (s->cls == 3 || s->cls == 0x26)) return b;
    for (i = 0; i < sizeof base / sizeof base[0]; i++) if ((s->typ & 15u) == base[i].code) {
        b->kind = base[i].kind; b->name = base[i].name; b->size = base[i].size; ok = 1; break;
    }
    if (quals != 0 && quals != 0x100 && quals != 0x1000 && quals != 0x1100 &&
        !(x == 1 && (quals == 0x5000 || quals == 0x5100)) &&
        !(function && s->typ == 0xE001)) ok = 0;
    if (s->cls == 0x27 || s->naux > 1) ok = 0;
    if (x) {
        /* 派生型の鎖、多次元、far、関数ポインタは未確認。 */
        if (!s->aux || function || (x != 1 && x != 3) || u32(s->aux)) ok = 0;
        else if (x == 1 && (u16(s->aux + 6) != 2 || u16(s->aux + 18) != 1)) ok = 0;
        else if (x == 3 && (!u16(s->aux + 8) || u16(s->aux + 10) || u16(s->aux + 12) || u16(s->aux + 14) ||
                           (uint32_t)u16(s->aux + 8) * b->size != u16(s->aux + 6))) ok = 0;
    }
    if (!ok) {
        b->kind = SIM_T_UNKNOWN; b->name = "?";
        b->size = !function && s->aux ? u16(s->aux + 6) : 0;
        rl78_warn(w, "LMF: %s 未対応の型コード 0x%04X 派生型 0x%04X / 記憶クラス 0x%02X", s->name, s->typ, s->x, s->cls);
        return b;
    }
    if (!x) return b;
    d = ar_alloc(&t->ar, sizeof *d);
    d->kind = x == 1 ? SIM_T_PTR : SIM_T_ARRAY;
    d->name = x == 1 ? "near pointer" : "array";
    d->target = b; d->size = u16(s->aux + 6);
    if (x == 3) d->count = u16(s->aux + 8);
    return d;
}

int rl78_lmf_load(rl78_elf *e, rl78_symtab *t, rl78_warns *w,
                  const char *path, char *err, size_t errlen)
{
    const uint8_t *d, *str;
    uint32_t symptr, count, slen, i, n = 0;
    uint64_t stroff, shoff;
    lsymbol *syms = NULL;
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
    e->sym = sim_xcalloc(count ? count : 1, sizeof *e->sym);
    for (i = 0; i < count;) {
        const uint8_t *p = d + symptr + (size_t)i * 20;
        lsymbol *s = &syms[n++];
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
    for (i = 0; i < n; i++) {
        lsymbol *s = &syms[i];
        int known = 1;
        if (s->cls == 0x67) { active = (size_t)-1; continue; }
        if (s->cls == 0x65 && !strcmp(s->name, ".ef")) { active = (size_t)-1; continue; }
        if ((s->cls == 0x26 || s->cls == 3) && (s->x & 3) == 2) {
            rl78_funcinfo *fi = rl78_symtab_add_func(t);
            uint32_t j, np = 0, k = 0;
            const char **pn;
            const sim_type **pt;
            fi->f.name = cname(s->name); fi->f.file = s->file; fi->f.addr = s->value;
            fi->f.is_external = s->cls == 0x26;
            fi->f.ret = lmf_type(t,s,1,w); fi->size = s->aux ? u32(s->aux+4) : 0;
            for (j = i+1; j < n && syms[j].cls != 0x67; j++) {
                if (syms[j].cls == 0x65 && !strcmp(syms[j].name,".ef")) break;
                if ((syms[j].x & 3) == 2 && (syms[j].cls == 3 || syms[j].cls == 0x26)) break;
                if (syms[j].cls == 9 || syms[j].cls == 0x11) np++;
            }
            pn = ar_alloc(&t->ar, (size_t)np*sizeof *pn); pt = ar_alloc(&t->ar, (size_t)np*sizeof *pt);
            fi->f.nparams = (int)np; fi->f.param_names = pn; fi->f.param_types = pt;
            for (j = i+1; k < np; j++) if (syms[j].cls == 9 || syms[j].cls == 0x11) {
                pn[k] = cname(syms[j].name); pt[k++] = lmf_type(t,&syms[j],0,w);
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
        case 1: case 4: case 9: case 0x11: case 0x20: case 0x64: case 0x65: case 0x77: case 0x78: continue;
        case 3: case 0x26: case 0x27: break;
        default:
            known = 0;
            rl78_warn(w,"LMF: %s 未対応の記憶クラス 0x%02X / 型コード 0x%04X",s->name,s->cls,s->typ);
        }
        if (s->sc > 0 && s->value <= 0xFFFFF && s->name[0] != '.') {
            sim_var *v = rl78_symtab_add_var(t);
            const rl78_sect *sec = &e->sect[s->sc-1];
            const char *owner = s->cls == 3 ? s->file : "";
            uint32_t j, best = 0; int found = 0;
            for (j = 0; j < n; j++) if (syms[j].cls == 0x78 && syms[j].sc == s->sc && syms[j].value <= s->value &&
                                       (!found || syms[j].value >= best)) {
                owner = syms[j].file; best = syms[j].value; found = 1;
            }
            v->name = cname(s->name); v->file = owner; v->addr = s->value;
            v->func = s->cls == 3 && active != (size_t)-1 ? t->funcs[active].f.name : NULL;
            v->is_external = s->cls == 0x26 || s->cls == 0x27;
            v->is_const = !(sec->flags & RL78_SHF_WRITE) || ((s->typ & 0xF000) == 0x5000 && s->x == 1);
            v->type = lmf_type(t,s,0,w);
            if (!known) {
                sim_type *u = ar_alloc(&t->ar,sizeof *u); u->kind = SIM_T_UNKNOWN; u->name = "?";
                u->size = s->aux ? u16(s->aux+6) : 0; v->type = u;
            }
        }
    }
    free(syms); return 0;
fail:
    free(syms); rl78_elf_free(e); return -1;
#undef BAD
}
