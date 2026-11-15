/* rl78_elf.c - ELF32 リトルエンディアン（CC-RL の .abs）の読み取りと、小物（領域、警告） */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "sim_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ 領域 */

struct rl78_arena_blk {
    rl78_arena_blk *next;
    size_t used, cap;
    /* この後ろにデータ */
};

#define ARENA_BLK 65536u

void *ar_alloc(rl78_arena *a, size_t n)
{
    rl78_arena_blk *b = a->head;
    void *p;
    n = (n + 15u) & ~(size_t)15u;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_BLK ? n : ARENA_BLK;
        b = sim_xmalloc(sizeof *b + 16u + cap);
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    p = (char *)b + ((sizeof *b + 15u) & ~(size_t)15u) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *ar_strndup(rl78_arena *a, const char *s, size_t n)
{
    char *p = ar_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *ar_strdup(rl78_arena *a, const char *s) { return s ? ar_strndup(a, s, strlen(s)) : NULL; }

void ar_free(rl78_arena *a)
{
    while (a->head) {
        rl78_arena_blk *n = a->head->next;
        free(a->head);
        a->head = n;
    }
}

/* ================================================================ 警告 */

void rl78_warn(rl78_warns *w, const char *fmt, ...)
{
    sim_buf b;
    va_list ap;
    sim_buf_init(&b);
    va_start(ap, fmt);
    sim_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (w->verbose) fprintf(stderr, "警告: %s\n", sim_buf_cstr(&b));
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 32;
        w->msg = sim_xrealloc(w->msg, (size_t)w->cap * sizeof *w->msg);
    }
    w->msg[w->n++] = b.p ? b.p : sim_xstrdup("");
}

void rl78_warns_free(rl78_warns *w)
{
    int i;
    for (i = 0; i < w->n; i++) free(w->msg[i]);
    free(w->msg);
    for (size_t j=0; j<w->nlmf_groups; j++)
        for (unsigned k=0; k<w->lmf_groups[j].nnames; k++) free(w->lmf_groups[j].names[k]);
    free(w->lmf_groups); w->lmf_groups = NULL; w->nlmf_groups = 0;
    w->msg = NULL;
    w->n = w->cap = 0;
}

/* ================================================================ ELF */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static const char *strtab_at(const rl78_elf *e, const rl78_sect *st, uint32_t off)
{
    const char *s;
    size_t k;
    if (!st || off >= st->size || (size_t)st->offset + st->size > e->len) return "";
    s = (const char *)e->data + st->offset + off;
    /* 終端がセクション内にあることを確かめる */
    for (k = off; k < st->size; k++) if (e->data[st->offset + k] == 0) return s;
    return "";
}

int rl78_elf_load(rl78_elf *e, const char *path, char *err, size_t errlen)
{
    const uint8_t *d;
    uint32_t phoff, shoff;
    uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
    int i;
    memset(e, 0, sizeof *e);
    e->data = (uint8_t *)sim_read_file(path, &e->len);
    if (!e->data) { snprintf(err, errlen, "%s を読めません", path); return -1; }
    d = e->data;
    if (e->len < 52 || memcmp(d, "\x7f" "ELF", 4) != 0) {
        snprintf(err, errlen, "%s は ELF 形式ではありません（CS+ / CC-RL の .abs を指定してください）", path);
        goto fail;
    }
    if (d[4] != 1) { snprintf(err, errlen, "%s は 32 ビット ELF ではありません（ELFCLASS %u）", path, d[4]); goto fail; }
    if (d[5] != 1) { snprintf(err, errlen, "%s はリトルエンディアンの ELF ではありません", path); goto fail; }
    e->etype = le16(d + 16);
    e->machine = le16(d + 18);
    e->entry = le32(d + 24);
    phoff = le32(d + 28);
    shoff = le32(d + 32);
    e->flags = le32(d + 36);
    phentsize = le16(d + 42);
    phnum = le16(d + 44);
    shentsize = le16(d + 46);
    shnum = le16(d + 48);
    shstrndx = le16(d + 50);

    if (phnum) {
        if (phentsize < 32 || (uint64_t)phoff + (uint64_t)phentsize * phnum > e->len) {
            snprintf(err, errlen, "%s: プログラムヘッダがファイルの外を指しています", path);
            goto fail;
        }
        e->seg = sim_xcalloc(phnum, sizeof *e->seg);
        for (i = 0; i < phnum; i++) {
            const uint8_t *p = d + phoff + (size_t)i * phentsize;
            rl78_seg *s = &e->seg[e->nseg++];
            s->type = le32(p);
            s->offset = le32(p + 4);
            s->vaddr = le32(p + 8);
            s->paddr = le32(p + 12);
            s->filesz = le32(p + 16);
            s->memsz = le32(p + 20);
            s->flags = le32(p + 24);
        }
    }
    if (shnum) {
        const rl78_sect *shs;
        if (shentsize < 40 || (uint64_t)shoff + (uint64_t)shentsize * shnum > e->len) {
            snprintf(err, errlen, "%s: セクションヘッダがファイルの外を指しています", path);
            goto fail;
        }
        e->sect = sim_xcalloc(shnum, sizeof *e->sect);
        for (i = 0; i < shnum; i++) {
            const uint8_t *p = d + shoff + (size_t)i * shentsize;
            rl78_sect *s = &e->sect[e->nsect++];
            s->name = "";
            s->type = le32(p + 4);
            s->flags = le32(p + 8);
            s->addr = le32(p + 12);
            s->offset = le32(p + 16);
            s->size = le32(p + 20);
            s->link = le32(p + 24);
            s->info = le32(p + 28);
            s->entsize = le32(p + 36);
            if (s->type != RL78_SHT_NOBITS && (uint64_t)s->offset + s->size > e->len) {
                snprintf(err, errlen, "%s: セクション %d がファイルの外を指しています", path, i);
                goto fail;
            }
        }
        shs = shstrndx < e->nsect ? &e->sect[shstrndx] : NULL;
        for (i = 0; i < e->nsect; i++) {
            const uint8_t *p = d + shoff + (size_t)i * shentsize;
            e->sect[i].name = strtab_at(e, shs, le32(p));
        }
        /* .symtab */
        for (i = 0; i < e->nsect; i++) {
            const rl78_sect *s = &e->sect[i];
            const rl78_sect *st;
            uint32_t es, k, n;
            if (s->type != RL78_SHT_SYMTAB) continue;
            es = s->entsize >= 16 ? s->entsize : 16;
            st = s->link < (uint32_t)e->nsect ? &e->sect[s->link] : NULL;
            n = s->size / es;
            e->sym = sim_xrealloc(e->sym, (size_t)(e->nsym + (int)n + 1) * sizeof *e->sym);
            for (k = 0; k < n; k++) {
                const uint8_t *p = d + s->offset + (size_t)k * es;
                rl78_sym *y = &e->sym[e->nsym++];
                y->name = strtab_at(e, st, le32(p));
                y->value = le32(p + 4);
                y->size = le32(p + 8);
                y->bind = (uint8_t)(p[12] >> 4);
                y->type = (uint8_t)(p[12] & 15u);
                y->shndx = le16(p + 14);
            }
        }
    }
    return 0;
fail:
    rl78_elf_free(e);
    return -1;
}

void rl78_elf_free(rl78_elf *e)
{
    free(e->data);
    free(e->sect);
    free(e->seg);
    free(e->sym);
    memset(e, 0, sizeof *e);
}

const rl78_sect *rl78_elf_section(const rl78_elf *e, const char *name)
{
    int i;
    for (i = 0; i < e->nsect; i++) if (strcmp(e->sect[i].name, name) == 0) return &e->sect[i];
    return NULL;
}

const rl78_sym *rl78_elf_symbol(const rl78_elf *e, const char *name)
{
    int i;
    for (i = 0; i < e->nsym; i++) if (strcmp(e->sym[i].name, name) == 0) return &e->sym[i];
    return NULL;
}
