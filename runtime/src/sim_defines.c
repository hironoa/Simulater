#define _CRT_SECURE_NO_WARNINGS
#include "sim_defines.h"
#include "sim_cfg.h"
#include "sim_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name, *body, *file; unsigned line; int state; const sim_type *type; } definition;
typedef struct {
    sim_ctx *ctx;
    sim_cfg cfg;
    char **files;
    size_t nfiles;
    definition *macros, *types;
    size_t nmacros, ntypes;
    size_t *macro_index, *type_index, macro_cap, type_cap;
    int recursive, guess, char_unsigned, verbose, error;
    uint64_t near_base;
    unsigned variables, bits, arrays, guessed, unresolved, duplicates, pointers, explicit_vars;
    char *err; size_t cap;
} defs;
typedef struct { const char *p, *start; char tok[128]; } lexer;

static void next(lexer *l)
{
    const char *s;
    size_t n;
    while (isspace((unsigned char)*l->p)) l->p++;
    l->start = s = l->p;
    if (sim_is_ident_start((unsigned char)*s)) {
        l->p++; while (sim_is_ident_char((unsigned char)*l->p)) l->p++;
    } else if (isdigit((unsigned char)*s)) {
        l->p++; while (isalnum((unsigned char)*l->p)) l->p++;
    } else if (*s) {
        l->p++;
        if ((!strncmp(s, "->", 2)) || !strncmp(s, "<<", 2)) l->p++;
    }
    n = (size_t)(l->p - s); if (n >= sizeof l->tok) n = sizeof l->tok - 1;
    memcpy(l->tok, s, n); l->tok[n] = 0;
}
static lexer lex(const char *s) { lexer l; memset(&l, 0, sizeof l); l.p = s; next(&l); return l; }
static char *owned_string(defs *d, const char *s)
{ char *p = sim_ctx_alloc(d->ctx, strlen(s)+1); strcpy(p,s); return p; }
static int qualifier(const char *s)
{ return !strcmp(s,"volatile") || !strcmp(s,"const") || !strcmp(s,"__near") || !strcmp(s,"__far") || !strcmp(s,"__sreg"); }
static int onoff(defs *d, const char *key, int def)
{
    const char *s = sim_cfg_get(&d->cfg,key);
    if (!s) return def;
    if (!strcmp(s,"on")) return 1;
    if (!strcmp(s,"off")) return 0;
    snprintf(d->err,d->cap,"%s は on / off で指定してください",key); d->error=1; return def;
}
static int wildcard(const char *p, const char *s)
{
    const char *star=NULL, *retry=NULL;
    while (*s) {
        if (*p && *p != '*' && tolower((unsigned char)*p) == tolower((unsigned char)*s)) { p++; s++; }
        else if (*p == '*') { star=p++; retry=s; }
        else if (star) { p=star+1; s=++retry; }
        else return 0;
    }
    while (*p=='*') p++;
    return !*p;
}
static int excluded(defs *d, const char *path)
{
    int i;
    for (i=0;i<d->cfg.n;i++) if (!strcmp(d->cfg.e[i].key,"defines_exclude")) {
        const char *p=d->cfg.e[i].value;
        if (strchr(p,'*') ? wildcard(p,path) || wildcard(p,sim_basename(path)) : strstr(path,p)!=NULL) return 1;
    }
    return 0;
}
static int source_extension(const char *p)
{
    const char *e=strrchr(p,'.');
    return e && (wildcard(".h",e) || wildcard(".c",e) || wildcard(".inc",e));
}
static void add_file(defs *d, const char *path)
{
    size_t i;
    if (excluded(d,path)) return;
    for (i=0;i<d->nfiles;i++) if (!strcmp(d->files[i],path)) return;
    d->files=sim_xrealloc(d->files,(d->nfiles+1)*sizeof *d->files);
    d->files[d->nfiles++]=sim_xstrdup(path);
}
typedef struct { defs *d; const char *dir, *pattern; unsigned depth; } walk;
static void visit_file(void *ctx, const char *name, int kind)
{
    walk *w=ctx;
    char *path;
    if (name[0]=='.' || w->d->error) return;
    path=sim_path_join(w->dir,name);
    if (!excluded(w->d,path)) {
        if (kind==2 && w->d->recursive && w->depth<64) {
            walk child={w->d,path,w->pattern,w->depth+1};
            if (sim_dir_each(path,visit_file,&child)) { snprintf(w->d->err,w->d->cap,"defines: フォルダ %s を読めません",path); w->d->error=1; }
        } else if (kind==1 && source_extension(path) && (!w->pattern || wildcard(w->pattern,path))) add_file(w->d,path);
    }
    free(path);
}
static void collect_path(defs *d, const char *s)
{
    char *p=sim_cfg_path(&d->cfg,s);
    int kind=sim_path_kind(p);
    if (strchr(p,'*')) {
        char *q=strchr(p,'*'), *slash=q;
        char *dir;
        while (slash>p && slash[-1]!='/' && slash[-1]!='\\') slash--;
        dir=slash>p ? sim_xstrndup(p,(size_t)(slash-p)) : sim_xstrdup(".");
        { walk w={d,dir,p,0}; if (sim_dir_each(dir,visit_file,&w)) d->error=1; }
        free(dir);
    } else if (kind==2) {
        walk w={d,p,NULL,0}; if (sim_dir_each(p,visit_file,&w)) d->error=1;
    } else if (kind==1) add_file(d,p);
    else d->error=1;
    if (d->error) snprintf(d->err,d->cap,"defines: パス %s を読めません",p);
    free(p);
}
static int path_cmp(const void *a,const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }

