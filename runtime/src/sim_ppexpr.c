/* sim_ppexpr.c - #if / #elif の条件式の評価。仕様は sim_ppexpr.h */
#include "sim_ppexpr.h"

#include <string.h>

#define MAX_TOKENS 1024
#define MAX_EXPAND 32   /* マクロ展開の入れ子 */
#define MAX_NEST 128    /* 括弧・単項・?: の入れ子 */

enum { T_END, T_NUM, T_OP, T_BAD };
enum {
    OP_LPAREN = 1, OP_RPAREN, OP_NOT, OP_COMPL, OP_MINUS, OP_PLUS, OP_MUL, OP_DIV, OP_MOD,
    OP_SHL, OP_SHR, OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE, OP_AND, OP_XOR, OP_OR,
    OP_LAND, OP_LOR, OP_QUEST, OP_COLON
};

typedef struct { int type, op; const char *s; size_t n; } token;

typedef struct {
    const sim_pp_env *env;
    token tok[MAX_TOKENS + 1];
    size_t ntok, pos;
    int prefix;
    unsigned parens;
    const char *end;
    int strict;
    unsigned steps;
    int error;                      /* 構文の誤り・上限超過: 式全体が判定不能 */
    const char *hide[MAX_EXPAND];   /* 展開中のマクロ名（自己参照を展開しない） */
    size_t hide_len[MAX_EXPAND];
    int bits;
    unsigned nest;
} pp;

typedef struct { uint64_t v; int uns, bad; } val;

