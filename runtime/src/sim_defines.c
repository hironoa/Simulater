#define _CRT_SECURE_NO_WARNINGS
#include "sim_defines.h"
#include "sim_cfg.h"
#include "sim_mtpj.h"
#include "sim_ppexpr.h"
#include "sim_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name, *body, *file; unsigned line; int state; const sim_type *type; } definition;
typedef struct { char *key; const char *name, *file; unsigned count, line; } missing_include;
/* 条件付きコンパイルの指令（#…）。ファイルを読んだときに 1 回だけ切り出す */
enum { D_IF = 1, D_IFDEF, D_IFNDEF, D_ELIF, D_ELSE, D_ENDIF, D_DEFINE, D_UNDEF, D_INCLUDE };
typedef struct {
    unsigned char kind, funclike;
    unsigned char evaluated, indeterminate; /* 評価したことがある／判定不能だったことがある */
    unsigned char logged;                   /* 未定義の識別子を数え済み */
    unsigned char missing_counted;          /* 有効な枝の見つからない #include を数え済み */
    uint32_t line;      /* 論理行の番号 */
    uint32_t sym;       /* #define / #undef / #ifdef / #ifndef の名前 */
    uint32_t guard;     /* 条件の直後の #define の名前（インクルードガード）。なければ 0 */
    uint32_t next;      /* 条件の系列の次の枝（#elif / #else / #endif）。なければ指令の数 */
    int32_t target;     /* #include: 解決したファイルの番号。-1 = 走査した中にない、-2 = 未解決 */
    const char *arg;    /* #if / #elif の式、#define の本体、#include のファイル名 */
} directive;
typedef struct {
    char *path;         /* defs.files の要素を借りる */
    char *lower;        /* '/' 区切り・小文字のパス（#include の照合用） */
    char *text;         /* コメントを除いた内容。行ごとに NUL で終端 */
    size_t len;
    unsigned bad_line; /* first lexical/structural failure, physical line */
    uint32_t *line_off; /* 論理行の先頭の位置 */
    unsigned *line_no;  /* 論理行の、元のファイルでの行番号 */
    size_t nlines;
    directive *dirs;
    size_t ndirs;
    uint64_t *seen;     /* 収集済みの「枝の選び方」 */
    size_t nseen;
    struct memo *memos; /* 読んだ結果の記録（同じ状態で #include されたら読み直さない） */
    unsigned nmemos;
    unsigned char included, visited, has_types;
    unsigned char closure;  /* defines_scope = mtpj で、#include をたどって足したファイル（起点にしない） */
    unsigned char packing;  /* 配置指定（#pragma pack / __packed / __attribute__）がある */
} source;
/* マクロの名前。状態は起点ごと（gen が今の世代なら kind / body、違えば定義マクロの base_*） */
typedef struct {
    char *name;
    const char *base_body, *body;
    uint32_t hash, gen;
    uint32_t wtick;     /* いまの起点で最後に #define / #undef した時刻（gen が今の世代のとき） */
    uint32_t mark;
    unsigned char base_kind, kind, origin, defined_somewhere;
    unsigned undef_refs, hidden_refs;
} psym;
typedef struct { int32_t take; int all; } cond_frame;
typedef struct { char *name; const sim_type *type; } ext_type;
typedef struct { const sim_type *base, *qualified; } const_type_entry;
/* マクロの状態の読み書きの記録。あるファイルを読んだ結果は「読む前の状態のうち実際に参照したもの（reads）」
   だけで決まるので、同じ reads の状態で再び #include されたら、書き込み（writes）を再生するだけで済ませる */
typedef struct { uint32_t sym, wtick; const char *body; unsigned char kind; } pp_read;
typedef struct { uint32_t sym; unsigned char kind; const char *body; } pp_write;
typedef struct memo {
    pp_read *reads; size_t nreads;
    pp_write *writes; size_t nwrites;
    unsigned includes, height;      /* 中で展開した #include の数と、入れ子の深さ */
} memo;
typedef struct {
    sim_ctx *ctx;
    sim_cfg cfg;
    char **files;
    size_t nfiles;
    definition *macros, *types, *tags;
    size_t ntags, tags_alloc, *tag_index, tag_cap;
    size_t nmacros, ntypes, macros_alloc, types_alloc;
    size_t *macro_index, *type_index, macro_cap, type_cap;
    int recursive, guess, char_unsigned, verbose, error, cond, compiler, packing;
    const_type_entry *const_types; size_t nconst_types;
    uint64_t near_base;
    unsigned variables, bits, arrays, guessed, unresolved, duplicates, pointers, values, explicit_vars;
    unsigned dup_same_file, dup_same_name, dup_other_name;
    missing_include *missing; size_t nmissing, missing_alloc;
    char *err; size_t cap;
    /* 条件付きコンパイル */
    source *src; size_t nsrc;
    uint32_t *by_base;              /* ファイル名順の番号（#include の検索用） */
    psym *syms; size_t nsyms, syms_alloc;
    uint32_t *sym_index; size_t sym_cap;
    uint32_t gen;
    uint32_t *ranges; size_t nranges, cap_ranges;       /* 読まない行の範囲（開始, 終了）の列 */
    cond_frame *frames; size_t nframes, cap_frames;
    sim_pp_env env;
    directive *site; int log, logged_any;
    char **owned; size_t nowned;
    ext_type *ext; size_t ext_cap, ext_count;   /* ヘッダにない型名の解決結果 */
    uint32_t tick, serial;
    pp_read *rlog; size_t nrlog, cap_rlog;
    pp_write *wlog; size_t nwlog, cap_wlog;
    size_t memo_cells;
    unsigned root_includes, memo_hits;
    unsigned roots, includes, include_missing, include_cut, if_evaluated, if_indeterminate, undefined, collections;
    unsigned enc_ascii, enc_utf8, enc_sjis, enc_utf16;
    size_t bytes, ndirectives;
    uint64_t t_scan, t_read, t_cond;
    /* defines_scope = mtpj: 読むのは登録ファイルと、そこから #include でたどれるファイルだけ */
    int scope;
    unsigned char *file_closure;    /* files と同じ並び。#include をたどって足したものは 1 */
    size_t files_alloc;
    char **incdirs; size_t nincdirs;    /* インクルード・パス（正規化済み）。前から順に探す */
    char **pkeys; int32_t *pvals; size_t pcap, pcount;  /* 正規化して小文字にしたパス -> files の番号（-1 = ない） */
    unsigned scope_registered, scope_added, scope_fallback;
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
    } else if (*s=='"' || *s=='\'') {
        char quote=*l->p++;
        while (*l->p && *l->p!=quote) {
            if (*l->p=='\\' && l->p[1]) l->p++;
            l->p++;
        }
        if (*l->p) l->p++;
    } else if (*s) {
        static const char *const pairs[]={"->","<<",">>","<=",">=","==","!=","&&","||","++","--","##"};
        size_t i;
        l->p++;
        for (i=0;i<sizeof pairs/sizeof pairs[0];i++) if (!strncmp(s,pairs[i],2)) { l->p++; break; }
    }
    n = (size_t)(l->p - s); if (n >= sizeof l->tok) n = sizeof l->tok - 1;
    memcpy(l->tok, s, n); l->tok[n] = 0;
}
static lexer lex(const char *s) { lexer l; memset(&l, 0, sizeof l); l.p = s; next(&l); return l; }
static char *owned_string(defs *d, const char *s)
{ char *p = sim_ctx_alloc(d->ctx, strlen(s)+1); strcpy(p,s); return p; }
static int qualifier(const char *s)
{ return !strcmp(s,"volatile") || !strcmp(s,"const") || !strcmp(s,"__near") || !strcmp(s,"__far") || !strcmp(s,"__sreg") || !strcmp(s,"__saddr"); }
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
/* 区切りを '/' にし、"." と ".." を畳んだパス（同じファイルを別の書き方で 2 回読まないため） */
static char *normal_path(const char *p)
{
    size_t n=strlen(p), i=0, o=0, root, lead=0;
    char *s=sim_xmalloc(n+3);
    if (n>=2 && p[1]==':') { s[o++]=p[0]; s[o++]=':'; i=2; }
    while (p[i]=='/' || p[i]=='\\') { if (lead<2 && (lead==0 || i==1)) { s[o++]='/'; lead++; } i++; }
    root=o;
    while (p[i]) {
        size_t b=i, len;
        while (p[i] && p[i]!='/' && p[i]!='\\') i++;
        len=i-b;
        while (p[i]=='/' || p[i]=='\\') i++;
        if (len==1 && p[b]=='.') continue;
        if (len==2 && p[b]=='.' && p[b+1]=='.' && o>root) {
            size_t q=o;
            while (q>root && s[q-1]!='/') q--;      /* 最後の要素の先頭 */
            if (!(o-q==2 && s[q]=='.' && s[q+1]=='.')) { o=q>root ? q-1 : root; continue; }
        }
        if (len==2 && p[b]=='.' && p[b+1]=='.' && o==root && lead) continue;   /* ルートより上はない */
        if (o>root) s[o++]='/';
        memcpy(s+o,p+b,len); o+=len;
    }
    if (!o) s[o++]='.';
    s[o]=0;
    return s;
}
static int absolute_path(const char *p) { return p[0]=='/' || p[0]=='\\' || (p[0] && p[1]==':'); }
static void push_file(defs *d, char *owned_path, int closure)
{
    if (d->nfiles==d->files_alloc) {
        d->files_alloc=d->files_alloc?d->files_alloc*2:256;
        d->files=sim_xrealloc(d->files,d->files_alloc*sizeof *d->files);
        d->file_closure=sim_xrealloc(d->file_closure,d->files_alloc);
    }
    d->file_closure[d->nfiles]=(unsigned char)closure;
    d->files[d->nfiles++]=owned_path;
}
static void add_file(defs *d, const char *path)
{
    size_t i;
    char *p;
    if (excluded(d,path)) return;
    p=d->scope ? normal_path(path) : sim_xstrdup(path);
    for (i=0;i<d->nfiles;i++) if (!strcmp(d->files[i],p)) { free(p); return; }
    push_file(d,p,0);
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

/* Detect each complete file independently. Reject overlong UTF-8, surrogates,
   truncated sequences and code points beyond U+10FFFF. */
enum { ENC_ASCII, ENC_UTF8, ENC_SJIS, ENC_UTF16 };
static int source_encoding(const unsigned char *s,size_t n)
{
    size_t i=0,zeros=0;
    int multi=0;
    if (n>=2 && ((s[0]==0xFF && s[1]==0xFE) || (s[0]==0xFE && s[1]==0xFF))) return ENC_UTF16;
    if (n>=3 && s[0]==0xEF && s[1]==0xBB && s[2]==0xBF) return ENC_UTF8;
    for (i=0;i<n;i++) if (!s[i]) zeros++;
    if (zeros && zeros>=n/4+(n%4!=0)) return ENC_UTF16;
    for (i=0;i<n;) {
        unsigned c=s[i++],cp,min;
        size_t more,j;
        if (c<0x80) continue;
        multi=1;
        if (c>=0xC2 && c<=0xDF) { more=1; cp=c&31; min=0x80; }
        else if (c>=0xE0 && c<=0xEF) { more=2; cp=c&15; min=0x800; }
        else if (c>=0xF0 && c<=0xF4) { more=3; cp=c&7; min=0x10000; }
        else return ENC_SJIS;
        if (more>n-i) return ENC_SJIS;
        for (j=0;j<more;j++) {
            c=s[i++]; if (c<0x80 || c>0xBF) return ENC_SJIS;
            cp=(cp<<6)|(c&63);
        }
        if (cp<min || cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF)) return ENC_SJIS;
    }
    return multi?ENC_UTF8:ENC_ASCII;
}
static int sjis_pair(const char *s,size_t n,size_t i,int enc)
{
    unsigned c=(unsigned char)s[i],t;
    if (enc!=ENC_SJIS || i+1>=n) return 0;
    t=(unsigned char)s[i+1];
    return ((c>=0x81 && c<=0x9F) || (c>=0xE0 && c<=0xFC)) &&
        ((t>=0x40 && t<=0x7E) || (t>=0x80 && t<=0xFC));
}
static void bad_source(source *s,unsigned line)
{
    if (!s->bad_line || line<s->bad_line) s->bad_line=line;
}
/* Splicing precedes comment removal. Mask Shift_JIS pairs outside comments
   too, so subsequent lexers never see a trail byte as an escape/operator. */
