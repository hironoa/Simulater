/* 開発機用: C ローダの像・生シンボル・復元型と逆アセンブルを出す。 */
#include "rl78_be.h"
#include "rl78.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    rl78_elf e = {0}; rl78_symtab t = {0}; rl78_warns w = {0};
    char err[1024]; int i; size_t k;
    if (argc < 2) return 2;
    w.verbose = 1;
    if (rl78_lmf_load(&e,&t,&w,argv[1],0,err,sizeof err)) { fprintf(stderr,"%s\n",err); return 1; }
    for (i=0;i<e.nseg;i++) {
        const rl78_seg *g = &e.seg[i]; uint32_t j;
        for (j=0;j<g->filesz;j++) printf("B %05X %02X\n",g->paddr+j,e.data[g->offset+j]);
    }
    for (i=0;i<e.nsym;i++) printf("S %05X %s\n",e.sym[i].value,e.sym[i].name);
    for (k=0;k<t.nfuncs;k++) {
        rl78_funcinfo *f = &t.funcs[k];
        printf("F %05llX %s %d\n",(unsigned long long)f->f.addr,f->f.name,f->f.nparams);
        if (argc > 2) {
            uint32_t a = (uint32_t)f->f.addr, end = a + f->size;
            printf("D %s\n",f->f.name);
            while (a < end) {
                int j;
                for (j=0;j<e.nseg;j++) if (a >= e.seg[j].paddr && a-e.seg[j].paddr<e.seg[j].filesz) break;
                if (j == e.nseg) break;
                {
                    rl78_seg *g = &e.seg[j]; char text[256];
                    int n = rl78_disasm_buf(RL78_CORE_S3,e.data+g->offset+a-g->paddr,g->filesz-(a-g->paddr),a,text,sizeof text);
                    if (n <= 0) break;
                    printf("D %05X %s\n",a,text); a += (uint32_t)n;
                }
            }
        }
    }
    rl78_elf_free(&e); rl78_symtab_free(&t); rl78_warns_free(&w); return 0;
}