/* Byte-oriented comment removal. A Shift_JIS lead byte consumes its trail
   byte before interpreting slash, star or backslash, including in comments. */
static char *clean_source(const char *s,size_t n,unsigned **lines)
{
    char *out=sim_xmalloc(n+1);
    size_t i=0,k=0,ln=0,cap=64;
    unsigned physical=1;
    int block=0,line=0,quote=0;
    *lines=sim_xmalloc(cap*sizeof **lines); (*lines)[0]=1;
    while (i<n) {
        unsigned char c=(unsigned char)s[i];
        if ((c>=0x81 && c<=0x9F) || (c>=0xE0 && c<=0xFC)) {
            out[k++]=(block||line)?' ':s[i]; i++;
            if (i<n) { out[k++]=(block||line)?' ':s[i]; i++; }
            continue;
        }
        if (c=='\\' && i+1<n && (s[i+1]=='\n' || (s[i+1]=='\r' && i+2<n && s[i+2]=='\n'))) {
            i+=s[i+1]=='\r'?3:2; physical++; out[k++]=' '; continue;
        }
        if (c=='\n') {
            out[k++]='\n'; i++; physical++; line=0;
            if (++ln==cap) { cap*=2; *lines=sim_xrealloc(*lines,cap*sizeof **lines); }
            (*lines)[ln]=physical; continue;
        }
        if (block) { if (c=='*' && i+1<n && s[i+1]=='/') { block=0; i+=2; out[k++]=' '; } else { out[k++]=' '; i++; } continue; }
        if (line) { out[k++]=' '; i++; continue; }
        if (!quote && c=='/' && i+1<n && s[i+1]=='*') { block=1; i+=2; out[k++]=' '; continue; }
        if (!quote && c=='/' && i+1<n && s[i+1]=='/') { line=1; i+=2; out[k++]=' '; continue; }
        if (quote && c=='\\' && i+1<n) { out[k++]=s[i++]; out[k++]=s[i++]; continue; }
        if (c=='"' || c=='\'') { if (!quote) quote=c; else if (quote==c) quote=0; }
        out[k++]=s[i++];
    }
    out[k]=0; return out;
}
static char *normalized(const char *s)
{
    lexer l=lex(s); sim_buf b; sim_buf_init(&b);
    while (l.tok[0]) { sim_buf_adds(&b,l.tok); sim_buf_addc(&b,' '); next(&l); }
    return b.p ? b.p : sim_xstrdup("");
}
static uint32_t name_hash(const char *s)
{ uint32_t h=2166136261u; while (*s) { h^=(unsigned char)*s++; h*=16777619u; } return h; }
static definition *lookup(defs *d,int type,const char *name)
{
    size_t cap=type?d->type_cap:d->macro_cap;
    size_t *index=type?d->type_index:d->macro_index, slot;
    definition *list=type?d->types:d->macros;
    if (!cap) return NULL;
    slot=name_hash(name)&(cap-1);
    while (index[slot]) {
        if (!strcmp(list[index[slot]-1].name,name)) return &list[index[slot]-1];
        slot=(slot+1)&(cap-1);
    }
    return NULL;
}
static void index_definition(defs *d,int type)
{
    size_t **index=type?&d->type_index:&d->macro_index;
    size_t *cap=type?&d->type_cap:&d->macro_cap;
    size_t count=type?d->ntypes:d->nmacros,i,slot;
    definition *list=type?d->types:d->macros;
    if (count*2>=*cap) {
        *cap=*cap?*cap*2:128; free(*index); *index=sim_xcalloc(*cap,sizeof **index);
        for (i=0;i<count;i++) {
            slot=name_hash(list[i].name)&(*cap-1);
            while ((*index)[slot]) slot=(slot+1)&(*cap-1);
            (*index)[slot]=i+1;
        }
    } else {
        slot=name_hash(list[count-1].name)&(*cap-1);
        while ((*index)[slot]) slot=(slot+1)&(*cap-1);
        (*index)[slot]=count;
    }
}
static void add_definition(defs *d, int type, const char *name, const char *body,const char *file,unsigned line)
{
    definition **list=type?&d->types:&d->macros;
    size_t *count=type?&d->ntypes:&d->nmacros;
    definition *previous=lookup(d,type,name);
    if (previous) {
        char *norm=normalized(body), *old=normalized(previous->body);
        if (strcmp(old,norm)) {
            d->duplicates++;
            if (d->verbose) fprintf(stderr,"defines: 異なる重複 %s: %s:%u / %s:%u（defines_exclude で除外できます）\n",name,previous->file,previous->line,file,line);
        }
        free(old); free(norm); return;
    }
    *list=sim_xrealloc(*list,(*count+1)*sizeof **list);
    { definition *v=&(*list)[(*count)++]; memset(v,0,sizeof *v); v->name=sim_xstrdup(name); v->body=sim_xstrdup(body); v->file=sim_xstrdup(file); v->line=line; }
    index_definition(d,type);
}
static void read_source(defs *d,const char *file)
{
    size_t n, ln=0;
    char *raw=sim_read_file(file,&n), *s, *p;
    unsigned *lines;
    lexer l;
    if (!raw) { snprintf(d->err,d->cap,"defines: %s を読めません",file); d->error=1; return; }
    s=clean_source(raw,n,&lines); free(raw);
    for (p=s;*p;) {
        char *end=strchr(p,'\n'), *q=p;
        char saved=end?*end:0;
        if (end) *end=0;
        while (isspace((unsigned char)*q)) q++;
        if (*q=='#') {
            lexer m=lex(q+1);
            if (!strcmp(m.tok,"define")) {
                char name[128]; next(&m); snprintf(name,sizeof name,"%s",m.tok);
                if (sim_is_ident_start((unsigned char)name[0]) && *m.p!='(') add_definition(d,0,name,m.p,file,lines[ln]);
            }
            memset(p,' ',(size_t)((end?end:p+strlen(p))-p));
        }
        if (!end) break;
        *end=saved; p=end+1; ln++;
    }
    l=lex(s);
    while (l.tok[0]) {
        int td=!strcmp(l.tok,"typedef"), tagged=!strcmp(l.tok,"struct") || !strcmp(l.tok,"union");
        if (td || tagged) {
            const char *begin=l.start, *body, *last=NULL;
            char last_name[128]="", tag[128]="";
            int depth=0, braces=0;
            if (td) next(&l);
            body=l.start;
            if (!strcmp(l.tok,"struct") || !strcmp(l.tok,"union")) {
                lexer t=l; next(&t);
                if (sim_is_ident_start((unsigned char)t.tok[0])) snprintf(tag,sizeof tag,"%s",t.tok);
            }
            while (l.tok[0]) {
                if (!strcmp(l.tok,"{")) { depth++; braces=1; }
                if (!strcmp(l.tok,"}")) depth--;
                if (!depth && !strcmp(l.tok,";")) break;
                if (!depth && sim_is_ident_start((unsigned char)l.tok[0])) { last=l.start; snprintf(last_name,sizeof last_name,"%s",l.tok); }
                next(&l);
            }
            if (l.tok[0] && (td || braces)) {
                if (td && last) { char *text=sim_xstrndup(body,(size_t)(last-body)); add_definition(d,1,last_name,text,file,1); free(text); }
                if (braces && tag[0]) { char *text=sim_xstrndup(body,(size_t)((td && last?last:l.start)-body)); add_definition(d,1,tag,text,file,1); free(text); }
            }
            if (l.start==begin) next(&l);
        }
        if (l.tok[0]) next(&l);
    }
    free(lines); free(s);
}