static char *clean_source(const char *s,size_t n,unsigned **lines,size_t *outlen,int enc,source *src)
{
    char *out=sim_xmalloc(n+1);
    size_t i=0,k=0,ln=0,cap=64;
    unsigned physical=1,block_line=0,quote_line=0;
    int block=0,line=0,quote=0;
    *lines=sim_xmalloc(cap*sizeof **lines); (*lines)[0]=1;
    if (n>=3 && enc==ENC_UTF8 && !memcmp(s,"\xEF\xBB\xBF",3)) i=3;
    while (i<n) {
        unsigned char c=(unsigned char)s[i];
        if (sjis_pair(s,n,i,enc)) {
            out[k++]=(block||line)?' ':(char)0x80;
            out[k++]=(block||line)?' ':(char)0x80;
            i+=2; continue;
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
        if (!quote && c=='/' && i+1<n && s[i+1]=='*') { block=1; block_line=physical; i+=2; out[k++]=' '; continue; }
        if (!quote && c=='/' && i+1<n && s[i+1]=='/') { line=1; i+=2; out[k++]=' '; continue; }
        if (quote && c=='\\' && i+1<n && s[i+1]!='\r' && s[i+1]!='\n') { out[k++]=s[i++]; out[k++]=s[i++]; continue; }
        if (c=='"' || c=='\'') { if (!quote) { quote=c; quote_line=physical; } else if (quote==c) quote=0; }
        out[k++]=s[i++];
    }
    if (block) bad_source(src,block_line);
    if (quote) bad_source(src,quote_line);
    for (i=0;i<k;i++) if (!out[i]) out[i]=' ';
    out[k]=0; *outlen=k; return out;
}
/* Diagnose unmatched delimiters without interpreting strings as C syntax.
   This is a lexical warning, not a replacement for the compiler's parser. */
static void check_delimiters(source *s)
{
    char *stack=sim_xmalloc(s->len+1);
    unsigned *at=sim_xmalloc((s->len+1)*sizeof *at);
    size_t i,top=0,line=0;
    int quote=0;
    for (i=0;i<s->len;i++) {
        char c=s->text[i];
        if (c=='\n') line++;
        if (quote && c=='\\' && i+1<s->len) { i++; continue; }
        if (c=='"' || c=='\'') { if (!quote) quote=c; else if (quote==c) quote=0; continue; }
        if (quote) continue;
        if (c=='(' || c=='{' || c=='[') { stack[top]=c; at[top++]=s->line_no[line]; }
        if (c==')' || c=='}' || c==']') {
            char want=c==')'?'(':c=='}'?'{':'[';
            if (!top || stack[top-1]!=want) bad_source(s,s->line_no[line]);
            else top--;
        }
    }
    if (top) bad_source(s,at[0]);
    free(stack); free(at);
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
    size_t cap=type==2?d->tag_cap:type?d->type_cap:d->macro_cap;
    size_t *index=type==2?d->tag_index:type?d->type_index:d->macro_index, slot;
    definition *list=type==2?d->tags:type?d->types:d->macros;
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
    size_t **index=type==2?&d->tag_index:type?&d->type_index:&d->macro_index;
    size_t *cap=type==2?&d->tag_cap:type?&d->type_cap:&d->macro_cap;
    size_t count=type==2?d->ntags:type?d->ntypes:d->nmacros,i,slot;
    definition *list=type==2?d->tags:type?d->types:d->macros;
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
/* 内容が違う重複だけを、定義元の関係で分類する */
static void count_duplicate(defs *d, const char *previous, const char *file)
{
    const char *a, *b;
    d->duplicates++;
    if (!strcmp(previous,file)) { d->dup_same_file++; return; }
    a=sim_basename(previous); b=sim_basename(file);
    while (*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) { a++; b++; }
    if (!*a && !*b) d->dup_same_name++;
    else d->dup_other_name++;
}
static void add_definition(defs *d, int type, const char *name, const char *body,const char *file,unsigned line)
{
    definition **list=type==2?&d->tags:type?&d->types:&d->macros;
    size_t *count=type==2?&d->ntags:type?&d->ntypes:&d->nmacros;
    definition *previous=lookup(d,type,name);
    if (previous && type==2) {
        lexer a=lex(previous->body), b=lex(body);
        while (qualifier(a.tok)) next(&a);
        while (qualifier(b.tok)) next(&b);
        if (strcmp(a.tok,b.tok)) { previous->state=3; count_duplicate(d,previous->file,file); return; }
        if (!strchr(body,'{')) return; /* 同じ種類の前方宣言 */
        if (!strchr(previous->body,'{') && previous->state!=3) {
            free(previous->body); previous->body=sim_xstrdup(body); return;
        }
    }
    if (previous) {
        char *norm=normalized(body), *old=normalized(previous->body);
        if (strcmp(old,norm)) {
            count_duplicate(d,previous->file,file);
            if (d->verbose) fprintf(stderr,"defines: 異なる重複 %s: %s:%u / %s:%u（defines_exclude で除外できます）\n",name,previous->file,previous->line,file,line);
        }
        free(old); free(norm); return;
    }
    /* 1 件ずつ伸ばさない（realloc が毎回コピーする処理系では、定義が数万件あると遅くなる） */
    { size_t *alloc=type==2?&d->tags_alloc:type?&d->types_alloc:&d->macros_alloc;
      if (*count==*alloc) { *alloc=*alloc?*alloc*2:256; *list=sim_xrealloc(*list,*alloc*sizeof **list); } }
    { definition *v=&(*list)[(*count)++]; memset(v,0,sizeof *v); v->name=sim_xstrdup(name); v->body=sim_xstrdup(body); v->file=sim_xstrdup(file); v->line=line; }
    index_definition(d,type);
}
/* ================================================================ 条件付きコンパイル
 *
 * 走査した各ファイルを起点として上から順に読み、#if / #ifdef / #ifndef / #elif / #else / #endif を評価して
 * 有効な枝にある #define・typedef・struct だけを集める。#include は走査したファイルの中から探して
 * その場で読む。マクロの状態は起点ごとに、定義マクロ（cdef / mtpj / 暗黙）から始める。
 *
 * 速度のための工夫:
 *   - ファイルは 1 回だけ読み、コメントを除いて行と指令（#…）に分けておく。条件の系列は「次の枝」を
 *     指しておき、読まない枝は 1 回で飛び越す。
 *   - マクロの名前は番号にし、状態は番号で引く配列に持つ。起点を替えるときは世代を進めるだけ。
 *   - 同じファイルを同じ枝の選び方で読んだ 2 回目以降は、定義の収集を省く（条件の評価と
 *     マクロの状態の更新だけを行う）。
 *   - どこからもインクルードされていないファイルを先に起点にし、どの起点からも読まれなかった
 *     ファイルだけを追加の起点にする。
 */

static uint32_t intern(defs *d, const char *name, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i, slot;
    for (i = 0; i < len; i++) { h ^= (unsigned char)name[i]; h *= 16777619u; }
    if (d->sym_cap) {
        slot = h & (d->sym_cap - 1);
        while (d->sym_index[slot]) {
            const char *s = d->syms[d->sym_index[slot]].name;
            if (!strncmp(s, name, len) && !s[len]) return d->sym_index[slot];
            slot = (slot + 1) & (d->sym_cap - 1);
        }
    }
    if (!d->nsyms) { d->syms = sim_xcalloc(64, sizeof *d->syms); d->syms_alloc = 64; d->nsyms = 1; }   /* 0 は「なし」 */
    if (d->nsyms == d->syms_alloc) {
        d->syms = sim_xrealloc(d->syms, d->syms_alloc * 2 * sizeof *d->syms);
        memset(d->syms + d->syms_alloc, 0, d->syms_alloc * sizeof *d->syms);
        d->syms_alloc *= 2;
    }
    d->syms[d->nsyms].name = sim_xstrndup(name, len);
    d->syms[d->nsyms].hash = h;
    d->nsyms++;
    if (d->nsyms * 2 >= d->sym_cap) {
        d->sym_cap = d->sym_cap ? d->sym_cap * 2 : 256;
        free(d->sym_index); d->sym_index = sim_xcalloc(d->sym_cap, sizeof *d->sym_index);
        for (i = 1; i < d->nsyms; i++) {
            slot = d->syms[i].hash & (d->sym_cap - 1);
            while (d->sym_index[slot]) slot = (slot + 1) & (d->sym_cap - 1);
            d->sym_index[slot] = (uint32_t)i;
        }
    } else {
        slot = h & (d->sym_cap - 1);
        while (d->sym_index[slot]) slot = (slot + 1) & (d->sym_cap - 1);
        d->sym_index[slot] = (uint32_t)(d->nsyms - 1);
    }
    return (uint32_t)(d->nsyms - 1);
}

/* いまの起点での状態（SIM_PP_*）と本体 */
static int sym_state(const defs *d, uint32_t id, const char **body)
{
    const psym *p = &d->syms[id];
    if (p->gen == d->gen) { if (body) *body = p->body; return p->kind; }
    if (body) *body = p->base_body;
    return p->base_kind;
}

/* 条件の中で未定義だった識別子を数える。同じ箇所（指令）からは最初の 1 回の評価だけ数える */
static void note_lookup(defs *d, uint32_t id, int kind)
{
    if (kind != SIM_PP_UNDEF || !d->log || (d->site && d->site->guard == id)) return;
    if (d->syms[id].defined_somewhere) d->syms[id].hidden_refs++; else d->syms[id].undef_refs++;
    d->logged_any = 1;
}

/* 条件の評価で状態を参照したことを記録する（memo の reads になる） */
static void record_read(defs *d, uint32_t id, int kind, const char *body)
{
    pp_read *r;
    if (d->nrlog == d->cap_rlog) { d->cap_rlog = d->cap_rlog ? d->cap_rlog * 2 : 1024; d->rlog = sim_xrealloc(d->rlog, d->cap_rlog * sizeof *d->rlog); }
    r = &d->rlog[d->nrlog++];
    r->sym = id; r->kind = (unsigned char)kind; r->body = body;
    r->wtick = d->syms[id].gen == d->gen ? d->syms[id].wtick : 0;
}
static void write_sym(defs *d, uint32_t id, int kind, const char *body)
{
    psym *p = &d->syms[id];
    pp_write *w;
    p->gen = d->gen; p->kind = (unsigned char)kind; p->body = body; p->wtick = ++d->tick;
    if (d->nwlog == d->cap_wlog) { d->cap_wlog = d->cap_wlog ? d->cap_wlog * 2 : 1024; d->wlog = sim_xrealloc(d->wlog, d->cap_wlog * sizeof *d->wlog); }
    w = &d->wlog[d->nwlog++];
    w->sym = id; w->kind = (unsigned char)kind; w->body = body;
}

static int lookup_cb(void *ctx, const char *name, size_t len, const char **body, int in_defined)
{
    defs *d = ctx;
    uint32_t id = intern(d, name, len);
    const char *b = NULL;
    int kind = sym_state(d, id, &b);
    (void)in_defined;
    *body = b;
    record_read(d, id, kind, b);
    note_lookup(d, id, kind);
    return kind;
}

static const char *skip_blank(const char *s) { while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\f' || *s == '\v') s++; return s; }

/* ファイルを読み、行と指令に分ける */
static void load_source(defs *d, source *s)
{
    size_t n, li, nopen = 0, cap_open = 0, cap_dirs = 0, k;
    char *raw = sim_read_file(s->path, &n);
    uint32_t *open = NULL;
    if (!raw) { snprintf(d->err, d->cap, "defines: %s を読めません", s->path); d->error = 1; return; }
    d->bytes += n;
    {
        int enc=source_encoding((const unsigned char *)raw,n);
        if (enc==ENC_UTF16) { d->enc_utf16++; n=0; }
        else if (enc==ENC_UTF8) d->enc_utf8++;
        else if (enc==ENC_SJIS) d->enc_sjis++;
        else d->enc_ascii++;
        s->text = clean_source(raw, n, &s->line_no, &s->len,enc,s);
        check_delimiters(s);
    }
    free(raw);
    if (strstr(s->text,"__packed") || strstr(s->text,"__attribute__")) s->packing=1;
    s->has_types = strstr(s->text, "typedef") || strstr(s->text, "struct") || strstr(s->text, "union");
    for (k = 0, s->nlines = 1; k < s->len; k++) if (s->text[k] == '\n') s->nlines++;
    s->line_off = sim_xmalloc(s->nlines * sizeof *s->line_off);
    s->line_off[0] = 0;
    for (k = 0, li = 1; k < s->len; k++) if (s->text[k] == '\n') { s->text[k] = 0; s->line_off[li++] = (uint32_t)(k + 1); }
    for (li = 0; li < s->nlines; li++) {
        const char *q = skip_blank(s->text + s->line_off[li]);
        directive x;
        lexer m;
        if (*q != '#') continue;
        m = lex(q + 1);
        if (!strcmp(m.tok,"pragma")) { lexer x=m; next(&x); if (!strcmp(x.tok,"pack")) s->packing=1; }
        memset(&x, 0, sizeof x);
        x.line = (uint32_t)li; x.next = UINT32_MAX; x.target = -2; x.arg = "";
        if (!strcmp(m.tok, "define") || !strcmp(m.tok, "undef")) {
            x.kind = m.tok[0] == 'd' ? D_DEFINE : D_UNDEF;
            next(&m);
            if (!sim_is_ident_start((unsigned char)m.tok[0])) continue;
            x.sym = intern(d, m.tok, strlen(m.tok));
            x.arg = m.p; x.funclike = *m.p == '(';
            if (x.kind == D_DEFINE) d->syms[x.sym].defined_somewhere = 1;
        } else if (!strcmp(m.tok, "ifdef") || !strcmp(m.tok, "ifndef")) {
            x.kind = m.tok[2] == 'd' ? D_IFDEF : D_IFNDEF;
            next(&m);
            if (sim_is_ident_start((unsigned char)m.tok[0])) x.sym = intern(d, m.tok, strlen(m.tok));
        } else if (!strcmp(m.tok, "if") || !strcmp(m.tok, "elif")) {
            x.kind = m.tok[0] == 'i' ? D_IF : D_ELIF; x.arg = m.p;
        } else if (!strcmp(m.tok, "else")) x.kind = D_ELSE;
        else if (!strcmp(m.tok, "endif")) x.kind = D_ENDIF;
        else if (!strcmp(m.tok, "include")) {
            char *b = (char *)skip_blank(m.p), *e, close = *b == '"' ? '"' : '>';
            if (*b != '"' && *b != '<') continue;      /* マクロで書いたインクルードは読まない */
            e = strchr(++b, close);
            if (!e || e == b) continue;
            *e = 0;
            x.kind = D_INCLUDE; x.arg = b;
        } else continue;                                /* #pragma、#error、#line など */
        /* 条件の系列をつなぐ。対応のない #elif / #else / #endif は無視する */
        if (x.kind == D_ELIF || x.kind == D_ELSE || x.kind == D_ENDIF) {
            if (!nopen) continue;
            s->dirs[open[nopen - 1]].next = (uint32_t)s->ndirs;
            if (x.kind == D_ENDIF) nopen--; else open[nopen - 1] = (uint32_t)s->ndirs;
        } else if (x.kind == D_IF || x.kind == D_IFDEF || x.kind == D_IFNDEF) {
            if (nopen == cap_open) { cap_open = cap_open ? cap_open * 2 : 16; open = sim_xrealloc(open, cap_open * sizeof *open); }
            open[nopen++] = (uint32_t)s->ndirs;
        }
        if (s->ndirs == cap_dirs) { cap_dirs = cap_dirs ? cap_dirs * 2 : 16; s->dirs = sim_xrealloc(s->dirs, cap_dirs * sizeof *s->dirs); }
        s->dirs[s->ndirs++] = x;
    }
    if (nopen) bad_source(s,s->line_no[s->dirs[open[0]].line]);
    free(open);
    for (k = 0; k < s->ndirs; k++) {
        directive *x = &s->dirs[k];
        if (x->next == UINT32_MAX) x->next = (uint32_t)s->ndirs;    /* #endif のない系列はファイルの終わりまで */
        /* インクルードガード: 条件の直後の #define が、その条件で調べている名前 */
        if (k + 1 < s->ndirs && s->dirs[k + 1].kind == D_DEFINE) {
            const char *name = d->syms[s->dirs[k + 1].sym].name;
            if (x->kind == D_IFNDEF && x->sym == s->dirs[k + 1].sym) x->guard = x->sym;
            else if (x->kind == D_IF && strstr(x->arg, "defined") && strstr(x->arg, name)) x->guard = s->dirs[k + 1].sym;
        }
    }
    d->ndirectives += s->ndirs;
}

/* '/' 区切り・小文字にしたパス */
static char *lower_path(const char *p)
{
    char *s = sim_xstrdup(p), *q;
    for (q = s; *q; q++) *q = *q == '\\' ? '/' : (char)tolower((unsigned char)*q);
    return s;
}
/* 見つからなかった指令を名前ごとに集計する。表示名と場所は source から借りる */
static void count_missing_include(defs *d, const source *s, const directive *x)
{
    char *key = lower_path(x->arg);
    missing_include *m;
    size_t k;
    for (k = 0; k < d->nmissing; k++) if (!strcmp(d->missing[k].key, key)) {
        d->missing[k].count++; free(key); return;
    }
    if (d->nmissing == d->missing_alloc) {
        d->missing_alloc = d->missing_alloc ? d->missing_alloc * 2 : 16;
        d->missing = sim_xrealloc(d->missing, d->missing_alloc * sizeof *d->missing);
    }
    m = &d->missing[d->nmissing++];
    m->key = key; m->name = x->arg; m->file = s->path;
    m->count = 1; m->line = s->line_no[x->line];
}
static int missing_cmp(const void *a, const void *b)
{
    const missing_include *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return strcmp(x->key, y->key);
}
static const char *base_of(const char *lower) { const char *b = strrchr(lower, '/'); return b ? b + 1 : lower; }
static defs *g_sort;    /* qsort の比較用 */
static int base_cmp(const void *a, const void *b)
{
    const source *x = &g_sort->src[*(const uint32_t *)a], *y = &g_sort->src[*(const uint32_t *)b];
    int c = strcmp(base_of(x->lower), base_of(y->lower));
    return c ? c : strcmp(x->lower, y->lower);
}

/* #include の名前を、走査したファイルの中から探す（パスの末尾一致、大文字小文字を区別しない）。
   同名が複数あれば、インクルード元と共通するフォルダの階層が深いものを選ぶ。なければ -1 */
static int32_t resolve_include(defs *d, const source *from, const char *name)
{
    char buf[512];
    const char *base;
    size_t n = 0, lo = 0, hi = d->nsrc, blen;
    int32_t best = -1;
    unsigned best_score = 0, best_depth = 0;
    for (;;) {      /* 先頭の ./ と ../ は照合に使わない */
        if (name[0] == '.' && (name[1] == '/' || name[1] == '\\')) name += 2;
        else if (name[0] == '.' && name[1] == '.' && (name[2] == '/' || name[2] == '\\')) name += 3;
        else break;
    }
    for (; name[n] && n + 1 < sizeof buf; n++) buf[n] = name[n] == '\\' ? '/' : (char)tolower((unsigned char)name[n]);
    buf[n] = 0;
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if (!n) return -1;
    base = base_of(buf);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(base_of(d->src[d->by_base[mid]].lower), base) < 0) lo = mid + 1; else hi = mid;
    }
    blen = strlen(buf);
    for (; lo < d->nsrc; lo++) {
        const source *c = &d->src[d->by_base[lo]];
        size_t clen = strlen(c->lower), i;
        unsigned score = 0, depth = 0;
        if (strcmp(base_of(c->lower), base)) break;
        if (clen < blen || strcmp(c->lower + clen - blen, buf) || (clen > blen && c->lower[clen - blen - 1] != '/')) continue;
        for (i = 0; from->lower[i] && from->lower[i] == c->lower[i]; i++) if (c->lower[i] == '/') score++;
        for (i = 0; c->lower[i]; i++) if (c->lower[i] == '/') depth++;
        if (best < 0 || score > best_score || (score == best_score && depth < best_depth)) {
            best = (int32_t)d->by_base[lo]; best_score = score; best_depth = depth;
        }
    }
    return best;
}

/* 条件を評価する。真 1、偽 0、判定不能 -1 */
static int eval_dir(defs *d, directive *x)
{
    int r;
    d->site = x; d->log = !x->logged; d->logged_any = 0;
    if (x->kind == D_IFDEF || x->kind == D_IFNDEF) {
        if (!x->sym) r = -1;
        else {
            const char *body = NULL;
            int kind = sym_state(d, x->sym, &body);
            record_read(d, x->sym, kind, body);
            note_lookup(d, x->sym, kind);
            r = (kind != SIM_PP_UNDEF) == (x->kind == D_IFDEF);
        }
    } else r = sim_pp_eval(x->arg, &d->env);
    if (d->logged_any) x->logged = 1;
    x->evaluated = 1;
    if (r < 0) x->indeterminate = 1;
    d->site = NULL;
    return r;
}

static size_t line_of_offset(const source *s, size_t off)
{
    size_t lo = 0, hi = s->nlines;
    while (hi - lo > 1) { size_t mid = lo + (hi - lo) / 2; if (s->line_off[mid] <= off) lo = mid; else hi = mid; }
    return lo;
}

/* 宣言指定子だけを読み飛ばす。宣言子の * / [] / () は別に保存する。 */
static int builtin_word(const char *s)
{
    return !strcmp(s,"unsigned") || !strcmp(s,"signed") || !strcmp(s,"char") ||
        !strcmp(s,"short") || !strcmp(s,"int") || !strcmp(s,"long") ||
        !strcmp(s,"float") || !strcmp(s,"double") || !strcmp(s,"void") ||
        !strcmp(s,"_Bool") || !strcmp(s,"bool");
}
/* 未対応の配置指定も宣言の収集時には飛ばし、別名を見失わない。 */
static void skip_layout(lexer *l)
{
    while (!strcmp(l->tok,"__packed") || !strcmp(l->tok,"__attribute__")) {
        int attr=!strcmp(l->tok,"__attribute__"),depth=0;
        next(l);
        if (attr && !strcmp(l->tok,"(")) do {
            if (!strcmp(l->tok,"(")) depth++;
            if (!strcmp(l->tok,")")) depth--;
            next(l);
        } while (l->tok[0] && depth);
    }
}
static void skip_spec(lexer *l)
{
    int seen=0;
    while (qualifier(l->tok) || !strcmp(l->tok,"__packed") || !strcmp(l->tok,"__attribute__")) {
        if (qualifier(l->tok)) next(l); else skip_layout(l);
    }
    if (!strcmp(l->tok,"struct") || !strcmp(l->tok,"union") || !strcmp(l->tok,"enum")) {
        int depth=0;
        next(l); skip_layout(l);
        if (sim_is_ident_start((unsigned char)l->tok[0])) next(l);
        skip_layout(l);
        if (!strcmp(l->tok,"{")) do {
            if (!strcmp(l->tok,"{")) depth++;
            if (!strcmp(l->tok,"}")) depth--;
            next(l);
        } while (l->tok[0] && depth);
    } else {
        while (builtin_word(l->tok) || qualifier(l->tok)) { seen=1; next(l); }
        if (!seen && sim_is_ident_start((unsigned char)l->tok[0])) next(l);
    }
    while (qualifier(l->tok)) next(l);
}

/* 配置指定はincludeをまたぐので、走査対象にあれば全レコードを保守的に拒否する。 */
static void scan_types(defs *d, const source *s, size_t base)
{
    char *t=sim_xmalloc(s->len+1);
    size_t li,r=base;
    int packed=d->packing;
    lexer l;
    memcpy(t,s->text,s->len+1);
    for (li=0;li<s->nlines;li++) {
        size_t b=s->line_off[li],e=li+1<s->nlines?s->line_off[li+1]-1:s->len;
        const char *q=skip_blank(t+b);
        while (r<d->nranges && d->ranges[r+1]<=li) r+=2;
        if ((r<d->nranges && li>=d->ranges[r]) || *q=='#') memset(t+b,' ',e-b);
        if (li+1<s->nlines) t[e]='\n';
    }
    l=lex(t);
    while (l.tok[0]) {
        int td=!strcmp(l.tok,"typedef");
        if (td || !strcmp(l.tok,"struct") || !strcmp(l.tok,"union") || !strcmp(l.tok,"enum")) {
            const char *body,*end;
            lexer spec,taglex;
            char tag[128]="";
            unsigned line=s->line_no[line_of_offset(s,(size_t)(l.start-t))];
            int record;
            if (td) next(&l);
            body=l.start; taglex=l;
            while (qualifier(taglex.tok)) next(&taglex);
            skip_layout(&taglex);
            record=!strcmp(taglex.tok,"struct") || !strcmp(taglex.tok,"union") || !strcmp(taglex.tok,"enum");
            if (record) { next(&taglex); skip_layout(&taglex); if (sim_is_ident_start((unsigned char)taglex.tok[0])) snprintf(tag,sizeof tag,"%s",taglex.tok); }
            skip_spec(&l); spec=l; end=l.start;
            if (record && tag[0]) {
                char *text=sim_xstrndup(body,(size_t)(end-body));
                add_definition(d,2,tag,packed?"__unsupported_packed":text,s->path,line); free(text);
            }
            if (td) do {
                const char *begin=l.start,*name_at=NULL,*name_end=NULL;
                char name[128]="";
                int depth=0;
                while (l.tok[0] && strcmp(l.tok,";") && (depth || strcmp(l.tok,","))) {
                    if (!name_at && sim_is_ident_start((unsigned char)l.tok[0]) && !qualifier(l.tok)) {
                        name_at=l.start; name_end=l.p; snprintf(name,sizeof name,"%s",l.tok);
                    }
                    if (!strcmp(l.tok,"(") || !strcmp(l.tok,"[")) depth++;
                    if (!strcmp(l.tok,")") || !strcmp(l.tok,"]")) depth--;
                    next(&l);
                }
                if (name_at) {
                    sim_buf text; sim_buf_init(&text);
                    sim_buf_printf(&text,"%.*s %.*s %.*s",(int)(spec.start-body),body,
                        (int)(name_at-begin),begin,(int)(l.start-name_end),name_end);
                    add_definition(d,1,name,packed && record?"__unsupported_packed":text.p,s->path,line);
                    sim_buf_free(&text);
                }
                if (strcmp(l.tok,",")) break;
                next(&l);
            } while (l.tok[0]);
            else if (!strcmp(l.tok,";")) { next(&l); continue; }
        }
        if (l.tok[0]) next(&l);
    }
    free(t);
}

/* 有効な行の定義を集める。同じファイルを同じ枝の選び方で読んだことがあれば何もしない */
static void collect(defs *d, source *s, size_t base)
{
    uint64_t h = UINT64_C(1469598103934665603);
    size_t i, r = base;
    for (i = base; i < d->nranges; i++) { h ^= d->ranges[i]; h *= UINT64_C(1099511628211); }
    for (i = 0; i < s->nseen; i++) if (s->seen[i] == h) return;
    s->seen = sim_xrealloc(s->seen, (s->nseen + 1) * sizeof *s->seen);
    s->seen[s->nseen++] = h;
    d->collections++;
    for (i = 0; i < s->ndirs; i++) {
        const directive *x = &s->dirs[i];
        if (x->kind != D_DEFINE || x->funclike) continue;
        while (r < d->nranges && d->ranges[r + 1] <= x->line) r += 2;
        if (r < d->nranges && x->line >= d->ranges[r]) continue;
        add_definition(d, 0, d->syms[x->sym].name, x->arg, s->path, s->line_no[x->line]);
    }
    if (s->has_types) scan_types(d, s, base);
}

/* 指令 i の枝（次の枝の指令 nx の手前まで）を読まない */
static void skip_branch(defs *d, const source *s, uint32_t i, uint32_t nx)
{
    uint32_t from = s->dirs[i].line + 1, to = nx < s->ndirs ? s->dirs[nx].line : (uint32_t)s->nlines;
    if (from >= to) return;
    if (d->nranges + 2 > d->cap_ranges) { d->cap_ranges = d->cap_ranges ? d->cap_ranges * 2 : 64; d->ranges = sim_xrealloc(d->ranges, d->cap_ranges * sizeof *d->ranges); }
    d->ranges[d->nranges++] = from; d->ranges[d->nranges++] = to;
}

#define MAX_INCLUDE_DEPTH 64        /* #include の入れ子。循環はここで打ち切る */
#define MAX_ROOT_INCLUDES 200000    /* 1 つの起点で展開する #include の数（ガードのない循環が枝分かれする場合の歯止め） */
#ifndef MAX_MEMOS
#define MAX_MEMOS 8                 /* 1 ファイルあたりの記録の数（0 にすると毎回読み直す。結果の比較用） */
#endif
#define MAX_MEMO_CELLS 8000000      /* 記録全体の大きさの上限（reads + writes の件数） */

static unsigned process_file(defs *d, uint32_t fi, unsigned depth);

/* #include されたファイルを読む。同じ状態で読んだ記録があれば、書き込みを再生するだけにする。入れ子の深さを返す */
static unsigned include_file(defs *d, uint32_t fi, unsigned depth)
{
    source *t = &d->src[fi];
    unsigned m;
    size_t k;
    d->includes++; d->root_includes++;
    for (m = 0; m < t->nmemos; m++) {
        const memo *e = &t->memos[m];
        if (depth + e->height > MAX_INCLUDE_DEPTH) continue;
        for (k = 0; k < e->nreads; k++) {
            const char *body = NULL;
            int kind = sym_state(d, e->reads[k].sym, &body);
            if (kind != e->reads[k].kind || (kind == SIM_PP_OBJECT && body != e->reads[k].body)) break;
        }
        if (k < e->nreads) continue;
        if (depth > 1) {
            for (k = 0; k < e->nreads; k++) record_read(d, e->reads[k].sym, e->reads[k].kind, e->reads[k].body);
            for (k = 0; k < e->nwrites; k++) write_sym(d, e->writes[k].sym, e->writes[k].kind, e->writes[k].body);
        } else {
            /* 起点から直接の #include: 外側に記録を作るファイルがないので、読み書きの記録は省いて状態だけ更新する */
            for (k = 0; k < e->nwrites; k++) {
                psym *p = &d->syms[e->writes[k].sym];
                p->gen = d->gen; p->kind = e->writes[k].kind; p->body = e->writes[k].body; p->wtick = ++d->tick;
            }
        }
        d->includes += e->includes; d->root_includes += e->includes; d->memo_hits++;
        return e->height;
    }
    return process_file(d, fi, depth);
}

static unsigned process_file(defs *d, uint32_t fi, unsigned depth)
{
    source *s = &d->src[fi];
    size_t base = d->nranges, fbase = d->nframes, rstart = d->nrlog, wstart = d->nwlog;
    uint32_t i = 0, t0 = d->tick;
    unsigned height = 0, cuts0 = d->include_cut, inc0 = d->includes;
    s->visited = 1;
    if (s->packing) d->packing = 1;     /* たどって足したファイルの配置指定は、実際に読まれたときに効かせる */
    while (i < s->ndirs) {
        directive *x = &s->dirs[i];
        switch (x->kind) {
        case D_IF: case D_IFDEF: case D_IFNDEF: {
            /* 系列の条件を先に上から評価する（#elif は前の枝がすべて偽のときだけ評価されるので、
               ここでの状態で決まる）。真の枝より前に判定不能があれば、系列のすべての枝を読む */
            int32_t take = -1;
            int all = !d->cond;
            uint32_t j = i;
            while (!all) {
                directive *y = &s->dirs[j];
                int r = y->kind == D_ELSE ? 1 : eval_dir(d, y);
                if (r < 0) all = 1;
                else if (r) { take = (int32_t)j; break; }
                else if (y->next >= s->ndirs || s->dirs[y->next].kind == D_ENDIF) break;
                else j = y->next;
            }
            if (d->nframes == d->cap_frames) { d->cap_frames = d->cap_frames ? d->cap_frames * 2 : 32; d->frames = sim_xrealloc(d->frames, d->cap_frames * sizeof *d->frames); }
            d->frames[d->nframes].take = take; d->frames[d->nframes].all = all; d->nframes++;
            if (all || take == (int32_t)i) i++;
            else { skip_branch(d, s, i, x->next); i = x->next; }
            continue;
        }
        case D_ELIF: case D_ELSE:
            if (d->nframes > fbase && (d->frames[d->nframes - 1].all || d->frames[d->nframes - 1].take == (int32_t)i)) i++;
            else { skip_branch(d, s, i, x->next); i = x->next; }
            continue;
        case D_ENDIF:
            if (d->nframes > fbase) d->nframes--;
            break;
        case D_DEFINE: case D_UNDEF:
            if (d->cond) write_sym(d, x->sym, x->kind == D_UNDEF ? SIM_PP_UNDEF : x->funclike ? SIM_PP_FUNCTION : SIM_PP_OBJECT, x->arg);
            break;
        case D_INCLUDE: {
            unsigned h;
            if (!d->cond) break;
            if (x->target == -2) x->target = resolve_include(d, s, x->arg);
            if (x->target < 0) {
                if (!x->missing_counted) { x->missing_counted = 1; count_missing_include(d, s, x); }
                break;
            }
            /* 自分自身を含め、読んでいる途中のファイルもそのまま読む（インクルードガードが普通に効く）。
               ガードのない循環は深さの上限で打ち切る */
            if (depth >= MAX_INCLUDE_DEPTH || d->root_includes >= MAX_ROOT_INCLUDES) { d->include_cut++; break; }
            h = include_file(d, (uint32_t)x->target, depth + 1) + 1;
            if (h > height) height = h;
            break;
        }
        }
        i++;
    }
    d->nframes = fbase;
    collect(d, s, base);
    d->nranges = base;
    /* 結果を記録する。打ち切りがあった読み方は、深さによって結果が変わるので記録しない */
    if (depth && d->include_cut == cuts0 && s->nmemos < MAX_MEMOS && d->memo_cells < MAX_MEMO_CELLS) {
        memo e;
        size_t k, n = 0;
        uint32_t serial = ++d->serial;
        memset(&e, 0, sizeof e);
        for (k = rstart; k < d->nrlog; k++) if (d->rlog[k].wtick <= t0 && d->syms[d->rlog[k].sym].mark != serial) { d->syms[d->rlog[k].sym].mark = serial; n++; }
        e.reads = sim_xmalloc((n ? n : 1) * sizeof *e.reads);
        serial = ++d->serial;
        for (k = rstart; k < d->nrlog; k++) if (d->rlog[k].wtick <= t0 && d->syms[d->rlog[k].sym].mark != serial) { d->syms[d->rlog[k].sym].mark = serial; e.reads[e.nreads++] = d->rlog[k]; }
        e.nwrites = d->nwlog - wstart;
        e.writes = sim_xmalloc((e.nwrites ? e.nwrites : 1) * sizeof *e.writes);
        if (e.nwrites) memcpy(e.writes, d->wlog + wstart, e.nwrites * sizeof *e.writes);
        e.includes = d->includes - inc0; e.height = height;
        s->memos = sim_xrealloc(s->memos, (s->nmemos + 1) * sizeof *s->memos);
        s->memos[s->nmemos++] = e;
        d->memo_cells += e.nreads + e.nwrites;
    }
    return height;
}

static void free_sources(defs *d)
{
    size_t k;
    for (k = 0; k < d->nmissing; k++) free(d->missing[k].key);
    free(d->missing);
    for (k = 0; k < d->nsrc; k++) {
        source *s = &d->src[k];
        unsigned m;
        for (m = 0; m < s->nmemos; m++) { free(s->memos[m].reads); free(s->memos[m].writes); }
        free(s->lower); free(s->text); free(s->line_off); free(s->line_no); free(s->dirs); free(s->seen); free(s->memos);
    }
    free(d->src); free(d->by_base); free(d->ranges); free(d->frames); free(d->rlog); free(d->wlog);
    for (k = 0; k < d->pcap; k++) free(d->pkeys[k]);
    free(d->pkeys); free(d->pvals); free(d->file_closure);
    for (k = 0; k < d->nincdirs; k++) free(d->incdirs[k]);
    free(d->incdirs);
    for (k = 1; k < d->nsyms; k++) free(d->syms[k].name);
    free(d->syms); free(d->sym_index);
    for (k = 0; k < d->ext_cap; k++) free(d->ext[k].name);
    free(d->ext);
    for (k = 0; k < d->nowned; k++) free(d->owned[k]);
    free(d->owned);
}

/* ---- defines_scope = mtpj: パス -> ファイルの番号の表（開番地法）。ないと分かったパスも -1 で覚える
   （同じヘッダを何百回も #include するので、そのたびにファイルの有無を OS に聞かない） */
static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
static size_t path_slot(const defs *d, const char *key)
{
    size_t i = path_hash(key) & (d->pcap - 1);
    while (d->pkeys[i] && strcmp(d->pkeys[i], key)) i = (i + 1) & (d->pcap - 1);
    return i;
}
/* key は表が持つ（あとで free する） */
static void path_put(defs *d, char *key, int32_t value)
{
    size_t i;
    if ((d->pcount + 1) * 2 > d->pcap) {
        char **keys = d->pkeys; int32_t *vals = d->pvals;
        size_t old = d->pcap, k;
        d->pcap = old ? old * 2 : 1024;
        d->pkeys = sim_xcalloc(d->pcap, sizeof *d->pkeys);
        d->pvals = sim_xmalloc(d->pcap * sizeof *d->pvals);
        for (k = 0; k < old; k++) if (keys[k]) { i = path_slot(d, keys[k]); d->pkeys[i] = keys[k]; d->pvals[i] = vals[k]; }
        free(keys); free(vals);
    }
    i = path_slot(d, key);
    if (d->pkeys[i]) { free(key); d->pvals[i] = value; return; }
    d->pkeys[i] = key; d->pvals[i] = value; d->pcount++;
}

/* コンパイラと同じ順番で #include を探す: インクルード元のフォルダ、インクルード・パスの順。
   まだ読んでいないファイルが見つかれば読む対象に足す。見つからなければ -2（あとでファイル名から探す） */
static int32_t scope_include(defs *d, size_t from, const char *name)
{
    size_t n = strlen(name), j, dirlen;
    const char *from_path = d->files[from], *b = sim_basename(from_path);
    char *nm;
    int32_t found = -2;
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) n--;
    if (!n) return -2;
    nm = sim_xstrndup(name, n);
    dirlen = (size_t)(b - from_path);
    for (j = 0; j <= d->nincdirs && found < 0; j++) {
        char *joined, *path, *key;
        size_t slot;
        if (absolute_path(nm)) { if (j) break; joined = sim_xstrdup(nm); }
        else if (j == 0) {
            char *dir = dirlen ? sim_xstrndup(from_path, dirlen) : sim_xstrdup(".");
            joined = sim_path_join(dir, nm); free(dir);
        } else joined = sim_path_join(d->incdirs[j - 1], nm);
        path = normal_path(joined); free(joined);
        key = lower_path(path);
        slot = d->pcap ? path_slot(d, key) : 0;
        if (d->pcap && d->pkeys[slot]) { found = d->pvals[slot] >= 0 ? d->pvals[slot] : -2; free(key); free(path); continue; }
        if (sim_path_kind(path) == 1 && !excluded(d, path)) {
            found = (int32_t)d->nfiles;
            path_put(d, key, found);
            push_file(d, path, 1);
            d->scope_added++;
        } else { path_put(d, key, -1); free(path); }
    }
    free(nm);
    return found;
}