static int ident_start(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int digit(int c) { return c >= '0' && c <= '9'; }
static int ident_char(int c) { return ident_start(c) || digit(c); }
static int space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

static void push(pp *p, int type, int op, const char *s, size_t n)
{
    if (p->ntok >= MAX_TOKENS) { p->error = 1; return; }
    p->tok[p->ntok].type = type; p->tok[p->ntok].op = op;
    p->tok[p->ntok].s = s; p->tok[p->ntok].n = n; p->ntok++;
}

static const char *skip_space(const char *s) { while (space((unsigned char)*s)) s++; return s; }

/* text を字句に分け、マクロを展開して p->tok に足す */
static void expand(pp *p, const char *s, unsigned depth)
{
    static const struct { const char *text; int op; } two[] = {
        {"<<", OP_SHL}, {">>", OP_SHR}, {"<=", OP_LE}, {">=", OP_GE}, {"==", OP_EQ}, {"!=", OP_NE},
        {"&&", OP_LAND}, {"||", OP_LOR}
    };
    static const char one[] = "()!~-+*/%<>&^|?:";
    static const int one_op[] = {
        OP_LPAREN, OP_RPAREN, OP_NOT, OP_COMPL, OP_MINUS, OP_PLUS, OP_MUL, OP_DIV, OP_MOD,
        OP_LT, OP_GT, OP_AND, OP_XOR, OP_OR, OP_QUEST, OP_COLON
    };
    while (!p->error) {
        const char *b;
        size_t n, k;
        if (++p->steps > 16384) { p->error = 1; return; }
        s = skip_space(s);
        if (!*s) { if (!depth) p->end=s; return; }
        if (p->prefix && !p->parens && strchr(")];,}",*s)) {
            if (depth) p->error=1;
            else p->end=s;
            return;
        }
        b = s;
        if (ident_start((unsigned char)*s)) {
            const char *body = NULL;
            int kind;
            while (ident_char((unsigned char)*s)) s++;
            n = (size_t)(s - b);
            if (!p->strict && n == 7 && !memcmp(b, "defined", 7)) {
                int paren = 0;
                s = skip_space(s);
                if (*s == '(') { paren = 1; s = skip_space(s + 1); }
                if (!ident_start((unsigned char)*s)) { p->error = 1; return; }
                b = s;
                while (ident_char((unsigned char)*s)) s++;
                n = (size_t)(s - b);
                if (paren) { s = skip_space(s); if (*s != ')') { p->error = 1; return; } s++; }
                kind = p->env && p->env->lookup ? p->env->lookup(p->env->ctx, b, n, &body, 1) : SIM_PP_UNDEF;
                push(p, T_NUM, 0, kind == SIM_PP_UNDEF ? "0" : "1", 1);
                continue;
            }
            for (k = 0; k < depth; k++)
                if (p->hide_len[k] == n && !memcmp(p->hide[k], b, n)) break;
            if (k < depth && p->strict) { p->error = 1; return; }
            if (k < depth) { push(p, p->strict ? T_BAD : T_NUM, 0, "0", 1); continue; }   /* 自己参照は識別子のまま = 0 */
            kind = p->env && p->env->lookup ? p->env->lookup(p->env->ctx, b, n, &body, 0) : SIM_PP_UNDEF;
            if (p->strict && kind != SIM_PP_OBJECT) { p->error = 1; return; }
            if (kind == SIM_PP_FUNCTION) {
                const char *q = skip_space(s);
                if (*q == '(') {
                    /* 関数形式マクロの呼び出し: 値は判定不能。引数を読み飛ばす */
                    int level = 0;
                    for (; *q; q++) {
                        if (*q == '(') level++;
                        else if (*q == ')' && --level == 0) { q++; break; }
                    }
                    if (level) { p->error = 1; return; }
                    s = q;
                    push(p, T_BAD, 0, b, n);
                } else push(p, T_NUM, 0, "0", 1);
            } else if (kind == SIM_PP_OBJECT && body) {
                if (depth >= MAX_EXPAND) { p->error=1; return; }
                p->hide[depth] = b; p->hide_len[depth] = n;
                expand(p, body, depth + 1);
            } else push(p, T_NUM, 0, "0", 1);
            continue;
        }
        if (digit((unsigned char)*s) || (*s == '.' && digit((unsigned char)s[1]))) {
            int real = 0;
            /* 1e+5 の符号は取り込まない（0x1E+2 と区別しない）。"1e" は数として読めず判定不能になる */
            while (ident_char((unsigned char)*s) || *s == '.') {
                if (*s == '.') real = 1;
                s++;
            }
            push(p, real ? T_BAD : T_NUM, 0, b, (size_t)(s - b));
            continue;
        }
        if (*s == '\'' || *s == '"') {
            char q = *s++;
            while (*s && *s != q) { if (*s == '\\' && s[1]) s++; s++; }
            if (!*s) { p->error = 1; return; }
            s++;
            push(p, q == '"' ? T_BAD : T_NUM, 0, b, (size_t)(s - b));
            continue;
        }
        for (k = 0; k < sizeof two / sizeof two[0]; k++)
            if (s[0] == two[k].text[0] && s[1] == two[k].text[1]) break;
        if (k < sizeof two / sizeof two[0]) { push(p, T_OP, two[k].op, b, 2); s += 2; continue; }
        {
            const char *o = strchr(one, *s);
            if (!o) { p->error = 1; return; }   /* = , # や非 ASCII など */
            if (p->prefix && *s=='(') p->parens++;
            if (p->prefix && *s==')') {
                if (!p->parens) { p->error=1; return; }
                p->parens--;
            }
            push(p, T_OP, one_op[o - one], b, 1); s++;
        }
    }
}

/* ---------------------------------------------------------------- 値 */

static uint64_t umax(const pp *p) { return p->bits == 32 ? UINT64_C(0xFFFFFFFF) : UINT64_MAX; }
static int64_t smax(const pp *p) { return p->bits == 32 ? INT64_C(0x7FFFFFFF) : INT64_MAX; }
static int64_t smin(const pp *p) { return p->bits == 32 ? -INT64_C(0x80000000) : INT64_MIN; }

/* 幅に切り詰める。符号付きは 64 ビットへ符号拡張した形で持つ */
static uint64_t wrap(const pp *p, uint64_t v, int uns)
{
    if (p->bits == 64) return v;
    v &= UINT64_C(0xFFFFFFFF);
    if (!uns && (v & UINT64_C(0x80000000))) v |= UINT64_C(0xFFFFFFFF00000000);
    return v;
}
static val make(const pp *p, uint64_t v, int uns) { val r; r.v = wrap(p, v, uns); r.uns = uns; r.bad = 0; return r; }
static val bad_val(void) { val r; r.v = 0; r.uns = 0; r.bad = 1; return r; }

static val number(const pp *p, const char *s, size_t n)
{
    uint64_t v = 0;
    unsigned base = 10;
    size_t i = 0, digits = 0;
    int uns = 0, nu = 0, nl = 0;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (n >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; i = 2; }
    else if (n >= 2 && s[0] == '0') { base = 8; i = 1; digits = 1; }
    for (; i < n; i++, digits++) {
        unsigned c = (unsigned char)s[i], dv;
        if (c >= '0' && c <= '9') dv = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') dv = c - 'A' + 10;
        else break;
        if (dv >= base) return bad_val();
        if (v > (UINT64_MAX - dv) / base) return bad_val();
        v = v * base + dv;
    }
    if (!digits) return bad_val();
    for (; i < n; i++) {
        if (s[i] == 'u' || s[i] == 'U') nu++;
        else if (s[i] == 'l' || s[i] == 'L') nl++;
        else return bad_val();
    }
    if (nu > 1 || nl > 2) return bad_val();
    if (v > umax(p)) return bad_val();
    uns = nu || v > (uint64_t)smax(p);
    return make(p, v, uns);
}

static val character(const pp *p, const char *s, size_t n)
{
    unsigned c;
    if (n < 3) return bad_val();
    s++; n -= 2;    /* 引用符を除く */
    if (s[0] != '\\') { if (n != 1) return bad_val(); c = (unsigned char)s[0]; }
    else if (n == 2) {
        switch (s[1]) {
        case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
        case 'a': c = '\a'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
        case 'v': c = '\v'; break; case '\\': c = '\\'; break; case '\'': c = '\''; break;
        case '"': c = '"'; break; case '?': c = '?'; break;
        default: if (s[1] >= '0' && s[1] <= '7') { c = (unsigned)(s[1] - '0'); break; } return bad_val();
        }
    } else {
        size_t i = 1;
        unsigned base = 8;
        c = 0;
        if (s[1] == 'x') { base = 16; i = 2; if (n == 2) return bad_val(); }
        for (; i < n; i++) {
            unsigned ch = (unsigned char)s[i], dv;
            if (ch >= '0' && ch <= '9') dv = ch - '0';
            else if (base == 16 && ch >= 'a' && ch <= 'f') dv = ch - 'a' + 10;
            else if (base == 16 && ch >= 'A' && ch <= 'F') dv = ch - 'A' + 10;
            else return bad_val();
            if (dv >= base) return bad_val();
            c = c * base + dv;
            if (c > 0xFF) return bad_val();
        }
    }
    if (c > 0x7F) return bad_val();   /* char の符号で値が変わる */
    return make(p, c, 0);
}

static int mul_overflows(int64_t a, int64_t b, int64_t lo, int64_t hi)
{
    if (!a || !b) return 0;
    if (a > 0) return b > 0 ? a > hi / b : b < lo / a;
    return b > 0 ? a < lo / b : b < hi / a;
}

static val binary(const pp *p, int op, val a, val b)
{
    int uns = a.uns || b.uns;
    int64_t sa = (int64_t)a.v, sb = (int64_t)b.v, lo = smin(p), hi = smax(p);
    uint64_t ua = a.v & umax(p), ub = b.v & umax(p);
    if (a.bad || b.bad) return bad_val();
    switch (op) {
    case OP_MUL:
        if (uns) return make(p, ua * ub, 1);
        if (mul_overflows(sa, sb, lo, hi)) return bad_val();
        return make(p, (uint64_t)(sa * sb), 0);
    case OP_DIV: case OP_MOD:
        if (!ub) return bad_val();
        if (uns) return make(p, op == OP_DIV ? ua / ub : ua % ub, 1);
        if (sa == lo && sb == -1) return bad_val();
        return make(p, (uint64_t)(op == OP_DIV ? sa / sb : sa % sb), 0);
    case OP_PLUS:
        if (uns) return make(p, ua + ub, 1);
        if ((sb > 0 && sa > hi - sb) || (sb < 0 && sa < lo - sb)) return bad_val();
        return make(p, (uint64_t)(sa + sb), 0);
    case OP_MINUS:
        if (uns) return make(p, ua - ub, 1);
        if ((sb < 0 && sa > hi + sb) || (sb > 0 && sa < lo + sb)) return bad_val();
        return make(p, (uint64_t)(sa - sb), 0);
    case OP_SHL: case OP_SHR:
        /* 結果の型は左辺の型。回数が負・幅以上は未定義 */
        if ((!b.uns && sb < 0) || ub >= (uint64_t)p->bits) return bad_val();
        if (op == OP_SHL) return make(p, ua << ub, a.uns);   /* 幅で切り捨てる */
        if (a.uns) return make(p, ua >> ub, 1);
        return make(p, (uint64_t)(sa < 0 ? ~(~(uint64_t)sa >> ub) : (uint64_t)sa >> ub), 0);   /* 算術シフト */
    case OP_LT: return make(p, uns ? ua < ub : sa < sb, 0);
    case OP_LE: return make(p, uns ? ua <= ub : sa <= sb, 0);
    case OP_GT: return make(p, uns ? ua > ub : sa > sb, 0);
    case OP_GE: return make(p, uns ? ua >= ub : sa >= sb, 0);
    case OP_EQ: return make(p, ua == ub, 0);
    case OP_NE: return make(p, ua != ub, 0);
    case OP_AND: return make(p, ua & ub, uns);
    case OP_XOR: return make(p, ua ^ ub, uns);
    case OP_OR: return make(p, ua | ub, uns);
    }
    return bad_val();
}

/* ---------------------------------------------------------------- 構文 */

static val conditional(pp *p);

static const token *peek(pp *p) { return &p->tok[p->pos]; }
static int accept(pp *p, int op)
{
    if (p->tok[p->pos].type == T_OP && p->tok[p->pos].op == op) { p->pos++; return 1; }
    return 0;
}

static val unary(pp *p)
{
    const token *t = peek(p);
    val v;
    if (p->error) return bad_val();
    if (++p->nest > MAX_NEST) { p->error = 1; return bad_val(); }
    if (t->type == T_NUM) {
        p->pos++;
        v = t->s[0] == '\'' ? character(p, t->s, t->n) : number(p, t->s, t->n);
    } else if (t->type == T_BAD) {
        p->pos++; v = bad_val();
    } else if (t->type == T_OP && t->op == OP_LPAREN) {
        p->pos++;
        v = conditional(p);
        if (!accept(p, OP_RPAREN)) p->error = 1;
    } else if (t->type == T_OP && (t->op == OP_NOT || t->op == OP_COMPL || t->op == OP_MINUS || t->op == OP_PLUS)) {
        int op = t->op;
        p->pos++;
        v = unary(p);
        if (!v.bad) {
            if (op == OP_NOT) v = make(p, !(v.v & umax(p)), 0);
            else if (op == OP_COMPL) v = make(p, ~v.v, v.uns);
            else if (op == OP_MINUS) {
                if (!v.uns && (int64_t)v.v == smin(p)) v = bad_val();
                else v = make(p, 0 - v.v, v.uns);
            }
        }
    } else { p->error = 1; v = bad_val(); }
    p->nest--;
    return v;
}

static int precedence(const token *t)
{
    if (t->type != T_OP) return 0;
    switch (t->op) {
    case OP_MUL: case OP_DIV: case OP_MOD: return 10;
    case OP_PLUS: case OP_MINUS: return 9;
    case OP_SHL: case OP_SHR: return 8;
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: return 7;
    case OP_EQ: case OP_NE: return 6;
    case OP_AND: return 5;
    case OP_XOR: return 4;
    case OP_OR: return 3;
    case OP_LAND: return 2;
    case OP_LOR: return 1;
    }
    return 0;
}

static val binary_expr(pp *p, int min)
{
    val a = unary(p);
    for (;;) {
        const token *t = peek(p);
        int prec = precedence(t), op = t->op;
        val b;
        if (!prec || prec < min || p->error) return a;
        p->pos++;
        b = binary_expr(p, prec + 1);
        if (op == OP_LAND) {
            /* 左が偽なら右は評価されない（右の判定不能は無視する） */
            if (a.bad) a = bad_val();
            else if (!(a.v & umax(p))) a = make(p, 0, 0);
            else a = b.bad ? bad_val() : make(p, (b.v & umax(p)) != 0, 0);
        } else if (op == OP_LOR) {
            if (a.bad) a = bad_val();
            else if (a.v & umax(p)) a = make(p, 1, 0);
            else a = b.bad ? bad_val() : make(p, (b.v & umax(p)) != 0, 0);
        } else a = binary(p, op, a, b);
    }
}

static val conditional(pp *p)
{
    val c, a, b;
    if (++p->nest > MAX_NEST) { p->error = 1; return bad_val(); }
    c = binary_expr(p, 1);
    if (!p->error && accept(p, OP_QUEST)) {
        int uns;
        a = conditional(p);
        if (!accept(p, OP_COLON)) p->error = 1;
        b = conditional(p);
        uns = a.uns || b.uns;
        if (c.bad) c = bad_val();
        else {
            val r = (c.v & umax(p)) ? a : b;    /* 選ばれない側の判定不能は無視する */
            c = r.bad ? bad_val() : make(p, r.v, uns);
        }
    }
    p->nest--;
    return c;
}

static int parse(pp *p, int bits, val *out)
{
    p->bits = bits; p->pos = 0; p->nest = 0;
    *out = conditional(p);
    if (p->error || p->tok[p->pos].type != T_END) return -1;
    return out->bad ? -1 : 0;
}

static int prepare(pp *p, const char *expr, const sim_pp_env *env, int strict, int prefix)
{
    p->env = env; p->ntok = 0; p->error = 0; p->strict = strict; p->steps = 0;
    p->prefix=prefix; p->parens=0; p->end=NULL;
    expand(p, expr, 0);
    if (p->error || !p->ntok) return -1;
    p->tok[p->ntok].type = T_END; p->tok[p->ntok].op = 0;
    return 0;
}

int sim_pp_eval_width(const char *expr, const sim_pp_env *env, int bits, sim_pp_value *out)
{
    pp p;
    val v;
    if (bits != 32 && bits != 64) return -1;
    if (prepare(&p, expr, env, 0, 0) || parse(&p, bits, &v)) return -1;
    if (out) { out->value = (int64_t)v.v; out->is_unsigned = v.uns; }
    return 0;
}

int sim_pp_eval(const char *expr, const sim_pp_env *env)
{
    pp p;
    val a, b;
    if (prepare(&p, expr, env, 0, 0)) return -1;
    if (parse(&p, 32, &a) || parse(&p, 64, &b)) return -1;
    if (!(a.v & UINT64_C(0xFFFFFFFF)) != !b.v) return -1;   /* 幅で真偽が変わる式 */
    return b.v != 0;
}

/* Direct-address/type expressions must never turn an unknown identifier into zero. */
int sim_pp_eval_constant(const char *expr, const sim_pp_env *env, uint64_t *out)
{
    pp p;
    val v;
    if (prepare(&p, expr, env, 1, 0) || parse(&p, 64, &v) || (!v.uns && (int64_t)v.v < 0)) return -1;
    *out = v.v;
    return 0;
}

int sim_pp_eval_constant_prefix(const char *expr, const sim_pp_env *env, uint64_t *out, const char **end)
{
    pp p;
    val v;
    if (prepare(&p, expr, env, 1, 1) || parse(&p, 64, &v) ||
        !p.end || (!v.uns && (int64_t)v.v < 0)) return -1;
    *out=v.v; *end=p.end;
    return 0;
}