static const sim_type basic[] = {
    {SIM_T_UINT,"u8",1,NULL,0,NULL,0},{SIM_T_INT,"s8",1,NULL,0,NULL,0},
    {SIM_T_UINT,"u16",2,NULL,0,NULL,0},{SIM_T_INT,"s16",2,NULL,0,NULL,0},
    {SIM_T_UINT,"u32",4,NULL,0,NULL,0},{SIM_T_INT,"s32",4,NULL,0,NULL,0},
    {SIM_T_FLOAT,"f32",4,NULL,0,NULL,0}
};
static const sim_type *resolve_name(defs *,const char *,unsigned);
static const sim_type *parse_type(defs *,lexer *,unsigned);
static uint64_t expression(defs *,lexer *,unsigned,int *);
static uint64_t atom(defs *d,lexer *l,unsigned depth,int *ok)
{
    uint64_t v=0;
    size_t n;
    if (depth>16) { *ok=0; return 0; }
    if (!strcmp(l->tok,"(")) { next(l); v=expression(d,l,depth+1,ok); if (strcmp(l->tok,")")) *ok=0; else next(l); return v; }
    if (!strcmp(l->tok,"+") || !strcmp(l->tok,"-")) { int neg=l->tok[0]=='-'; next(l); v=atom(d,l,depth+1,ok); return neg?0-v:v; }
    n=strlen(l->tok);
    if (isdigit((unsigned char)l->tok[0])) {
        while (n && (l->tok[n-1]=='U' || l->tok[n-1]=='u' || l->tok[n-1]=='L' || l->tok[n-1]=='l')) n--;
        if (sim_parse_uint_n(l->tok,n,UINT64_MAX,&v)) *ok=0;
        next(l); return v;
    }
    { definition *macro=lookup(d,0,l->tok); if (macro) {
        lexer m=lex(macro->body); v=expression(d,&m,depth+1,ok); if (m.tok[0]) *ok=0; next(l); return v;
    } }
    *ok=0; if (l->tok[0]) next(l); return 0;
}
static uint64_t sum_expr(defs *d,lexer *l,unsigned depth,int *ok)
{
    uint64_t v=atom(d,l,depth,ok);
    while (!strcmp(l->tok,"+") || !strcmp(l->tok,"-")) { int add=l->tok[0]=='+'; uint64_t r; next(l); r=atom(d,l,depth,ok); if (add ? v>UINT64_MAX-r : v<r) *ok=0; v=add?v+r:v-r; }
    return v;
}
static uint64_t shift_expr(defs *d,lexer *l,unsigned depth,int *ok)
{
    uint64_t v=sum_expr(d,l,depth,ok);
    while (!strcmp(l->tok,"<<")) { uint64_t r; next(l); r=sum_expr(d,l,depth,ok); if (r>=64 || v>(UINT64_MAX>>r)) *ok=0; else v<<=r; }
    return v;
}
static uint64_t expression(defs *d,lexer *l,unsigned depth,int *ok)
{
    uint64_t v=shift_expr(d,l,depth,ok);
    while (!strcmp(l->tok,"|")) { next(l); v|=shift_expr(d,l,depth,ok); }
    return v;
}
static unsigned align_up(unsigned n,unsigned a) { return (n+a-1)/a*a; }
static unsigned type_alignment(const sim_type *t,unsigned depth)
{
    unsigned a=1; int i;
    if (!t || depth>32) return 1;
    if (t->kind==SIM_T_ARRAY) return type_alignment(t->target,depth+1);
    if (t->kind!=SIM_T_STRUCT && t->kind!=SIM_T_UNION) return t->size==1?1:2;
    for (i=0;i<t->nmembers;i++) if (type_alignment(t->members[i].type,depth+1)>a) a=2;
    return a;
}
static const sim_type *record_type(defs *d,lexer *l,int un,unsigned depth)
{
    sim_type *t=sim_ctx_alloc(d->ctx,sizeof *t);
    sim_member *members=NULL;
    size_t nm=0;
    unsigned size=0,alignment=1,unit=0,used=0;
    const sim_type *unit_type=NULL;
    if (depth>32) return NULL;
    next(l); /* { */
    while (l->tok[0] && strcmp(l->tok,"}")) {
        const sim_type *mt=parse_type(d,l,depth+1);
        if (!mt) { free(members); return NULL; }
        do {
            char name[128]="";
            const sim_type *field=mt;
            unsigned count=0, width=0,off=0;
            int bit=0,ok=1;
            if (sim_is_ident_start((unsigned char)l->tok[0])) { snprintf(name,sizeof name,"%s",l->tok); next(l); }
            if (!strcmp(l->tok,"[")) {
                uint64_t n; next(l); n=expression(d,l,0,&ok);
                if (!ok || !n || (!mt->size || n>UINT32_MAX/mt->size) || strcmp(l->tok,"]")) { free(members); return NULL; }
                count=(unsigned)n; next(l);
                { sim_type *a=sim_ctx_alloc(d->ctx,sizeof *a); a->kind=SIM_T_ARRAY; a->target=mt; a->count=count; a->size=count*mt->size; a->name=mt->name; field=a; }
            }
            if (!strcmp(l->tok,":")) {
                uint64_t n; bit=1; next(l); n=expression(d,l,0,&ok);
                if (!ok || n>mt->size*8 || mt->kind==SIM_T_FLOAT || count) { free(members); return NULL; }
                width=(unsigned)n;
            }
            { unsigned a=type_alignment(field,0); if (a>alignment) alignment=a;
              if (bit) {
                  if (!width) { unit_type=NULL; used=0; }
                  else if (un) { off=0; unit=0; used=0; if (size<mt->size) size=mt->size; }
                  else if (unit_type!=mt || used+width>mt->size*8) { unit=align_up(size,a); size=unit+mt->size; used=0; unit_type=mt; }
                  off=unit;
              } else { unit_type=NULL; used=0; off=un?0:align_up(size,a); if (size<off+field->size) size=off+field->size; }
            }
            if (!bit || width) {
                members=sim_xrealloc(members,(nm+1)*sizeof *members);
                members[nm].name=owned_string(d,name); members[nm].type=field; members[nm].offset=off;
                members[nm].bit_offset=bit?(int32_t)(off*8+used):-1; members[nm].bit_size=width; nm++;
                if (bit) used+=width;
            }
            if (strcmp(l->tok,",")) break;
            next(l);
        } while (l->tok[0]);
        if (strcmp(l->tok,";")) { free(members); return NULL; }
        next(l);
    }
    if (strcmp(l->tok,"}")) { free(members); return NULL; }
    next(l);
    t->kind=un?SIM_T_UNION:SIM_T_STRUCT; t->name=un?"union":"struct"; t->size=align_up(size,alignment);
    t->members=sim_ctx_alloc(d->ctx,nm*sizeof *members); memcpy((void *)t->members,members,nm*sizeof *members); t->nmembers=(int)nm;
    free(members); return t;
}
static const sim_type *parse_type(defs *d,lexer *l,unsigned depth)
{
    const sim_type *t;
    while (qualifier(l->tok)) next(l);
    if (!strcmp(l->tok,"struct") || !strcmp(l->tok,"union")) {
        int un=!strcmp(l->tok,"union"); char tag[128]=""; next(l);
        if (sim_is_ident_start((unsigned char)l->tok[0])) { snprintf(tag,sizeof tag,"%s",l->tok); next(l); }
        if (!strcmp(l->tok,"{")) return record_type(d,l,un,depth);
        return tag[0]?resolve_name(d,tag,depth+1):NULL;
    }
    {
        int uns=0,sgn=0,chr=0,lng=0,flt=0,seen=0;
        for (;;) {
            if (qualifier(l->tok)) { next(l); continue; }
            if (!strcmp(l->tok,"unsigned")) uns=seen=1;
            else if (!strcmp(l->tok,"signed")) sgn=seen=1;
            else if (!strcmp(l->tok,"char")) chr=seen=1;
            else if (!strcmp(l->tok,"short") || !strcmp(l->tok,"int")) seen=1;
            else if (!strcmp(l->tok,"long")) lng=seen=1;
            else if (!strcmp(l->tok,"float") || !strcmp(l->tok,"double")) flt=seen=1;
            else break;
            next(l);
        }
        if (seen) return &basic[flt?6:chr?(uns || (!sgn && d->char_unsigned)?0:1):lng?(uns?4:5):(uns?2:3)];
    }
    t=resolve_name(d,l->tok,depth+1); if (l->tok[0]) next(l);
    while (qualifier(l->tok)) next(l);
    return t;
}
static const sim_type *resolve_name(defs *d,const char *name,unsigned depth)
{
    size_t i;
    static const char *stdnames[]={"uint8_t","int8_t","uint16_t","int16_t","uint32_t","int32_t",NULL};
    if (depth>32) return NULL;
    for (i=0;i<7;i++) if (!strcmp(name,basic[i].name)) return &basic[i];
    for (i=0;stdnames[i];i++) if (!strcmp(name,stdnames[i])) return &basic[i];
    { definition *v=lookup(d,1,name); if (v) {
        if (v->state==2) return v->type;
        if (v->state==1) return NULL;
        if (!v->state) {
        v->state=1;
        { lexer l=lex(v->body); const sim_type *t=parse_type(d,&l,depth+1); v->type=(!l.tok[0] || !strcmp(l.tok,";"))?t:NULL; }
        v->state=v->type?2:3;
        if (v->type) return v->type;
        }
    } }
    { const sim_type *t=sim_ctx_named_type(d->ctx,name); if (t) return t; }
    for (i=0;i<(size_t)d->cfg.n;i++) if (!strcmp(d->cfg.e[i].key,"ctype")) {
        lexer l=lex(d->cfg.e[i].value);
        if (strcmp(l.tok,name)) continue;
        next(&l); if (strcmp(l.tok,":")) return NULL; next(&l);
        if (!strcmp(l.tok,"bits8") || !strcmp(l.tok,"bits16")) {
            sim_type *t=sim_ctx_alloc(d->ctx,sizeof *t);
            t->kind=SIM_T_STRUCT; t->name=!strcmp(l.tok,"bits8")?"bits8":"bits16"; t->size=!strcmp(l.tok,"bits8")?1:2; return t;
        }
        return parse_type(d,&l,depth+1);
    }
    return NULL;
}
static int bit_number(const char *s,unsigned limit)
{
    const char *p=s; uint64_t n;
    if (!isalpha((unsigned char)*p)) return -1;
    while (isalpha((unsigned char)*p)) p++;
    if (sim_parse_uint(p,limit-1,&n)) return -1;
    return (int)n;
}
static const sim_member *member(const sim_type *t,const char *name,unsigned depth,uint32_t *offset)
{
    int k;
    if (!t || depth>16) return NULL;
    for (k=0;k<t->nmembers;k++) if (!strcmp(t->members[k].name,name)) { *offset=0; return &t->members[k]; }
    for (k=0;k<t->nmembers;k++) if (!t->members[k].name[0]) {
        uint32_t off; const sim_member *m=member(t->members[k].type,name,depth+1,&off);
        if (m) { *offset=off+t->members[k].offset; return m; }
    }
    return NULL;
}
static void resolve_macro(defs *d,definition *v)
{
    lexer l=lex(v->body), after={0};
    const sim_type *t=NULL;
    uint64_t addr;
    int deref=0,ok=1,bit=-1;
    unsigned bits=0;
    /* A cast must have one pointer star immediately before its closing ). */
    while (l.tok[0]) {
        if (!strcmp(l.tok,"*")) deref=1;
        if (!strcmp(l.tok,"(")) {
            lexer cast=l; next(&cast);
            t=parse_type(d,&cast,0);
            if (!strcmp(cast.tok,"*")) {
                next(&cast); while (qualifier(cast.tok)) next(&cast);
                if (!strcmp(cast.tok,")")) { next(&cast); after=cast; break; }
            }
        }
        next(&l);
    }
    if (!after.p) return; /* Ordinary constants/function macros are not lvalues. */
    l=after; addr=expression(d,&l,0,&ok);
    if (!ok) goto unresolved;
    while (!strcmp(l.tok,")")) next(&l);
    if (!deref && strcmp(l.tok,"->") && strcmp(l.tok,".")) { d->pointers++; return; }
    if (addr>0xFFFFF) { snprintf(d->err,d->cap,"defines: %s:%u %s の番地が20ビット範囲外です",v->file,v->line,v->name); d->error=1; return; }
    if (addr<=0xFFFF && d->near_base) { addr+=d->near_base; if (d->verbose) fprintf(stderr,"defines: %s near -> 0x%05llX\n",v->name,(unsigned long long)addr); }
    while (!strcmp(l.tok,"->") || !strcmp(l.tok,".")) {
        const sim_member *m; uint32_t parent=0;
        next(&l);
        if (!t || (t->name && (!strcmp(t->name,"bits8") || !strcmp(t->name,"bits16")))) {
            int guessed=t==NULL;
            int b=bit_number(l.tok,t?t->size*8:8);
            if (b<0 || (guessed && !d->guess)) goto unresolved;
            bit=b; bits=1; t=&basic[t && t->size==2?2:0];
            if (guessed) { d->guessed++; if (d->verbose) fprintf(stderr,"defines: %s をビット%dと推定\n",v->name,b); }
        } else {
            m=member(t,l.tok,0,&parent); if (!m) goto unresolved;
            addr+=parent+m->offset;
            if (m->bit_offset>=0) { bit=m->bit_offset-(int)(m->offset*8); bits=m->bit_size; }
            t=m->type;
        }
        next(&l); while (!strcmp(l.tok,")")) next(&l);
    }
    if (l.tok[0] || !t || !t->size) goto unresolved;
    if (addr>0xFFFFF || t->size>0x100000-addr) { snprintf(d->err,d->cap,"defines: %s:%u %s の番地またはサイズが20ビット範囲外です",v->file,v->line,v->name); d->error=1; return; }
    {
        int added=sim_ctx_define(d->ctx,v->name,t,addr,bit,bits,v->file,0);
        if (!added) { if (d->verbose) fprintf(stderr,"defines: %s はロードモジュールの同名シンボルを優先\n",v->name); return; }
    }
    if (bits) d->bits++; else if (t->kind==SIM_T_ARRAY) d->arrays++; else d->variables++;
    return;
unresolved:
    d->unresolved++;
    if (d->verbose) fprintf(stderr,"defines: 型または番地を解決できない定義 %s (%s:%u)\n",v->name,v->file,v->line);
}
static void free_definitions(definition *v,size_t n)
{ size_t i; for (i=0;i<n;i++) { free(v[i].name); free(v[i].body); free(v[i].file); } free(v); }

