/* rl78_sym.c - CA78K0R oc78k0r の .sym。型情報は補わない。 */
#define _CRT_SECURE_NO_WARNINGS
#include "rl78_be.h"
#include "sim_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Ldddd は .sym で公開/ローカルが混在し、LMFから所属を区別できない（形式仕様8節）。 */
static int compiler_label(const char *s)
{
    return strlen(s)==5 && s[0]=='L' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) &&
           isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]);
}
static int hexn(const char *p, int n, unsigned *out)
{
    unsigned v=0; int i;
    for(i=0;i<n;i++) {
        unsigned char c=(unsigned char)p[i]; unsigned x;
        if(c>='0' && c<='9') x=c-'0';
        else if(c>='A' && c<='F') x=c-'A'+10;
        else if(c>='a' && c<='f') x=c-'a'+10;
        else return -1;
        v=(v<<4)|x;
    }
    *out=v; return 0;
}
void rl78_sym_load(rl78_elf *e, rl78_symtab *t, rl78_warns *w, const char *path)
{
    FILE *fp=fopen(path,"rb"); char line[2048];
    const char *module=""; int public=1, fallback=e->nsym==0, ok=0, bad=0;
    if(!fp) return;
    while(fgets(line,sizeof line,fp)) {
        size_t n=strlen(line); char *p=line; unsigned attr,addr; int i,found=0,same=0;
        if(n && p[n-1]!='\n' && !feof(fp)) {
            int c; while((c=fgetc(fp))!=EOF && c!='\n') {}
            rl78_warn(w,"SYM: 長すぎる行を読み飛ばします"); continue;
        }
        while(n && (p[n-1]=='\r'||p[n-1]=='\n'||p[n-1]==0x1A)) p[--n]=0;
        if(!strncmp(p,";FF",3)) {
            p+=3; while(isspace((unsigned char)*p)) p++;
            module=ar_strdup(&t->ar,p); public=!strcmp(module,"PUBLIC"); continue;
        }
        if(!n || *p=='#' || *p=='=') continue;
        if(*p=='<') { p++; n--; }
        if(n<8 || hexn(p,2,&attr) || hexn(p+2,5,&addr)) {
            rl78_warn(w,"SYM: 不正なシンボル行を読み飛ばします"); continue;
        }
        p+=7;
        if(attr==3) { /* LMF のビット位置は未確認。アドレス比較も推測しない。 */
            rl78_warn(w,"SYM: %s 未対応の記憶クラス 0x03（bit 属性、照合対象外）",p); continue;
        }
        if(attr>2) { rl78_warn(w,"SYM: %s 未対応の記憶クラス 0x%02X",p,attr); continue; }
        for(i=0;i<e->nsym;i++) if(!strcmp(e->sym[i].name,p) && ((e->sym[i].bind!=0)==public || compiler_label(p))) {
            found=1; if(e->sym[i].value==addr) same=1;
        }
        if(!fallback) {
            if(found && same) ok++;
            else { bad++; rl78_warn(w,"SYM: %s 0x%05X が LMF シンボルと%s",p,addr,found?"食い違います":"一致しません"); }
            continue;
        }
        {
            rl78_sym *raw;
            e->sym=sim_xrealloc(e->sym,(size_t)(e->nsym+1)*sizeof *e->sym);
            raw=&e->sym[e->nsym++]; memset(raw,0,sizeof *raw);
            raw->name=ar_strdup(&t->ar,p); raw->value=addr; raw->bind=(uint8_t)public;
            if(attr==1) {
                rl78_funcinfo *f=rl78_symtab_add_func(t);
                f->f.name=raw->name+(*raw->name=='_'); f->f.addr=addr;
                f->f.file=public?"":module; f->f.is_external=public;
            } else if(attr==2) {
                sim_type *u=ar_alloc(&t->ar,sizeof *u);
                sim_var *v=rl78_symtab_add_var(t);
                u->kind=SIM_T_UNKNOWN; u->name="?";
                v->name=raw->name+(*raw->name=='_'); v->addr=addr; v->file=public?"":module;
                v->is_external=public; v->type=u;
            }
        }
    }
    if(ferror(fp)) rl78_warn(w,"SYM: %s の読み取りに失敗しました",path);
    fclose(fp);
    if(w->verbose) fprintf(stderr,"SYM: %d matched, %d mismatched%s\n",ok,bad,fallback?" (address-only fallback)":"");
}