/* 走査したファイルをすべて読み、条件を評価して定義を集める */
static void preprocess(defs *d)
{
    size_t k, i, src_alloc = d->nfiles ? d->nfiles : 1;
    uint64_t t0 = sim_now_ns();
    d->src = sim_xcalloc(src_alloc, sizeof *d->src);
    if (d->scope) for (k = 0; k < d->nfiles; k++) path_put(d, lower_path(d->files[k]), (int32_t)k);
    /* defines_scope = mtpj では、読んだファイルの #include から読む対象（files）が増えていく */
    for (k = 0; k < d->nfiles && !d->error; k++) {
        if (k == src_alloc) {
            d->src = sim_xrealloc(d->src, src_alloc * 2 * sizeof *d->src);
            memset(d->src + src_alloc, 0, src_alloc * sizeof *d->src);
            src_alloc *= 2;
        }
        d->nsrc = k + 1;
        d->src[k].path = d->files[k]; d->src[k].lower = lower_path(d->files[k]);
        d->src[k].closure = d->file_closure ? d->file_closure[k] : 0;
        load_source(d, &d->src[k]);
        /* 配置指定があれば全レコードを保守的に拒否する。ただし #include をたどって足しただけのファイルは、
           無効な枝からしか参照されていないかもしれないので、ここでは効かせない（読まれたときに process_file が効かせる） */
        if (d->src[k].packing && !(d->scope && d->cond && d->src[k].closure)) d->packing = 1;
        if (d->scope && !d->error)
            for (i = 0; i < d->src[k].ndirs; i++) {
                directive *x = &d->src[k].dirs[i];
                if (x->kind == D_INCLUDE) x->target = scope_include(d, k, x->arg);
            }
    }
    d->nsrc = d->error ? d->nsrc : d->nfiles;
    d->by_base = sim_xmalloc((d->nsrc ? d->nsrc : 1) * sizeof *d->by_base);
    if (d->error) return;
    d->t_read = sim_now_ns() - t0; t0 = sim_now_ns();
    for (k = 0; k < d->nsrc; k++) d->by_base[k] = (uint32_t)k;
    g_sort = d; qsort(d->by_base, d->nsrc, sizeof *d->by_base, base_cmp);
    if (d->cond)
        for (k = 0; k < d->nsrc; k++) for (i = 0; i < d->src[k].ndirs; i++) {
            directive *x = &d->src[k].dirs[i];
            if (x->kind != D_INCLUDE) continue;
            if (d->scope && x->target >= 0) { d->src[x->target].included = 1; continue; }
            /* 範囲を絞らない方式、または上の順番で見つからなかったもの: 読んだファイルの中からファイル名で探す */
            x->target = resolve_include(d, &d->src[k], x->arg);
            if (x->target >= 0) { d->src[x->target].included = 1; if (d->scope) d->scope_fallback++; }
            else d->include_missing++;
        }
    for (i = 0; i < 2; i++)
        for (k = 0; k < d->nsrc; k++) {
            if (i == 0 ? d->src[k].included : d->src[k].visited) continue;
            /* #include をたどって足しただけのファイルは起点にしない（読まれるのは、有効な枝からインクルードされたときだけ） */
            if (d->scope && d->cond && d->src[k].closure) continue;
            d->gen++; d->roots++;
            d->tick = 0; d->nrlog = d->nwlog = 0; d->root_includes = 0;
            process_file(d, (uint32_t)k, 0);
        }
    for (k = 0; k < d->nsrc; k++) for (i = 0; i < d->src[k].ndirs; i++) {
        d->if_evaluated += d->src[k].dirs[i].evaluated;
        d->if_indeterminate += d->src[k].dirs[i].indeterminate;
    }
    d->t_cond = sim_now_ns() - t0;
}

