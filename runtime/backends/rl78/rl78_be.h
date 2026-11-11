/*
 * rl78_be.h - RL78 バックエンドの内部定義
 *
 *   rl78_elf.c    ELF32（.abs）の読み取り: セクション、プログラムヘッダ、.symtab
 *   rl78_dwarf.c  DWARF 2〜4 の .debug_info / .debug_abbrev / .debug_str を読み、型・変数・関数の表を作る
 *   rl78_map.c    リンクマップ（.map）の Symbol List と Mapping List
 *   rl78_backend.c  CPU コア（rl78/）との接続、CC-RL の呼び出し規約、スタブ、sim.cfg、sim_backend の実装
 */
#ifndef RL78_BE_H
#define RL78_BE_H

#include "sim_backend.h"

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- 領域（まとめて解放） */

typedef struct rl78_arena_blk rl78_arena_blk;
typedef struct {
    rl78_arena_blk *head;
} rl78_arena;

void *ar_alloc(rl78_arena *a, size_t n);   /* 0 で初期化 */
char *ar_strdup(rl78_arena *a, const char *s);
char *ar_strndup(rl78_arena *a, const char *s, size_t n);
void ar_free(rl78_arena *a);

/* ---------------------------------------------------------------- 警告 */

typedef struct {
    char **msg;
    int n, cap;
    int verbose;     /* 1 なら出たその場で標準エラーにも出す */
} rl78_warns;

#if defined(__GNUC__) || defined(__clang__)
void rl78_warn(rl78_warns *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void rl78_warn(rl78_warns *w, const char *fmt, ...);
#endif
void rl78_warns_free(rl78_warns *w);

/* ---------------------------------------------------------------- ELF */

#define RL78_SHT_PROGBITS 1u
#define RL78_SHT_SYMTAB 2u
#define RL78_SHT_STRTAB 3u
#define RL78_SHT_NOBITS 8u
#define RL78_SHF_WRITE 1u
#define RL78_SHF_ALLOC 2u
#define RL78_SHF_EXEC 4u

typedef struct {
    const char *name;
    uint32_t type, flags, addr, offset, size, link, info, entsize;
} rl78_sect;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags;
} rl78_seg;

typedef struct {
    const char *name;
    uint32_t value, size;
    uint8_t bind, type;   /* STB_*, STT_* */
    uint16_t shndx;
} rl78_sym;

typedef struct {
    uint8_t *data;          /* ファイル全体 */
    size_t len;
    uint16_t machine, etype;
    uint32_t entry, flags;
    rl78_sect *sect;
    int nsect;
    rl78_seg *seg;
    int nseg;
    rl78_sym *sym;
    int nsym;
} rl78_elf;

int rl78_elf_load(rl78_elf *e, const char *path, char *err, size_t errlen);
void rl78_elf_free(rl78_elf *e);
const rl78_sect *rl78_elf_section(const rl78_elf *e, const char *name);
const rl78_sym *rl78_elf_symbol(const rl78_elf *e, const char *name);

/* ---------------------------------------------------------------- シンボル表 */

typedef struct {
    sim_func f;
    uint32_t size;          /* 関数の大きさ（分かれば。0 = 不明） */
    int from_dwarf;
} rl78_funcinfo;

typedef struct {
    rl78_arena ar;
    sim_var *vars;
    size_t nvars, capvars;
    rl78_funcinfo *funcs;
    size_t nfuncs, capfuncs;
    /* DWARF の概要（sim info 用） */
    int ncu, cu_versions[6];  /* cu_versions[v] = バージョン v の CU の数 */
    const char *producer;
    int dwarf_ok;            /* DWARF の変数か関数を 1 つ以上読めた */
    int nskipped_cu;
} rl78_symtab;

/* DWARF 読み取りの指定 */
typedef struct {
    int bitfield_lsb;        /* DWARF2/3 の DW_AT_bit_offset を LSB 側から数える（非標準）なら 1 */
    int addr_class_size[16]; /* DW_AT_address_class の値ごとのポインタのバイト数（0 = 既定） */
} rl78_dwarf_opts;

void rl78_symtab_init(rl78_symtab *t);
void rl78_symtab_free(rl78_symtab *t);
sim_var *rl78_symtab_add_var(rl78_symtab *t);
rl78_funcinfo *rl78_symtab_add_func(rl78_symtab *t);

/* DWARF から変数・関数を足す。DWARF がなければ 0、読めた CU の数を返す */
int rl78_dwarf_read(rl78_symtab *t, const rl78_elf *e, const rl78_dwarf_opts *o, rl78_warns *w);
/* .symtab から、DWARF になかった変数・関数を足す（型は不明） */
void rl78_symtab_from_elf(rl78_symtab *t, const rl78_elf *e, rl78_warns *w);
/* シンボル名（"_name"、"_name@1"、"_name@1@func"）を名前・関数名・グローバルかに分ける。
 * name/func は t->ar に置く。func は関数内 static でなければ NULL */
void rl78_split_symname(rl78_symtab *t, const char *sym, const char **name, const char **func, int *is_static);

/* ---------------------------------------------------------------- .map */

typedef struct {
    const char *name;        /* マップのまま（"_name@1"） */
    const char *file;        /* 直前の FILE=（オブジェクトファイル）。なければ "" */
    uint32_t addr, size;
    char kind;               /* 'f' func, 'd' data, 'n' none */
    int global;
} rl78_mapsym;

typedef struct {
    const char *name;
    uint32_t start, end, size;
} rl78_mapsect;

typedef struct {
    rl78_arena ar;
    rl78_mapsym *sym;
    int nsym, capsym;
    rl78_mapsect *sect;
    int nsect, capsect;
} rl78_map;

int rl78_map_load(rl78_map *m, const char *path);   /* 読めなければ -1 */
void rl78_map_free(rl78_map *m);
/* マップのシンボルから、まだない変数・関数を足す */
void rl78_symtab_from_map(rl78_symtab *t, const rl78_map *m, rl78_warns *w);

#endif
