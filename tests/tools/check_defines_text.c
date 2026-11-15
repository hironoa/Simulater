/* Public definition-loading contract, also run with ASan/UBSan on POSIX. */
#include "sim_defines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr,"FAIL text line %d: %s\n",__LINE__,#x); } } while (0)
static sim_ctx *load(const char *cfg,sim_backend **backend)
{
    sim_open_opts o={0};
    char err[2048];
    sim_ctx *c;
    o.cfg_path=cfg;
    o.verbose=strstr(cfg,"text-stress")==NULL;
    *backend=sim_backend_open(&o,err,sizeof err);
    CHECK(*backend!=NULL);
    if (!*backend) return NULL;
    c=sim_ctx_new(*backend);
    CHECK(sim_defines_load(c,&o,err,sizeof err)==0);
    return c;
}
static void close_ctx(sim_ctx *c,sim_backend *b) { sim_ctx_free(c); b->close(b); }
static void variable(sim_ctx *c,const char *name,uint64_t addr,unsigned size,unsigned count)
{
    size_t i;
    const sim_var *v=NULL;
    for (i=0;i<sim_ctx_sym_count(c);i++) {
        const sim_var *p=sim_ctx_sym_var(c,i);
        if (p && !strcmp(p->name,name)) { v=p; break; }
    }
    CHECK(v!=NULL);
    if (!v) return;
    CHECK(v->addr==addr);
    CHECK(v->type->size==size);
    if (count) CHECK(v->type->kind==SIM_T_ARRAY && v->type->count==count);
}
static void info_has(sim_ctx *c,const char *text)
{
    FILE *f=tmpfile();
    char buf[16384]; size_t n;
    CHECK(f!=NULL); if (!f) return;
    sim_ctx_print_definition_info(c,f); rewind(f);
    n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    CHECK(strstr(buf,text)!=NULL);
}
int main(int argc,char **argv)
{
    char path[4096],cfg[4096],src[4096];
    sim_backend *b;
    sim_ctx *c;
    unsigned i;
    static const char *names[]={"UTF8_REG","UTF8_BLOCK","UTF8_BOM","UTF8_BOM_BLOCK","SJIS_SLASH","SJIS_BLOCK","SJIS_STRING","SJIS_CHAR","SJIS_IF","SJIS_TYPE","SJIS_INVALID_NL","SJIS_INVALID_STAR","SJIS_INVALID_SLASH","SJIS_INVALID_QUOTE","SJIS_KANA","ASCII_REG"};
    if (argc!=3) return 2;
    snprintf(path,sizeof path,"%s/defines/encoding.cfg",argv[1]);
    c=load(path,&b); if (!c) return 1;
    for (i=0;i<sizeof names/sizeof names[0];i++) variable(c,names[i],0xF930+i,1,0);
    info_has(c,"variables=16 bits=0 arrays=0 guessed=0 unresolved=0");
    info_has(c,"構文を追えなかったファイル: 0 件");
    info_has(c,"defines encoding: UTF8=2 ShiftJIS=1 ASCII=1");
    close_ctx(c,b);
    snprintf(path,sizeof path,"%s/defines/expansion.cfg",argv[1]);
    c=load(path,&b); if (!c) return 1;
    variable(c,"EXP_BYTES",0xF940,5,5);
    variable(c,"EXP_TAIL",0xF955,1,0);
    variable(c,"EXP_ADDR",0xF920,1,0);
    variable(c,"EXP_IF",0xF960,1,0);
    variable(c,"EXP_NEST",0xF970,5,5);
    variable(c,"EXP_BIT",0xF980,1,0);
    variable(c,"EXP_PARTIAL",0xF990,7,7);
    variable(c,"EXP_OPEN",0xF9A0,6,6);
    variable(c,"EXP_CLOSE",0xF9B0,6,6);
    {
        char err[256]; unsigned char byte=0;
        sim_ref *r=sim_ref_parse(c,"EXP_BIT",err,sizeof err);
        sim_cellval value;
        CHECK(r!=NULL);
        CHECK(sim_parse_cell("1",1,&value,err,sizeof err)==0);
        if (r) CHECK(sim_ref_write(c,r,&value,err,sizeof err)==0);
        CHECK(b->read(b,0xF980,&byte,1,err,sizeof err)==0 && byte==0x20);
        sim_cellval_free(&value); sim_ref_free(r);
    }
    info_has(c,"variables=3 bits=1 arrays=5 guessed=0 unresolved=0");
    close_ctx(c,b);
    snprintf(path,sizeof path,"%s/defines/rejected.cfg",argv[1]);
    c=load(path,&b); if (!c) return 1;
    info_has(c,"variables=0 bits=0 arrays=0 guessed=0 unresolved=7");
    close_ctx(c,b);
    snprintf(path,sizeof path,"%s/defines/diagnostics.cfg",argv[1]);
    c=load(path,&b); if (!c) return 1;
    info_has(c,"UTF-16 のため読まなかったファイル: 3 件");
    info_has(c,"構文を追えなかったファイル: 7 件");
    info_has(c,"if.h:2");
    info_has(c,"block.h:1");
    info_has(c,"paren.h:2");
    info_has(c,"brace.h:2");
    close_ctx(c,b);
    /* Deterministic malformed bytes, every prefix of a source, a huge line,
       deep delimiters and exponential/empty macro expansion. */
    snprintf(cfg,sizeof cfg,"%s/text-stress.cfg",argv[2]);
    snprintf(src,sizeof src,"%s/text-stress.h",argv[2]);
    { FILE *f=fopen(cfg,"wb"); CHECK(f!=NULL); if (!f) return 1; fprintf(f,"defines = %s\n",src); fclose(f); }
    for (i=0;i<200;i++) {
        FILE *f=fopen(src,"wb"); unsigned j; uint32_t seed=i+1;
        CHECK(f!=NULL); if (!f) return 1;
        if (i<64) for (j=0;j<2048;j++) { seed=seed*1664525u+1013904223u; fputc((int)(seed>>24),f); }
        else if (i<196) {
            static const char sample[]="// \xe3\x81\x82\n#define COUNT 1+2\ntypedef struct { unsigned char a[COUNT*2]; } T;\n#if COUNT*2==5\n#define REG (*(T*)0xF900)\n#endif\n";
            size_t n=i-64; if (n>sizeof sample-1) n=sizeof sample-1;
            fwrite(sample,1,n,f);
        } else if (i==196) { for (j=0;j<1024*1024;j++) fputc('A',f); }
        else if (i==197) { fputs("#define REG (*(unsigned char*)",f); for (j=0;j<4096;j++) fputc('(',f); fputs("1\n",f); }
        else if (i==198) { for (j=0;j<20;j++) fprintf(f,"#define D%u D%u D%u\n",j,j+1,j+1); fputs("#define D20\n#if D0\n#endif\n",f); }
        else { for (j=0;j<4096;j++) fputs("#if 1\n",f); }
        fclose(f);
        c=load(cfg,&b); if (!c) return 1;
        close_ctx(c,b);
    }
    printf("defines text: %u checks, %u failed, 200 malformed inputs\n",checks,failed);
    return !checks || failed;
}