/* ---------------------------------------------------------------- 定義マクロ（cdef / mtpj / 暗黙） */

enum { ORIGIN_IMPLICIT = 1, ORIGIN_MTPJ, ORIGIN_CFG, ORIGIN_CLI };

static char *own(defs *d, char *s)
{
    d->owned = sim_xrealloc(d->owned, (d->nowned + 1) * sizeof *d->owned);
    return d->owned[d->nowned++] = s;
}

/* 「名前」または「名前=値」。値を省くと 1 */
static int add_cdef(defs *d, const char *text, int origin, const char *what)
{
    const char *p = skip_blank(text), *b = p, *v;
    uint32_t id;
    size_t n;
    while (sim_is_ident_char((unsigned char)*p)) p++;
    v = skip_blank(p);
    if (p == b || !sim_is_ident_start((unsigned char)*b) || (*v && *v != '=')) {
        snprintf(d->err, d->cap, "%s '%s' は 名前 または 名前=値 の形で指定してください", what, text);
        d->error = 1; return -1;
    }
    id = intern(d, b, (size_t)(p - b));
    if (*v == '=') {
        v = skip_blank(v + 1);
        n = strlen(v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) n--;
        d->syms[id].base_body = own(d, sim_xstrndup(v, n));
    } else d->syms[id].base_body = "1";
    d->syms[id].base_kind = SIM_PP_OBJECT; d->syms[id].origin = (unsigned char)origin;
    return 0;
}

