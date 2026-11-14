/* CI 用: LMF の復元型を、lmfdump.py 由来の期待値とバイト比較する。
 * 実行時には Python・外部コンパイラのインストールを必要としない。 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    rl78_elf e = {0}; rl78_symtab t = {0}; rl78_warns w = {0};
    char err[1024]; size_t i; int nv = 0, nf = 0, bad = 0;
    if (argc != 3) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); /* fc /b 用の期待値は LF 固定 */
#endif
    if (!strcmp(argv[2], "list")) {
        FILE *fp = fopen(argv[1], "rb"); char line[2048]; int n = 0;
        if (!fp) return 2;
        while (fgets(line, sizeof line, fp)) if (!strncmp(line, "var  ", 5) && !strstr(line, " ? ")) {
            size_t len = strlen(line);
            while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
            puts(line); n++;
        }
        bad = ferror(fp) || n != 57; fclose(fp); return bad;
    }
    w.verbose = 1;
    if (rl78_lmf_load(&e, &t, &w, argv[1], 0, err, sizeof err)) {
        fprintf(stderr, "%s\n", err); return 1;
    }
    if (!strcmp(argv[2], "bad")) {
        int found = 0;
        for (i = 0; i < t.nvars; i++) if (!strcmp(t.vars[i].name, "agg")) {
            found++; bad |= t.vars[i].type->kind != SIM_T_UNKNOWN;
        }
        bad |= found != 1 || w.n == 0;
        printf("malformed tag: checked %d, failures %d\n", found, bad);
    } else {
        for (i = 0; i < t.nvars; i++) {
            const sim_var *v = &t.vars[i];
            if (v->type->kind == SIM_T_UNKNOWN) continue;
            printf("V %05llX %u %s%s%s %s [%s]\n", (unsigned long long)v->addr,
                   v->type->size, v->func ? v->func : "", v->func ? "@" : "",
                   v->name, v->type->name, v->file);
            nv++;
        }
        for (i = 0; i < t.nfuncs; i++) {
            const sim_func *f = &t.funcs[i].f; int k;
            if (!f->ret) continue; /* 型なしランタイムラベル */
            printf("F %05llX %s %s %u", (unsigned long long)f->addr, f->name, f->ret->name, f->ret->size);
            for (k = 0; k < f->nparams; k++) printf(" | %s %s %u", f->param_names[k], f->param_types[k]->name, f->param_types[k]->size);
            putchar('\n'); nf++;
        }
        printf("variables %d functions %d warnings %d\n", nv, nf, w.n);
        bad = nv != 57 || nf != 17 || w.n != 0;
    }
    rl78_elf_free(&e); rl78_symtab_free(&t); rl78_warns_free(&w);
    return bad != 0;
}