static int explicit_var(defs *d,const char *text,const char *file,unsigned line)
{
    lexer l=lex(text);
    char name[128];
    const sim_type *t;
    uint64_t addr,n;
    int bit=-1; unsigned bits=0;
    snprintf(name,sizeof name,"%s",l.tok);
    if (!sim_is_ident_start((unsigned char)name[0])) goto bad;
    next(&l); if (strcmp(l.tok,":")) goto bad;
    next(&l); t=parse_type(d,&l,0);
    if (!t || !t->size) goto bad;
    if (!strcmp(l.tok,"[")) {
        sim_type *a;
        next(&l); if (sim_parse_uint(l.tok,0x100000/t->size,&n) || !n) goto bad;
        next(&l); if (strcmp(l.tok,"]")) goto bad; next(&l);
        a=sim_ctx_alloc(d->ctx,sizeof *a); a->kind=SIM_T_ARRAY; a->name=t->name;
        a->target=t; a->count=(uint32_t)n; a->size=(uint32_t)n*t->size; t=a;
    } else if (!strcmp(l.tok,".")) {
        uint64_t end;
        if (t->kind!=SIM_T_UINT && t->kind!=SIM_T_INT) goto bad;
        next(&l); if (sim_parse_uint(l.tok,t->size*8-1,&n)) goto bad;
        bit=(int)n; end=n; next(&l);
        if (!strcmp(l.tok,"-")) { next(&l); if (sim_parse_uint(l.tok,t->size*8-1,&end) || end<n) goto bad; next(&l); }
        bits=(unsigned)(end-n+1);
    }
    if (strcmp(l.tok,"@")) goto bad;
    next(&l); if (sim_parse_uint(l.tok,0xFFFFF,&addr) || t->size>0x100000-addr) goto bad;
    next(&l); if (l.tok[0]) goto bad;
    {
        int added=sim_ctx_define(d->ctx,name,t,addr,bit,bits,file,2);
        if (d->verbose && added==2) fprintf(stderr,"var: %s の同名シンボルを明示定義で上書き\n",name);
    }
    d->explicit_vars++; return 0;
bad:
    snprintf(d->err,d->cap,"%s:%u: var は 名前 : 型 @ 20ビット番地 の形で指定してください（型・配列長・ビット範囲も確認してください）",file,line);
    d->error=1; return -1;
}
static void var_file(defs *d,const char *value)
{
    char *path=sim_cfg_path(&d->cfg,value),*data,*p;
    size_t n; unsigned line=1;
    data=sim_read_file(path,&n);
    if (!data || memchr(data,0,n)) { snprintf(d->err,d->cap,"varfile %s を読めません（NULを含まないテキストが必要）",path); d->error=1; free(path); free(data); return; }
    p=data;
    if (n>=3 && (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p+=3;
    while (*p && !d->error) {
        char *end=strchr(p,'\n'),*hash;
        if (end) *end=0;
        hash=strchr(p,'#'); if (hash) *hash=0;
        while (isspace((unsigned char)*p)) p++;
        if (*p) explicit_var(d,p,path,line);
        if (!end) break;
        p=end+1; line++;
    }
    free(path); free(data);
}

int sim_defines_load(sim_ctx *c,const sim_open_opts *o,char *err,size_t cap)
{
    defs d; char *path; int rc,i; size_t k; uint64_t t0=sim_now_ns(); sim_buf info;
    memset(&d,0,sizeof d); d.ctx=c; d.err=err; d.cap=cap; d.verbose=o->verbose;
    sim_cfg_init(&d.cfg);
    if (o->cfg_path) path=sim_xstrdup(o->cfg_path);
    else if (o->abs_path) { const char *b=sim_basename(o->abs_path); char *dir=sim_xstrndup(o->abs_path,(size_t)(b-o->abs_path)); path=sim_path_join(dir[0]?dir:".","sim.cfg"); free(dir); }
    else path=sim_xstrdup("sim.cfg");
    rc=sim_cfg_load(&d.cfg,path,err,cap); free(path);
    if (rc==-2 || (rc==-1 && o->cfg_path)) { if (rc==-1) snprintf(err,cap,"設定ファイルを読めません"); sim_cfg_free(&d.cfg); return -1; }
    d.recursive=onoff(&d,"defines_recursive",1); d.guess=onoff(&d,"defines_guess_bits",1);
    d.char_unsigned=onoff(&d,"char_unsigned",0); d.near_base=0xF0000;
    { const char *s=sim_cfg_get(&d.cfg,"defines_near_base"); if (s && (sim_parse_uint(s,0xF0000,&d.near_base) || (d.near_base && d.near_base!=0xF0000))) { snprintf(err,cap,"defines_near_base は 0xF0000 / 0 です"); d.error=1; } }
    for (i=0;i<d.cfg.n && !d.error;i++) if (!strcmp(d.cfg.e[i].key,"defines")) collect_path(&d,d.cfg.e[i].value);
    for (i=0;i<o->ndefines && !d.error;i++) collect_path(&d,o->defines[i]);
    qsort(d.files,d.nfiles,sizeof *d.files,path_cmp);
    for (k=0;k<d.nfiles && !d.error;k++) read_source(&d,d.files[k]);
    for (k=0;k<d.nmacros && !d.error;k++) resolve_macro(&d,&d.macros[k]);
    for (i=0;i<d.cfg.n && !d.error;i++) {
        if (!strcmp(d.cfg.e[i].key,"var")) explicit_var(&d,d.cfg.e[i].value,d.cfg.path?d.cfg.path:"sim.cfg",(unsigned)d.cfg.e[i].line);
        if (!strcmp(d.cfg.e[i].key,"varfile")) var_file(&d,d.cfg.e[i].value);
    }
    sim_ctx_refresh_symbols(c);
    sim_buf_init(&info);
    if (d.nfiles || d.explicit_vars) {
        sim_buf_printf(&info,"defines files=%zu variables=%u bits=%u arrays=%u guessed=%u unresolved=%u duplicates=%u pointer-skipped=%u (%.3f s)\n",d.nfiles,d.variables,d.bits,d.arrays,d.guessed,d.unresolved,d.duplicates,d.pointers,(double)(sim_now_ns()-t0)/1e9);
        sim_buf_printf(&info,"型を解決できなかった定義: %u 件。同じ名前で内容の違う定義: %u 件（defines_exclude で除外できます）\n",d.unresolved,d.duplicates);
        sim_buf_printf(&info,"var definitions=%u\n",d.explicit_vars);
        sim_ctx_set_definition_info(c,sim_buf_cstr(&info));
        if (d.verbose) fputs(sim_buf_cstr(&info),stderr);
    }
    sim_buf_free(&info);
    for (k=0;k<d.nfiles;k++) free(d.files[k]);
    free(d.files);
    free_definitions(d.macros,d.nmacros); free_definitions(d.types,d.ntypes); sim_cfg_free(&d.cfg);
    free(d.macro_index); free(d.type_index);
    return d.error?-1:0;
}
