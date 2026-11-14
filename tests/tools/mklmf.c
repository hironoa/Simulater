/* mklmf.c - 実物を再配布しない LMF 生成器。GNU ツールは不要。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lmfprog_code.h"
static unsigned char image[32768];
static size_t sy, pos, strings = 4;
static unsigned count;
static unsigned char str[8192];
static void w16(size_t p, unsigned v) { image[p]=(unsigned char)v; image[p+1]=(unsigned char)(v>>8); }
static void w32(size_t p, unsigned v) { w16(p,v); w16(p+2,v>>16); }
static void name(size_t p, const char *s)
{
    size_t n=strlen(s);
    if (n<=8) memcpy(image+p,s,n);
    else { w32(p+4,(unsigned)strings); memcpy(str+strings,s,n+1); strings+=n+1; }
}
static size_t sym(const char *n,unsigned value,int sc,unsigned typ,unsigned cls,unsigned x,int aux)
{
    size_t p=pos; name(p,n); w32(p+8,value); w16(p+12,(unsigned)sc); w16(p+14,typ);
    image[p+16]=(unsigned char)cls; image[p+17]=(unsigned char)aux; w16(p+18,x);
    pos+=20u*(1u+(unsigned)aux); count+=1u+(unsigned)aux; return p+20;
}
static void var(const char *n,unsigned addr,unsigned typ,unsigned x,unsigned size,unsigned dim,unsigned cls)
{
    size_t a=sym(n,addr,2,typ,cls,x,size!=0);
    if (size) { w16(a+6,size); w16(a+8,dim); if(x==1) w16(a+18,1); }
}
static unsigned code_size(unsigned addr)
{
    static const unsigned labels[] = { LP_main,LP_echo8,LP_echo16,LP_second8,LP_second16,LP_second32,LP_ret32,LP_readptr,LP_echoptr,LP_cycle,LP_first32,LP_third32,sizeof LP_IMAGE };
    unsigned i,end=sizeof LP_IMAGE;
    for(i=0;i<sizeof labels/sizeof labels[0];i++) if(labels[i]>addr && labels[i]<end) end=labels[i];
    return end-addr;
}
static void func(const char *n,unsigned addr,unsigned ret,unsigned p1,unsigned p2)
{
    size_t a=sym(n,addr,1,ret,0x26,2,1); w32(a+4,code_size(addr)); w32(a+16,1);
    sym(".bf",addr,1,0,0x65,0,1);
    if(!strcmp(n,"_cycle")) var("_local",0xFE01A,13,0,0,0,3);
    if(p1) sym("_a",0,-1,p1,0x11,0,0);
    if(p2) sym("_b",4,-1,p2,9,0,0);
    sym(".ef",addr+1,1,0,0x65,0,1);
}
static void ptrfunc(const char *n,unsigned addr,int retptr)
{
    size_t a=sym(n,addr,1,12,0x26,retptr?6:2,1); w32(a+4,code_size(addr)); w32(a+16,1);
    sym(".bf",addr,1,0,0x65,0,1);
    a=sym("_a",0,-1,12,0x11,1,1); w16(a+6,2); w16(a+18,1);
    sym(".ef",addr+1,1,0,0x65,0,1);
}
int main(int argc,char **argv)
{
    FILE *f; size_t a, tag_aux, member_aux, agg_aux; unsigned tag_index; const char *mode;
    if(argc!=3) return 2;
    mode=argv[1];
    w16(0,0x1148); w16(2,2); w16(16,68); w16(18,3);
    memcpy(image+88,"@@CODE",6); w32(88+16,sizeof LP_IMAGE); w32(88+20,sizeof LP_IMAGE); w32(88+28,192); image[88+46]=0x20; image[88+47]=0x20;
    memcpy(image+140,"@@DATA",6); w32(140+8,0xFE000); w32(140+12,0xFE000); w32(140+16,64); image[140+46]=0x40; image[140+47]=0x80;
    memcpy(image+192,LP_IMAGE,sizeof LP_IMAGE);
    sy=pos=(192+sizeof LP_IMAGE+3)&~(size_t)3;
    a=sym(".file",0,-2,0x3F,0x67,0,1); memcpy(image+a,"lmf_test.c",10);
    sym("lmf_test",0,-2,0,0x77,0,0); sym("@@DATA",0xFE000,2,0,0x78,0,0);
    tag_index=count;
    tag_aux=sym("_pair",0,-2,8,10,0,1); w16(tag_aux+6,4);
    member_aux=sym("_low",0,-1,12,0x12,0,1); w16(member_aux+6,3);
    a=sym("_high",3,-1,12,0x12,0,1); w16(a+6,5);
    sym("_word",2,-1,14,8,0,0);
    a=sym(".eos",4,-1,0,0x66,0,1); w32(a,tag_index); w16(a+6,4);
    w32(tag_aux+12,count);
    agg_aux=sym("_agg",0xFE020,2,8,0x26,0,1); w32(agg_aux,tag_index); w16(agg_aux+6,4);
    a=sym("_faragg",0xFE024,2,8,0x26,1,1); w32(a,tag_index); w16(a+6,4); w16(a+18,2);
    func("_main",LP_main,1,0,0);
    func("_echo8",LP_echo8,12,12,0); func("_echo16",LP_echo16,4,4,0);
    func("_second8",LP_second8,12,12,12); func("_second16",LP_second16,13,13,13);
    func("_second32",LP_second32,5,13,5); func("_ret32",LP_ret32,5,0,0);
    func("_cycle",LP_cycle,1,0,0); func("_first32",LP_first32,1,5,0);
    ptrfunc("_readptr",LP_readptr,0);
    /* 関数戻りの派生型は未確認なので echoptr は意図的に未知型になる。 */
    ptrfunc("_echoptr",LP_echoptr,1);
    a=sym("_third32",LP_third32,1,5,0x26,2,1); w32(a+4,code_size(LP_third32)); w32(a+16,1);
    sym(".bf",LP_third32,1,0,0x65,0,1);
    sym("_a",0,-1,12,0x11,0,0); sym("_b",4,-1,12,9,0,0); sym("_c",6,-1,5,9,0,0);
    sym(".ef",LP_third32+code_size(LP_third32)-1,1,0,0x65,0,1);
    var("_init",0xFE000,13,0,0,0,0x26); var("_ticks",0xFE002,13,0,0,0,0x26);
    var("_arr",0xFE004,12,3,4,4,0x26); var("_ptr",0xFE008,12,1,2,0,0x26);
    var("_unknown",0xFE00A,8,0,6,0,0x26); var("_mystery",0xFE010,13,0,2,0,0x55);
    var("_vol",0xFE012,0x100D,0,0,0,0x26); var("_float",0xFE014,6,0,0,0,0x26);
    var("_private",0xFE018,13,0,0,0,3);
    sym("_@STBEG",0xFFDE0,-1,0,0x20,0,0); sym("_@STEND",0xFF000,-1,0,0x20,0,0);
    w32(8,(unsigned)sy); w32(12,count); w32(pos,(unsigned)strings); memcpy(image+pos+4,str+4,strings-4); pos+=strings;
    if(!strcmp(mode,"truncated")) pos=19;
    else if(!strcmp(mode,"section")) w32(88+28,0xFFFFFFF0u);
    else if(!strcmp(mode,"symbols")) w32(12,0xFFFFFFFFu);
    else if(!strcmp(mode,"aux")) image[sy+17]=255;
    else if(!strcmp(mode,"string")) w32(sy+4,0xFFFFFFFFu),w32(sy,0);
    else if(!strcmp(mode,"unterminated")) image[pos-1]=1;
    else if(!strcmp(mode,"stringcut")) pos--;
    else if(!strcmp(mode,"address")) w32(88+8,0xFFFFF);
    else if(!strcmp(mode,"lines")) w32(88+36,0xFFFFFFF0u),w16(88+44,1);
    else if(!strcmp(mode,"magic")) w32(0,0x12345678);
    else if(!strcmp(mode,"nodbg")) { w32(8,0); w32(12,0); pos=sy; }
    else if(!strcmp(mode,"tagref")) w32(agg_aux,0xFFFFFFFFu);
    else if(!strcmp(mode,"tagaux")) w32(agg_aux,tag_index+1);
    else if(!strcmp(mode,"tagmismatch")) w16(agg_aux-20+14,9);
    else if(!strcmp(mode,"tagend")) w32(tag_aux+12,0xFFFFFFFFu);
    else if(!strcmp(mode,"tagcycle")) {
        w16(member_aux-20+14,8); image[member_aux-20+16]=8;
        w32(member_aux,tag_index); w16(member_aux+6,4);
    }
    else if(!strcmp(mode,"tagwidth")) w16(member_aux+6,33);
    else if(strcmp(mode,"normal")) return 2;
    f=fopen(argv[2],"wb"); if(!f) return 1;
    if(fwrite(image,1,pos,f)!=pos) { fclose(f); return 1; }
    return fclose(f)!=0;
}