static int add_cundef(defs *d, const char *text, const char *what)
{
    const char *p = skip_blank(text), *b = p;
    uint32_t id;
    while (sim_is_ident_char((unsigned char)*p)) p++;
    if (p == b || !sim_is_ident_start((unsigned char)*b) || *skip_blank(p)) {
        snprintf(d->err, d->cap, "%s '%s' は名前で指定してください", what, text);
        d->error = 1; return -1;
    }
    id = intern(d, b, (size_t)(p - b));
    d->syms[id].base_kind = SIM_PP_UNDEF; d->syms[id].base_body = NULL; d->syms[id].origin = 0;
    return 0;
}

/* 実行方式が PC 用 DLL の方式（--exec native、なければ設定ファイルの exec = native）なら 1 */
static int exec_native(defs *d, const sim_open_opts *o)
{
    const char *e = o->exec && *o->exec ? o->exec : sim_cfg_get(&d->cfg, "exec");
    return e && !strcmp(e, "native");
}

/* ロードモジュールの種類: 0 不明（モックなど）、1 CA78K0R（lmf、PC 用 DLL の方式）、2 CC-RL（abs = ELF） */
static int compiler_kind(defs *d, const sim_open_opts *o)
{
    const char *abi = sim_cfg_get(&d->cfg, "abi"), *abs = sim_cfg_get(&d->cfg, "abs");
    char *path = o->abs_path ? sim_xstrdup(o->abs_path) : abs ? sim_cfg_path(&d->cfg, abs) : NULL;
    unsigned char m[4] = {0};
    size_t n = 0;
    int kind = 0;
    if (path) {
        FILE *fp = sim_fopen(path, "rb");
        if (fp) { n = fread(m, 1, 4, fp); fclose(fp); }
        free(path);
    }
    if (n >= 2 && m[0] == 0x48 && m[1] == 0x11) kind = 1;
    else if (n == 4 && !memcmp(m, "\x7f" "ELF", 4)) kind = 2;
    /* PC 用 DLL の方式はロードモジュールを使わない。DLL は CA78K0R のソースとしてコンパイルする
       （sim_native_pre.h が __K0R__ と __CA78K0R__ を定義する）ので、ヘッダーも同じ条件で読む */
    else if (exec_native(d, o)) kind = 1;
    if (abi && !strcmp(abi, "ca78k0r")) kind = 1;
    else if (abi && !strcmp(abi, "ccrl")) kind = 2;
    return kind;
}

/* コンパイラが暗黙に定義するマクロのうち、品種・メモリモデル・オプションに依らないものだけ。
   CA78K0R: docs/CA78K0R_NOTES.md 6 節（__K0R__ は共通、__CA78K0R__）。__CHAR_UNSIGNED__ は -qu（char_unsigned = on）のとき。
     __RL78__ / __RL78_1__〜3__ / __K0R_SMALL__ など、品種とメモリモデルで変わるものは定義しない。
   CC-RL: __CCRL__ / __CCRL / __RL78__ / __RENESAS__。__RL78_S1__〜S3__ / __RL78_SMALL__ などは定義しない。 */
static void add_implicit(defs *d, int kind)
{
    static const char *const ca[] = {"__K0R__", "__CA78K0R__", NULL};
    static const char *const cc[] = {"__CCRL__", "__CCRL", "__RL78__", "__RENESAS__", NULL};
    const char *const *list = kind == 1 ? ca : kind == 2 ? cc : NULL;
    for (; list && *list; list++) add_cdef(d, *list, ORIGIN_IMPLICIT, "暗黙の定義");
    if (kind == 1 && d->char_unsigned) add_cdef(d, "__CHAR_UNSIGNED__", ORIGIN_IMPLICIT, "暗黙の定義");
}

/* UTF-8 の項目名を表示幅 26 桁にそろえる。長い名前にも区切りを置く。 */
/* 注記（字下げした ※ の行）を、その見出しの最後へまとめる。項目の行の間に注記がはさまると、
   値の位置がそろっていても表として読みにくいため。見出しは「空行 + "== "」で始まる */
static void info_notes_last(sim_buf *info)
{
    const char *s=sim_buf_cstr(info);
    sim_buf out, notes;
    sim_buf_init(&out); sim_buf_init(&notes);
    while (*s) {
        const char *e=strchr(s,'\n');
        size_t n=e ? (size_t)(e-s)+1 : strlen(s);
        int heading=n>=3 && !memcmp(s,"== ",3), blank=n==1 && *s=='\n';
        if ((heading || (blank && !strncmp(s+1,"== ",3))) && notes.len) { sim_buf_add(&out,sim_buf_cstr(&notes),notes.len); notes.len=0; }
        if (n>5 && !memcmp(s,"  \xE2\x80\xBB",5)) sim_buf_add(&notes,s+2,n-2);
        else sim_buf_add(&out,s,n);
        s+=n;
    }
    if (notes.len) sim_buf_add(&out,sim_buf_cstr(&notes),notes.len);
    info->len=0;
    sim_buf_add(info,sim_buf_cstr(&out),out.len);
    sim_buf_free(&out); sim_buf_free(&notes);
}
static void info_item(sim_buf *b, const char *label, const char *fmt, ...)
{
    const unsigned char *p=(const unsigned char *)label;
    unsigned width=0, spaces;
    va_list ap;
    for (; *p; p++) if (*p<0x80) width++; else if (*p>=0xC0) width+=2;
    sim_buf_adds(b,label);
    spaces=width<26?26-width:2;
    while (spaces--) sim_buf_addc(b,' ');
    va_start(ap,fmt);
    sim_buf_vprintf(b,fmt,ap);
    va_end(ap);
    sim_buf_addc(b,'\n');
}

static void list_origin(defs *d, sim_buf *b, int origin, const char *label)
{
    size_t k, n = 0;
    for (k = 1; k < d->nsyms; k++) if (d->syms[k].base_kind && d->syms[k].origin == origin) {
        sim_buf_printf(b, "%s%s=%s", n ? ", " : label, d->syms[k].name, d->syms[k].base_body);
        n++;
    }
    if (n) sim_buf_addc(b, '\n');
}

static int ref_cmp(const void *a, const void *b)
{
    const psym *x = &g_sort->syms[*(const uint32_t *)a], *y = &g_sort->syms[*(const uint32_t *)b];
    unsigned rx = x->undef_refs + x->hidden_refs, ry = y->undef_refs + y->hidden_refs;
    if (rx != ry) return rx < ry ? 1 : -1;
    return strcmp(x->name, y->name);
}

/* 条件の中で未定義だった識別子の一覧。hidden = 0: どこにも定義がないもの、1: 定義はあるがその場所から見えなかったもの */
static void list_undefined(defs *d, sim_buf *b, int hidden, size_t limit)
{
    uint32_t *ids = sim_xmalloc((d->nsyms ? d->nsyms : 1) * sizeof *ids);
    size_t k, n = 0;
    for (k = 1; k < d->nsyms; k++) if (hidden ? d->syms[k].hidden_refs : d->syms[k].undef_refs) ids[n++] = (uint32_t)k;
    g_sort = d; qsort(ids, n, sizeof *ids, ref_cmp);
    if (!hidden) {
        d->undefined = (unsigned)n;
        if (n) {
            info_item(b,"未定義で 0 とした識別子","%zu 個",n);
            if (n > limit) sim_buf_printf(b,"  ※ 未定義で 0 とした識別子は、参照の多い順に上位 %zu 個です。--verbose で全部を表示します\n",limit);
            sim_buf_adds(b,"  ※ 未定義で 0 とした識別子が品種やビルド構成のスイッチなら、cdef / mtpj で与えてください\n");
        }
    } else if (n) {
        info_item(b,"その場所から見えない識別子","%zu 個（走査したファイルに #define あり）",n);
        sim_buf_adds(b,"  ※ その場所から見えない識別子が、.c で定義してからインクルードするスイッチなら、cdef で与えてください\n");
    }
    for (k = 0; k < n && k < limit; k++) {
        const psym *p = &d->syms[ids[k]];
        sim_buf_printf(b, "  %s (%u)\n", p->name, hidden ? p->hidden_refs : p->undef_refs);
    }
    free(ids);
}

static const sim_type basic[] = {
    {SIM_T_UINT,"u8",1,NULL,0,NULL,0,0},{SIM_T_INT,"s8",1,NULL,0,NULL,0,0},
    {SIM_T_UINT,"u16",2,NULL,0,NULL,0,0},{SIM_T_INT,"s16",2,NULL,0,NULL,0,0},
    {SIM_T_UINT,"u32",4,NULL,0,NULL,0,0},{SIM_T_INT,"s32",4,NULL,0,NULL,0,0},
    {SIM_T_FLOAT,"f32",4,NULL,0,NULL,0,0},
    {SIM_T_UINT,"u64",8,NULL,0,NULL,0,0},{SIM_T_INT,"s64",8,NULL,0,NULL,0,0}
};
static const sim_type *resolve_name(defs *,const char *,unsigned);
static const sim_type *resolve_tag(defs *,const char *,sim_kind,unsigned);
static const sim_type *external_type(defs *,const char *,unsigned);
static const sim_type *parse_type(defs *,lexer *,unsigned);
static const sim_type *unqualified_type(defs *,const sim_type *);
static int constant_lookup(void *ctx, const char *name, size_t len, const char **body, int in_defined)
{
    defs *d=ctx;
    char key[128];
    definition *m;
    (void)in_defined;
    if (len>=sizeof key) return SIM_PP_UNDEF;
    memcpy(key,name,len); key[len]=0;
    m=lookup(d,0,key);
    if (!m) return SIM_PP_UNDEF;
    *body=m->body;
    return SIM_PP_OBJECT;
}
/* The caller owns the enclosing delimiter. Evaluate the complete expression,
   with the same token expansion and bounded parser used by #if. */
static uint64_t expression(defs *d,lexer *l,unsigned depth,int *ok)
{
    const char *end;
    uint64_t v=0;
    sim_pp_env env={constant_lookup,d};
    (void)depth;
    if (sim_pp_eval_constant_prefix(l->start,&env,&v,&end)) *ok=0;
    else { l->p=end; next(l); }
    return v;
}

static unsigned align_up(unsigned n,unsigned a) { return (n+a-1)/a*a; }
static unsigned type_alignment(const sim_type *t,unsigned depth)
{
    unsigned a=1; int i;
    while (t && t->kind==SIM_T_ARRAY) t=t->target;
    if (!t || depth>32) return 1;
    if (t->kind!=SIM_T_STRUCT && t->kind!=SIM_T_UNION) return t->size==1?1:2;
    for (i=0;i<t->nmembers;i++) if (type_alignment(t->members[i].type,depth+1)>a) a=2;
    return a;
}
/* 配列は右端が最内側。上限と再帰深さを検査してからサイズを掛ける。 */
static const sim_type *array_suffix(defs *d,lexer *l,const sim_type *t,unsigned depth)
{
    uint64_t n; int ok=1;
    sim_type *a;
    if (strcmp(l->tok,"[")) return t;
    if (!t || !t->size || depth>32) return NULL;
    next(l); n=expression(d,l,0,&ok);
    if (!ok || !n || strcmp(l->tok,"]")) return NULL;
    next(l); t=array_suffix(d,l,t,depth+1);
    if (!t || n>0x100000/t->size) return NULL;
    a=sim_ctx_alloc(d->ctx,sizeof *a); a->kind=SIM_T_ARRAY; a->target=t;
    a->count=(uint32_t)n; a->size=(uint32_t)n*t->size; return a;
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
                field=array_suffix(d,l,mt,0); count=1;
                if (!field) { free(members); return NULL; }
            }
            /* ポインタ・関数宣言子等はレコード全体を拒否する。 */
            if (!name[0] && strcmp(l->tok,":") &&
                !(mt->kind==SIM_T_STRUCT || mt->kind==SIM_T_UNION)) { free(members); return NULL; }
            if (!strcmp(l->tok,":")) {
                uint64_t n; bit=1; next(l); n=expression(d,l,0,&ok);
                if (!ok || n>mt->size*8 || (mt->kind!=SIM_T_INT && mt->kind!=SIM_T_UINT) || count || (!n && name[0])) { free(members); return NULL; }
                width=(unsigned)n;
            }
            { unsigned a=type_alignment(field,0); if (a>alignment) alignment=a;
              if (bit) {
                  if (!width) { size=align_up(size,a); unit_type=NULL; used=0; unit=0; }
                  else if (un) { off=0; unit=0; used=0; if (size<mt->size) size=mt->size; }
                  else if (unqualified_type(d,unit_type)!=unqualified_type(d,mt) || used+width>mt->size*8) { unit=align_up(size,a); size=unit+mt->size; used=0; unit_type=mt; }
                  off=unit;
              } else { unit_type=NULL; used=0; off=un?0:align_up(size,a);
                  if (off>0x100000 || field->size>0x100000-off) { free(members); return NULL; }
                  if (size<off+field->size) size=off+field->size; }
            }
            /* 名前のない詰め物も、親レコードの整列を復元するため型を残す。 */
            if (!bit || width || !name[0]) {
                members=sim_xrealloc(members,(nm+1)*sizeof *members);
                members[nm].name=owned_string(d,name); members[nm].type=field; members[nm].offset=off;
                members[nm].bit_offset=bit?(int32_t)(off*8+used):-1; members[nm].bit_size=width; nm++;
            }
            if (bit) used+=width;
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
static const sim_type *parse_type_impl(defs *d,lexer *l,unsigned depth)
{
    const sim_type *t;
    while (qualifier(l->tok)) next(l);
    if (!strcmp(l->tok,"struct") || !strcmp(l->tok,"union")) {
        int un=!strcmp(l->tok,"union"); char tag[128]=""; next(l);
        if (sim_is_ident_start((unsigned char)l->tok[0])) { snprintf(tag,sizeof tag,"%s",l->tok); next(l); }
        if (!strcmp(l->tok,"{")) { t=record_type(d,l,un,depth); return d->packing?NULL:t; }
        return tag[0]?resolve_tag(d,tag,un?SIM_T_UNION:SIM_T_STRUCT,depth+1):NULL;
    }
    {
        int uns=0,sgn=0,chr=0,lng=0,flt=0,dbl=0,shrt=0,ints=0,seen=0;
        for (;;) {
            if (qualifier(l->tok)) { next(l); continue; }
            if (!strcmp(l->tok,"unsigned")) uns++;
            else if (!strcmp(l->tok,"signed")) sgn++;
            else if (!strcmp(l->tok,"char")) chr++;
            else if (!strcmp(l->tok,"short")) shrt++;
            else if (!strcmp(l->tok,"int")) ints++;
            else if (!strcmp(l->tok,"long")) lng++;
            else if (!strcmp(l->tok,"float")) flt++;
            else if (!strcmp(l->tok,"double")) dbl++;
            else break;
            seen=1; next(l);
        }
        if (seen) {
            if (uns>1 || sgn>1 || uns+sgn>1 || chr>1 || shrt>1 || ints>1 || lng>2 ||
                flt+dbl>1 || (chr && (shrt || ints || lng || flt || dbl)) ||
                (shrt && (lng || flt || dbl)) || ((flt || dbl) && (uns || sgn || ints || lng))) return NULL;
            if (lng==2) return d->compiler==1?NULL:&basic[uns?7:8];
            return &basic[(flt || dbl)?6:chr?(uns || (!sgn && d->char_unsigned)?0:1):lng?(uns?4:5):(uns?2:3)];
        }
    }
    if (!strcmp(l->tok,"void") || !strcmp(l->tok,"_Bool") || !strcmp(l->tok,"bool") || !strcmp(l->tok,"enum")) { skip_spec(l); return NULL; }
    t=resolve_name(d,l->tok,depth+1); if (l->tok[0]) next(l);
    while (qualifier(l->tok)) next(l);
    return t;
}
/* 修飾付きの写しもコンテキストが所有し、参照の寿命中有効にする。 */
static int const_type(defs *d,const sim_type *t)
{
    (void)d;
    return t && t->is_const;
}
static const sim_type *unqualified_type(defs *d,const sim_type *t)
{
    size_t i;
    for (i=0;i<d->nconst_types;i++) if (d->const_types[i].qualified==t) return d->const_types[i].base;
    return t;
}
static const sim_type *parse_type(defs *d,lexer *l,unsigned depth)
{
    lexer begin=*l;
    const sim_type *t=parse_type_impl(d,l,depth);
    int ro=0, braces=0;
    while (begin.start<l->start && begin.tok[0]) {
        if (!strcmp(begin.tok,"{")) braces++;
        if (!braces && !strcmp(begin.tok,"const")) ro=1;
        if (!strcmp(begin.tok,"}")) braces--;
        next(&begin);
    }
    if (t && ro && !const_type(d,t)) {
        size_t i;
        sim_type *qualified;
        for (i=0;i<d->nconst_types;i++) if (d->const_types[i].base==t) return d->const_types[i].qualified;
        qualified=sim_ctx_alloc(d->ctx,sizeof *qualified); *qualified=*t; qualified->is_const=1;
        d->const_types=sim_xrealloc(d->const_types,(d->nconst_types+1)*sizeof *d->const_types);
        d->const_types[d->nconst_types].base=t;
        d->const_types[d->nconst_types++].qualified=qualified; t=qualified;
    }
    return t;
}
static const sim_type *resolve_tag(defs *d,const char *name,sim_kind kind,unsigned depth)
{
    definition *v=lookup(d,2,name);
    const sim_type *t;
    char full[160];
    if (depth>32) return NULL;
    if (v) {
        lexer l=lex(v->body);
        if (v->state==1 || v->state==3 || !strchr(v->body,'{')) return NULL;
        if (!v->state) {
            v->state=1; v->type=parse_type(d,&l,depth+1);
            v->state=v->type?2:3;
        }
        t=v->type;
    } else {
        snprintf(full,sizeof full,"%s %s",kind==SIM_T_UNION?"union":"struct",name);
        t=sim_ctx_named_type(d->ctx,full);
        /* デバッグ変数自身の修飾はタグの定義に含めない。メンバの修飾は残す。 */
        if (t && t->is_const) {
            sim_type *plain=sim_ctx_alloc(d->ctx,sizeof *plain);
            *plain=*t; plain->is_const=0;
            if (plain->name && !strncmp(plain->name,"const ",6)) plain->name+=6;
            t=plain;
        }
    }
    return t && t->kind==kind?t:NULL;
}
static const sim_type *resolve_name(defs *d,const char *name,unsigned depth)
{
    size_t i;
    static const char *stdnames[]={"uint8_t","int8_t","uint16_t","int16_t","uint32_t","int32_t",NULL};
    if (depth>32) return NULL;
    for (i=0;i<9;i++) if (!strcmp(name,basic[i].name)) return &basic[i];
    for (i=0;stdnames[i];i++) if (!strcmp(name,stdnames[i])) return &basic[i];
    { definition *v=lookup(d,1,name); if (v) {
        if (v->state==2) return v->type;
        if (v->state==1) return NULL;
        if (!v->state) {
        v->state=1;
        { lexer l=lex(v->body); const sim_type *t=parse_type(d,&l,depth+1); t=array_suffix(d,&l,t,0); v->type=(!l.tok[0] || !strcmp(l.tok,";"))?t:NULL; }
        v->state=v->type?2:3;
        if (v->type) return v->type;
        }
        return NULL;
    } }
    /* ここから先（ロードモジュールの同名の型と ctype）は全変数をたどるので、名前ごとに結果を覚える。
       マクロの本体の「(」ごとに型かどうかを試すため、型でない名前で何度も呼ばれる */
    {
        size_t slot; const sim_type *t;
        if (d->ext_count*2>=d->ext_cap) {
            size_t old=d->ext_cap, j; ext_type *prev=d->ext;
            d->ext_cap=old?old*2:256; d->ext=sim_xcalloc(d->ext_cap,sizeof *d->ext);
            for (j=0;j<old;j++) if (prev[j].name) {
                slot=name_hash(prev[j].name)&(d->ext_cap-1);
                while (d->ext[slot].name) slot=(slot+1)&(d->ext_cap-1);
                d->ext[slot]=prev[j];
            }
            free(prev);
        }
        slot=name_hash(name)&(d->ext_cap-1);
        while (d->ext[slot].name) { if (!strcmp(d->ext[slot].name,name)) return d->ext[slot].type; slot=(slot+1)&(d->ext_cap-1); }
        t=external_type(d,name,depth);
        /* external_type が再帰して表を伸ばしたかもしれないので、場所を探し直す */
        slot=name_hash(name)&(d->ext_cap-1);
        while (d->ext[slot].name) { if (!strcmp(d->ext[slot].name,name)) return t; slot=(slot+1)&(d->ext_cap-1); }
        d->ext[slot].name=sim_xstrdup(name); d->ext[slot].type=t; d->ext_count++;
        return t;
    }
}
static const sim_type *external_type(defs *d,const char *name,unsigned depth)
{
    size_t i;
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
static const sim_member *member(const sim_type *t,const char *name,unsigned depth,uint32_t *offset,int *readonly)
{
    int k;
    if (!t || depth>16) return NULL;
    for (k=0;k<t->nmembers;k++) if (!strcmp(t->members[k].name,name)) { *offset=0; *readonly|=const_type(NULL,t); return &t->members[k]; }
    for (k=0;k<t->nmembers;k++) if (!t->members[k].name[0]) {
        uint32_t off; const sim_member *m=member(t->members[k].type,name,depth+1,&off,readonly);
        if (m) { *offset=off+t->members[k].offset; *readonly|=const_type(NULL,t); return m; }
    }
    return NULL;
}
typedef struct {
    const sim_type *type;
    uint64_t addr, near_addr;
    int pointer, bit, readonly, reject_guess;
    unsigned bits, guessed;
} direct_ref;

/* A cast consumes exactly one address primary. In particular, a binary
   operator after a bare integer is never part of the cast operand. */
static int address_primary(defs *d,lexer *l,uint64_t *addr)
{
    int ok=1;
    if (!strcmp(l->tok,"(")) {
        next(l); *addr=expression(d,l,0,&ok);
        if (!ok || strcmp(l->tok,")")) return 0;
        next(l); return 1;
    }
    if (isdigit((unsigned char)l->tok[0]) || sim_is_ident_start((unsigned char)l->tok[0])) {
        sim_pp_env env={constant_lookup,d};
        if (sim_pp_eval_constant(l->tok,&env,addr)) return 0;
        next(l); return 1;
    }
    return 0;
}
/* Parse unary/postfix expressions with C precedence, retaining whether the
   result is a pointer or an lvalue. Other operators are deliberately absent. */
static int direct_expression(defs *d,lexer *l,direct_ref *r,unsigned depth,int *unresolved)
{
    if (depth>128) { *unresolved=1; return 0; }
    memset(r,0,sizeof *r); r->bit=-1;
    if (!strcmp(l->tok,"*")) {
        next(l);
        if (!direct_expression(d,l,r,depth+1,unresolved) || !r->pointer) return 0;
        r->pointer=0;
        /* The operand has already consumed its postfix operators. */
        return 1;
    }
    if (strcmp(l->tok,"(")) return 0;
    {
        lexer cast=*l, name;
        const sim_type *t;
        next(&cast); name=cast;
        t=sim_is_ident_start((unsigned char)cast.tok[0])?parse_type(d,&cast,0):NULL;
        if (t && !strcmp(cast.tok,"(")) { *unresolved=1; return 0; }
        if (sim_is_ident_start((unsigned char)name.tok[0]) && !strcmp(cast.tok,"*")) {
            next(&cast); while (qualifier(cast.tok)) next(&cast);
            if (strcmp(cast.tok,")")) { *unresolved=1; return 0; }
            next(&cast);
            if (!address_primary(d,&cast,&r->addr)) { *unresolved=1; return 0; }
            r->type=t; r->pointer=1; r->readonly=const_type(d,t);
            while (qualifier(name.tok)) next(&name);
            r->reject_guess=!strcmp(name.tok,"struct") || !strcmp(name.tok,"union") ||
                lookup(d,1,name.tok)!=NULL || builtin_word(name.tok) || !strcmp(name.tok,"enum");
            if (r->addr>0xFFFFF) { *unresolved=1; return 0; }
            if (r->addr<=0xFFFF && d->near_base) { r->addr+=d->near_base; r->near_addr=r->addr; }
            *l=cast;
        } else {
            next(l);
            if (!direct_expression(d,l,r,depth+1,unresolved)) return 0;
            if (strcmp(l->tok,")")) return 0;
            next(l);
        }
    }
    for (;;) {
        const sim_type *t=r->type;
        if (!strcmp(l->tok,"[")) {
            uint64_t n; int ok=1;
            next(l); n=expression(d,l,0,&ok);
            if (!ok || strcmp(l->tok,"]")) { *unresolved=1; return 0; }
            if (!r->pointer) {
                if (!t || t->kind!=SIM_T_ARRAY || n>=t->count) { *unresolved=1; return 0; }
                t=t->target;
            }
            if (r->bit>=0 || !t || !t->size || r->addr>0xFFFFF || n>(0xFFFFF-r->addr)/t->size) {
                *unresolved=1; return 0;
            }
            r->addr+=n*t->size; r->type=t; r->pointer=0;
            r->readonly|=const_type(d,t); next(l);
        } else if (!strcmp(l->tok,".") || !strcmp(l->tok,"->")) {
            const sim_member *m; uint32_t parent=0;
            int arrow=!strcmp(l->tok,"->");
            if (arrow!=r->pointer || r->bit>=0) return 0;
            next(l);
            if (!t || (t->name && (!strcmp(t->name,"bits8") || !strcmp(t->name,"bits16")))) {
                int guessed=t==NULL, bit=bit_number(l->tok,t?t->size*8:8);
                if ((guessed && (r->reject_guess || !d->guess)) || bit<0) { *unresolved=1; return 0; }
                r->bit=bit; r->bits=1; r->type=&basic[t && t->size==2?2:0];
                r->guessed+=guessed;
            } else {
                m=member(t,l->tok,0,&parent,&r->readonly);
                if (!m) { *unresolved=1; return 0; }
                r->addr+=parent+m->offset;
                if (m->bit_offset>=0) { r->bit=m->bit_offset-(int)(m->offset*8); r->bits=m->bit_size; }
                r->type=m->type; r->readonly|=const_type(d,m->type);
            }
            r->pointer=0; next(l);
        } else break;
    }
    return 1;
}
static void resolve_macro(defs *d,definition *v)
{
    lexer l=lex(v->body), scan=l;
    direct_ref r;
    int candidate=0, unresolved=0, parsed;
    /* Identify direct-address casts even when enclosed in a value expression.
       Unsupported pointer declarators remain type-unresolved. */
    while (scan.tok[0]) {
        if (!strcmp(scan.tok,"(")) {
            lexer cast=scan, spec; const sim_type *t;
            next(&cast); spec=cast;
            while (qualifier(spec.tok)) next(&spec);
            if (sim_is_ident_start((unsigned char)cast.tok[0])) {
                t=parse_type(d,&cast,0);
                if (!t && (!strcmp(spec.tok,"struct") || !strcmp(spec.tok,"union"))) candidate=1;
                if (!strcmp(cast.tok,"*")) {
                    next(&cast); while (qualifier(cast.tok)) next(&cast);
                    if (!strcmp(cast.tok,")") || t) candidate=1;
                } else if (t && !strcmp(cast.tok,"(")) candidate=1;
            }
        }
        next(&scan);
    }
    if (!candidate) return;
    parsed=direct_expression(d,&l,&r,0,&unresolved);
    if (!parsed || l.tok[0]) {
        if (unresolved || (parsed && !strcmp(l.tok,")"))) goto unknown;
        d->values++;
        if (d->verbose) fprintf(stderr,"defines: 値の式のマクロ（対象外）: %s (%s:%u)\n",v->name,v->file,v->line);
        return;
    }
    if (r.pointer) { d->pointers++; return; }
    if (!r.type || !r.type->size || r.addr>0xFFFFF || r.type->size>0x100000-r.addr) goto unknown;
    {
        int added=sim_ctx_define_at(d->ctx,v->name,r.type,r.addr,r.bit,r.bits,v->file,v->line,0);
        if (!added) {
            if (d->verbose) fprintf(stderr,"defines: %s はロードモジュールの同名シンボルを優先\n",v->name);
            return;
        }
        if (d->verbose && r.near_addr) fprintf(stderr,"defines: %s near -> 0x%05llX\n",v->name,(unsigned long long)r.near_addr);
        sim_ctx_definition_const(d->ctx,v->name,r.readonly);
    }
    d->guessed+=r.guessed;
    if (r.guessed && d->verbose) fprintf(stderr,"defines: %s をビット%dと推定\n",v->name,r.bit);
    if (r.bits) d->bits++; else if (r.type->kind==SIM_T_ARRAY) d->arrays++; else d->variables++;
    return;
unknown:
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
        int added=sim_ctx_define_at(d->ctx,name,t,addr,bit,bits,file,line,2);
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
    defs d; char *path; int rc,i,kind; size_t k; uint64_t t0=sim_now_ns(),t1; sim_buf info;
    sim_mtpj mtpj; char *mtpj_path=NULL; int have_mtpj=0;
    unsigned n_implicit=0,n_mtpj=0,n_cfg=0,n_cli=0,n_cundef=0;
    sim_buf scope_missing; unsigned scope_nmissing=0,scope_other=0,scope_skipped=0,scope_cfg_incs=0;
    memset(&d,0,sizeof d); d.ctx=c; d.err=err; d.cap=cap; d.verbose=o->verbose;
    d.env.lookup=lookup_cb; d.env.ctx=&d;
    memset(&mtpj,0,sizeof mtpj);
    sim_cfg_init(&d.cfg);
    sim_buf_init(&scope_missing);
    path=sim_cfg_locate(o->cfg_path,o->abs_path);
    rc=sim_cfg_load(&d.cfg,path,err,cap); free(path);
    if (rc==-2 || (rc==-1 && o->cfg_path)) { if (rc==-1) snprintf(err,cap,"設定ファイルを読めません"); sim_cfg_free(&d.cfg); return -1; }
    d.recursive=onoff(&d,"defines_recursive",1); d.guess=onoff(&d,"defines_guess_bits",1);
    d.char_unsigned=onoff(&d,"char_unsigned",0); d.near_base=0xF0000;
    d.cond=onoff(&d,"defines_cond",1);
    { const char *s=sim_cfg_get(&d.cfg,"defines_scope");
      if (s && !strcmp(s,"mtpj")) d.scope=1;
      else if (s && strcmp(s,"folder")) { snprintf(err,cap,"defines_scope = %s は読めません（folder: defines のフォルダの下すべて、mtpj: プロジェクトに登録されたファイルと #include でたどれるファイル）",s); d.error=1; } }
    { const char *s=sim_cfg_get(&d.cfg,"defines_near_base"); if (s && (sim_parse_uint(s,0xF0000,&d.near_base) || (d.near_base && d.near_base!=0xF0000))) { snprintf(err,cap,"defines_near_base は 0xF0000 / 0 です"); d.error=1; } }
    /* 定義マクロ: 暗黙 -> mtpj の定義・定義解除 -> sim.cfg の cdef -> --cdef -> cundef の順に適用する */
    kind=d.error?0:compiler_kind(&d,o); d.compiler=kind;
    if (!d.error) add_implicit(&d,kind);
    /* sim_native_pre.h が定義する目印。DLL のコンパイルとヘッダーの読み取りで、#if の枝をそろえる */
    if (!d.error && exec_native(&d,o)) add_cdef(&d,"__SIM_NATIVE__",ORIGIN_IMPLICIT,"暗黙の定義");
    if (!d.error) {
        const char *s=sim_cfg_get(&d.cfg,"mtpj"), *mode=sim_cfg_get(&d.cfg,"mtpj_mode");
        if (o->mtpj_path) mtpj_path=sim_xstrdup(o->mtpj_path);
        else if (s) mtpj_path=sim_cfg_path(&d.cfg,s);
        if (mtpj_path) {
            if (sim_mtpj_load(&mtpj,mtpj_path,mode,err,cap)) d.error=1;
            else {
                have_mtpj=1;
                for (i=0;i<mtpj.ndefs && !d.error;i++) add_cdef(&d,mtpj.defs[i],ORIGIN_MTPJ,"mtpj の定義マクロ");
                for (i=0;i<mtpj.nundefs && !d.error;i++) add_cundef(&d,mtpj.undefs[i],"mtpj の定義解除マクロ");
            }
        } else if (mode) { snprintf(err,cap,"mtpj_mode は mtpj = パス と一緒に指定してください"); d.error=1; }
        if (!d.error && d.scope && !have_mtpj) { snprintf(err,cap,"defines_scope = mtpj は mtpj = パス と一緒に指定してください"); d.error=1; }
    }
    for (i=0;i<d.cfg.n && !d.error;i++) if (!strcmp(d.cfg.e[i].key,"cdef")) add_cdef(&d,d.cfg.e[i].value,ORIGIN_CFG,"cdef");
    for (i=0;i<o->ncdefs && !d.error;i++) add_cdef(&d,o->cdefs[i],ORIGIN_CLI,"--cdef");
    for (i=0;i<d.cfg.n && !d.error;i++) if (!strcmp(d.cfg.e[i].key,"cundef")) { add_cundef(&d,d.cfg.e[i].value,"cundef"); n_cundef++; }
    for (k=1;k<d.nsyms;k++) if (d.syms[k].base_kind) switch (d.syms[k].origin) {
        case ORIGIN_IMPLICIT: n_implicit++; break; case ORIGIN_MTPJ: n_mtpj++; break;
        case ORIGIN_CFG: n_cfg++; break; case ORIGIN_CLI: n_cli++; break;
    }
    for (i=0;i<d.cfg.n && !d.error;i++) if (!strcmp(d.cfg.e[i].key,"defines")) collect_path(&d,d.cfg.e[i].value);
    for (i=0;i<o->ndefines && !d.error;i++) collect_path(&d,o->defines[i]);
    if (!d.error && d.scope) {
        /* 登録ファイルとインクルード・パスは mtpj のフォルダから見る */
        const char *mb=sim_basename(mtpj_path);
        char *mdir=mb>mtpj_path ? sim_xstrndup(mtpj_path,(size_t)(mb-mtpj_path)) : sim_xstrdup(".");
        for (i=0;i<mtpj.nfiles;i++) {
            char *joined, *full;
            size_t before=d.nfiles;
            if (!source_extension(mtpj.files[i])) { scope_other++; continue; }
            joined=absolute_path(mtpj.files[i]) ? sim_xstrdup(mtpj.files[i]) : sim_path_join(mdir,mtpj.files[i]);
            full=normal_path(joined); free(joined);
            if (sim_path_kind(full)==1) { add_file(&d,full); if (d.nfiles>before) d.scope_registered++; else scope_skipped++; }
            else { if (scope_nmissing<20 || d.verbose) sim_buf_printf(&scope_missing,"  見つからない登録ファイル: %s\n",full); scope_nmissing++; }
            free(full);
        }
        for (i=0;i<mtpj.nincs+d.cfg.n;i++) {
            char *joined, *full;
            if (i<mtpj.nincs) joined=absolute_path(mtpj.incs[i]) ? sim_xstrdup(mtpj.incs[i]) : sim_path_join(mdir,mtpj.incs[i]);
            else if (!strcmp(d.cfg.e[i-mtpj.nincs].key,"defines_include")) { joined=sim_cfg_path(&d.cfg,d.cfg.e[i-mtpj.nincs].value); scope_cfg_incs++; }
            else continue;
            full=normal_path(joined); free(joined);
            d.incdirs=sim_xrealloc(d.incdirs,(d.nincdirs+1)*sizeof *d.incdirs);
            d.incdirs[d.nincdirs++]=full;
        }
        free(mdir);
    } else if (!d.error && sim_cfg_get(&d.cfg,"defines_include")) { snprintf(err,cap,"defines_include は defines_scope = mtpj と一緒に指定してください"); d.error=1; }
    qsort(d.files,d.nfiles,sizeof *d.files,path_cmp);
    d.t_scan=sim_now_ns()-t0;
    if (!d.error) preprocess(&d);
    t1=sim_now_ns();
    for (k=0;k<d.nmacros && !d.error;k++) resolve_macro(&d,&d.macros[k]);
    for (i=0;i<d.cfg.n && !d.error;i++) {
        if (!strcmp(d.cfg.e[i].key,"var")) explicit_var(&d,d.cfg.e[i].value,d.cfg.path?d.cfg.path:"sim.cfg",(unsigned)d.cfg.e[i].line);
        if (!strcmp(d.cfg.e[i].key,"varfile")) var_file(&d,d.cfg.e[i].value);
    }
    sim_ctx_refresh_symbols(c);
    sim_buf_init(&info);
    if (!d.error && (d.nfiles || d.explicit_vars || have_mtpj || n_cfg || n_cli)) {
        size_t limit=d.verbose?(size_t)-1:20;
        sim_buf_adds(&info,"== ヘッダの定義\n");
        info_item(&info,"読んだファイル","%zu 個（%.2f 秒）",d.nfiles,(double)(sim_now_ns()-t0)/1e9);
        info_item(&info,"取り込んだ定義","変数 %u、ビット %u、配列 %u",d.variables,d.bits,d.arrays);
        info_item(&info,"取り込めなかった定義","型を解決できない %u、値の式のマクロ %u、ポインタ %u",d.unresolved,d.values,d.pointers);
        if (d.unresolved || d.values || d.pointers) sim_buf_adds(&info,"  ※ 取り込めなかった定義の名前と場所は、--verbose で表示します\n");
        info_item(&info,"同じ名前で内容が違う定義","%u 件",d.duplicates);
        if (d.duplicates) {
            /* 内訳の右に、その分類が何を意味するかを一言だけ添える（0 件の行には付けない） */
            info_item(&info,"  同じファイルの中","%u%s",d.dup_same_file,d.dup_same_file?"  （インクルード元によって違う枝を読むヘッダ）":"");
            info_item(&info,"  同じ名前で別のフォルダ","%u%s",d.dup_same_name,d.dup_same_name?"  （品種違いのファイルを両方読んでいる可能性）":"");
            info_item(&info,"  名前の違うファイル同士","%u",d.dup_other_name);
            sim_buf_adds(&info,"  ※ 同じ名前で内容が違う定義は、最初に読んだものを使います。CSV や GUI で使う名前なら、--verbose で場所を確かめてください\n");
        }
        if (d.explicit_vars) info_item(&info,"明示した定義（var）","%u 個",d.explicit_vars);
        {
            unsigned bad=0;
            for (k=0;k<d.nsrc;k++) if (d.src[k].bad_line) bad++;
            if (d.enc_utf16) info_item(&info,"UTF-16 で読めないファイル","%u 個",d.enc_utf16);
            if (bad) info_item(&info,"構文を追えないファイル","%u 個",bad);
            if (d.verbose) for (k=0;k<d.nsrc;k++) if (d.src[k].bad_line)
                sim_buf_printf(&info,"  構文を追えなかったファイル: %s:%u\n",d.src[k].path,d.src[k].bad_line);
        }
        if (d.scope) {
            size_t j;
            sim_buf_adds(&info,"\n== 読む範囲（defines_scope = mtpj）\n");
            info_item(&info,"プロジェクト","%s（ビルド・モード %s）",mtpj_path,mtpj.modes[mtpj.mode]);
            info_item(&info,"登録ファイル","%d 個（読んだ %u、.h / .c / .inc 以外 %u、ビルド対象外 %d、除外 %u、見つからない %u）",
                mtpj.files_total,d.scope_registered,scope_other,mtpj.files_off,scope_skipped,scope_nmissing);
            sim_buf_adds(&info,sim_buf_cstr(&scope_missing));
            if (scope_nmissing>20 && !d.verbose) sim_buf_printf(&info,"  …ほか %u 個（--verbose で全部を表示）\n",scope_nmissing-20);
            info_item(&info,"#include から足した","%u 個",d.scope_added);
            info_item(&info,"インクルード・パス","%zu 個（mtpj %d、defines_include %u）",d.nincdirs,mtpj.nincs,scope_cfg_incs);
            if (d.verbose || !mtpj.inc_element) sim_buf_printf(&info,"  mtpj の要素: <%s>\n",mtpj.inc_element?mtpj.inc_element:"AdditionalIncludePaths（要素なし）");
            for (j=0;j<d.nincdirs;j++) {
                int exists=sim_path_kind(d.incdirs[j])==2;
                if (!exists || d.verbose) sim_buf_printf(&info,"  %s%s\n",d.incdirs[j],exists?"":"（フォルダがありません）");
            }
            if (d.scope_fallback) {
                info_item(&info,"名前で探した #include","%u 箇所",d.scope_fallback);
                sim_buf_adds(&info,"  ※ 名前で探した #include は、インクルード・パスの順では見つからなかったものです。パスが足りなければ defines_include で足せます\n");
            }
            sim_buf_adds(&info,"  ※ ビルド対象の判定とインクルード・パスの書式は実例からの推定です。CS+ の画面と見比べてください\n");
        }
        sim_buf_adds(&info,"\n== コンパイルスイッチ\n");
        if (have_mtpj && !d.scope) info_item(&info,"プロジェクト","%s（ビルド・モード %s）",mtpj_path,mtpj.modes[mtpj.mode]);
        info_item(&info,"定義マクロ","mtpj %u 個、cdef %u 個、--cdef %u 個、暗黙 %u 個",n_mtpj,n_cfg,n_cli,n_implicit);
        if (d.verbose) {
            sim_buf_printf(&info,"  暗黙の定義の対象: %s%s\n",
                kind==1?"CA78K0R":kind==2?"CC-RL":"ロードモジュールの種類が不明のため定義なし",n_implicit?"":"（なし）");
            sim_buf_adds(&info,"  ※ 暗黙の定義マクロには、品種・メモリモデル・オプションで変わるものを含めません\n");
            list_origin(&d,&info,ORIGIN_IMPLICIT,"  implicit: ");
        }
        if (have_mtpj) {
            sim_buf_printf(&info,"  ビルド・モード %s（%d 個中 %d 番目）\n",mtpj.modes[mtpj.mode],mtpj.nmodes,mtpj.mode+1);
            sim_buf_adds(&info,"  全モード: ");
            for (i=0;i<mtpj.nmodes;i++) sim_buf_printf(&info,"%s%s",i?", ":"",mtpj.modes[i]);
            sim_buf_addc(&info,'\n');
            if (!mtpj.def_element) {
                sim_buf_printf(&info,"  ※ mtpj に定義マクロの要素 <MacroDefinitions-%d> が見つかりません\n",mtpj.mode);
                sim_buf_adds(&info,"  ※ mtpj の定義マクロは読んでいません（CA78K0R 以外のプロジェクトか、未対応の版）\n");
            } else {
                sim_buf_printf(&info,"  mtpj: 定義マクロは要素 <%s> から読みました\n",mtpj.def_element);
                if (!mtpj.compiler_instance) sim_buf_adds(&info,"  ※ mtpj でコンパイラのオプションの Instance を特定できず、最初に現れた定義マクロを使いました\n");
                sim_buf_adds(&info,"  ※ mtpj の定義マクロの書式（1 行に 名前 または 名前=値）は実例からの推定です。CS+ の「定義マクロ」と見比べてください\n");
                sim_buf_printf(&info,"  mtpj macro definitions (%d):",mtpj.ndefs);
                for (i=0;i<mtpj.ndefs;i++) sim_buf_printf(&info,"%s %s",i?",":"",mtpj.defs[i]);
                sim_buf_printf(&info,"%s\n  mtpj macro undefinitions (%d):",mtpj.ndefs?"":" なし",mtpj.nundefs);
                for (i=0;i<mtpj.nundefs;i++) sim_buf_printf(&info,"%s %s",i?",":"",mtpj.undefs[i]);
                sim_buf_printf(&info,"%s\n",mtpj.nundefs?"":" なし");
            }
            for (i=0;i<mtpj.nbad;i++) sim_buf_printf(&info,"  mtpj: 名前として読めない行を無視しました: %s\n",mtpj.bad[i]);
            if (mtpj.individual) {
                sim_buf_printf(&info,"  個別コンパイル・オプションらしい定義マクロ: %d 件\n",mtpj.individual);
                sim_buf_adds(&info,"  ※ ファイルごとの個別コンパイル・オプションの定義マクロは読んでいません。必要なら cdef で与えてください\n");
            }
        }
        list_origin(&d,&info,ORIGIN_MTPJ,"  有効な定義マクロ（mtpj）: ");
        list_origin(&d,&info,ORIGIN_CFG,"  有効な定義マクロ（cdef）: ");
        list_origin(&d,&info,ORIGIN_CLI,"  有効な定義マクロ（--cdef）: ");
        if (n_cundef) {
            sim_buf_adds(&info,"  cundef:");
            for (i=0,rc=0;i<d.cfg.n;i++) if (!strcmp(d.cfg.e[i].key,"cundef")) sim_buf_printf(&info,"%s %s",rc++?",":"",d.cfg.e[i].value);
            sim_buf_addc(&info,'\n');
        }
        if (d.nfiles) {
            if (d.cond) {
                info_item(&info,"評価した #if","%u 箇所（判定不能 %u）",d.if_evaluated,d.if_indeterminate);
                if (d.if_indeterminate) sim_buf_adds(&info,"  ※ 判定不能の #if の系列は、すべての枝を読みます\n");
                {
                for (k=0;k<d.nsrc;k++) { size_t j; for (j=0;j<d.src[k].ndirs;j++) if (d.src[k].dirs[j].indeterminate) {
                    const directive *x=&d.src[k].dirs[j];
                    sim_buf_printf(&info,"  判定不能: %s:%u: #%s %s\n",d.src[k].path,d.src[k].line_no[x->line],
                        x->kind==D_ELIF?"elif":x->kind==D_IF?"if":x->kind==D_IFDEF?"ifdef":"ifndef",skip_blank(x->arg));
                } }
                }
            } else info_item(&info,"defines_cond = off","#if を評価せず、すべての枝を読みます");
            if (d.include_cut) info_item(&info,"打ち切った #include","%u 回（循環、または深さの上限）",d.include_cut);
            if (d.nmissing) {
                unsigned total=0;
                for (k=0;k<d.nmissing;k++) total+=d.missing[k].count;
                qsort(d.missing,d.nmissing,sizeof *d.missing,missing_cmp);
                info_item(&info,"見つからない #include","%u 箇所、%zu 種類（有効な枝のみ。無効な枝を含めると %u 箇所）",total,d.nmissing,d.include_missing);
                sim_buf_adds(&info,"  ※ 見つからない #include は、標準ヘッダなら問題ありません。プロジェクトのヘッダなら、インクルード・パスを確かめてください\n");
                for (k=0;k<d.nmissing && k<limit;k++)
                    sim_buf_printf(&info,"  %s × %u（最初: %s:%u）\n",d.missing[k].name,d.missing[k].count,d.missing[k].file,d.missing[k].line);
                if (k<d.nmissing) sim_buf_printf(&info,"  …ほか %zu 種類（--verbose で全部を表示）\n",d.nmissing-k);
            } else if (d.include_missing) info_item(&info,"見つからない #include","0 箇所（有効な枝のみ。無効な枝を含めると %u 箇所）",d.include_missing);
            if (d.cond) {
                list_undefined(&d,&info,0,limit);
                if (d.verbose) list_undefined(&d,&info,1,limit);
            }
        }
        sim_buf_adds(&info,"\n== 詳細（数値）\n");
        sim_buf_printf(&info,"defines files=%zu variables=%u bits=%u arrays=%u guessed=%u unresolved=%u duplicates=%u pointer-skipped=%u value-excluded=%u (%.3f s)\n",d.nfiles,d.variables,d.bits,d.arrays,d.guessed,d.unresolved,d.duplicates,d.pointers,d.values,(double)(sim_now_ns()-t0)/1e9);
        if (d.nfiles) {
            sim_buf_printf(&info,"defines time: scan=%.3f s read=%.3f s cond=%.3f s resolve=%.3f s (%.1f MB, %zu directives)\n",
                (double)d.t_scan/1e9,(double)d.t_read/1e9,(double)d.t_cond/1e9,(double)(sim_now_ns()-t1)/1e9,(double)d.bytes/1048576.0,d.ndirectives);
            sim_buf_printf(&info,"defines cond=%s roots=%u includes=%u include-reused=%u include-missing=%u include-cut=%u if-evaluated=%u if-indeterminate=%u\n",
                d.cond?"on":"off",d.roots,d.includes,d.memo_hits,d.include_missing,d.include_cut,d.if_evaluated,d.if_indeterminate);
        }
        sim_buf_printf(&info,"cdef implicit=%u mtpj=%u cfg=%u cli=%u cundef=%u\n",n_implicit,n_mtpj,n_cfg,n_cli,n_cundef);
        sim_buf_printf(&info,"var definitions=%u\n",d.explicit_vars);
        {   /* 上の「ヘッダの定義」では 0 件のとき省くので、件数はここに必ず出す（テストがこの 2 行を探す） */
            unsigned bad=0;
            for (k=0;k<d.nsrc;k++) if (d.src[k].bad_line) bad++;
            sim_buf_printf(&info,"UTF-16 のため読まなかったファイル: %u 件\n構文を追えなかったファイル: %u 件\n",d.enc_utf16,bad);
        }
        if (d.verbose) sim_buf_printf(&info,"defines encoding: UTF8=%u ShiftJIS=%u ASCII=%u\n",d.enc_utf8,d.enc_sjis,d.enc_ascii);
        info_notes_last(&info);
        sim_ctx_set_definition_info(c,sim_buf_cstr(&info));
        if (d.verbose) fputs(sim_buf_cstr(&info),stderr);
    }
    sim_buf_free(&info); sim_buf_free(&scope_missing);
    if (have_mtpj) sim_mtpj_free(&mtpj);
    free(mtpj_path);
    free_sources(&d);
    for (k=0;k<d.nfiles;k++) free(d.files[k]);
    free(d.files);
    free(d.const_types);
    free_definitions(d.macros,d.nmacros); free_definitions(d.types,d.ntypes); sim_cfg_free(&d.cfg);
    free(d.macro_index); free(d.type_index);
    free_definitions(d.tags,d.ntags); free(d.tag_index);
    return d.error?-1:0;
}
