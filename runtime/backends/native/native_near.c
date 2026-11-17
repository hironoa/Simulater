/* native_near.c - LLVM IR の inttoptr を、RL78 の near の番地（16 ビット + 0xF0000）として読み替える
 *
 * RL78 の near のデータのポインタは 16 ビットで、実際の番地は 0xF0000 を補ったもの。製品のソースが番地を
 * 16 ビットに切り詰めてからポインタにすると、PC では 0x10000 より下（Windows が使わせない範囲）を指す。
 * そこで、整数からポインタへの変換をすべて捕まえて、値 v が 1〜0xFFFF なら v + 0xF0000 にする。
 * 0（NULL）と 0x10000 以上（5 桁で書いた番地や、PC の本物のポインタ）は変えない。
 *
 * 形は 2 つ（LLVM IR は 1 命令 1 行。長い定数式は 1 行の中に入れ子で現れる）:
 *   定数の形  inttoptr (iN V to ptr)          どこに現れても、V を書き換える（型は i64 にする）
 *   命令の形  %x = inttoptr iN %v to ptr       %v を i64 に広げて @__sim_near を通してからポインタにする
 * 文字列（c"…"、!"…"、"…"）とコメント（; 以降）の中は見ない。知らない形は書き換えずに数える。
 */
#include "native_near.h"
#include <string.h>

#define NEAR_MAX 0xFFFFull

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '$' || c == '-';
}
static int blank(char c) { return c == ' ' || c == '\t'; }
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && blank(*p)) p++;
    return p;
}
/* p から語 w が続き、その後ろが名前の文字でなければ、語の後ろを返す */
static const char *word(const char *p, const char *end, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(end - p) < n || memcmp(p, w, n)) return NULL;
    if (p + n < end && name_char(p[n])) return NULL;
    return p + n;
}

/* iN の N（1〜64）を読む。読めなければ NULL */
static const char *int_type(const char *p, const char *end, unsigned *bits)
{
    unsigned n = 0;
    const char *q;
    if (p >= end || *p != 'i') return NULL;
    for (q = p + 1; q < end && *q >= '0' && *q <= '9'; q++) {
        n = n * 10 + (unsigned)(*q - '0');
        if (n > 1000) return NULL;
    }
    if (q == p + 1 || (q < end && name_char(*q)) || n == 0 || n > 64) return NULL;
    *bits = n;
    return q;
}

/* 10 進の整数（負の表記を含む）を、bits の幅の符号なしの値として読む。読めなければ NULL */
static const char *int_value(const char *p, const char *end, unsigned bits, unsigned long long *v)
{
    unsigned long long mag = 0, mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    int neg = 0;
    const char *q;
    if (p < end && *p == '-') { neg = 1; p++; }
    for (q = p; q < end && *q >= '0' && *q <= '9'; q++) {
        if (mag > (~0ull - 9) / 10) return NULL;
        mag = mag * 10 + (unsigned long long)(*q - '0');
    }
    if (q == p || (q < end && name_char(*q))) return NULL;
    *v = (neg ? ~mag + 1 : mag) & mask;   /* inttoptr はこの値を 0 で広げる */
    return q;
}

/* 値の名前（%名前、%"名前"）。読めなければ NULL */
static const char *value_name(const char *p, const char *end)
{
    const char *q;
    if (p >= end || *p != '%') return NULL;
    q = p + 1;
    if (q < end && *q == '"') {
        for (q++; q < end && *q != '"'; q++) {}
        return q < end ? q + 1 : NULL;
    }
    while (q < end && name_char(*q)) q++;
    return q > p + 1 ? q : NULL;
}

static int in_range(unsigned long long v) { return v >= 1 && v <= NEAR_MAX; }

/* 「to ptr」を読み、ptr の後ろを返す（ptr addrspace(N) やベクタは NULL） */
static const char *to_ptr(const char *p, const char *end)
{
    const char *q;
    p = skip_ws(p, end);
    if (!(q = word(p, end, "to"))) return NULL;
    p = skip_ws(q, end);
    if (!(q = word(p, end, "ptr"))) return NULL;
    p = skip_ws(q, end);
    if (word(p, end, "addrspace")) return NULL;
    return q;
}

/* 定数の形 inttoptr (iN V to ptr)。p は ( を指す。書き換えたら 1、そのまま残すなら 0、知らない形は -1。
 * *after は読み終えた位置（-1 のときは変えない） */
static int const_form(const char *p, const char *end, unsigned long long *v, const char **after)
{
    unsigned bits;
    const char *q;
    p = skip_ws(p + 1, end);
    if (!(q = int_type(p, end, &bits))) return -1;
    p = skip_ws(q, end);
    if (!(q = int_value(p, end, bits, v))) return -1;
    if (!(q = to_ptr(q, end))) return -1;
    q = skip_ws(q, end);
    if (q >= end || *q != ')') return -1;
    *after = q + 1;
    return in_range(*v);
}

typedef struct {
    sim_buf *out;
    native_near_stats *st;
    int seq;
    const char *nl;          /* 差し込む行の改行（元の行に合わせる） */
} near_ctx;

/* 命令の形。line..end は 1 行（改行なし）、at は inttoptr の位置、p は語の後ろ。
 * 出力したら 1（*after に行の残りの先頭）、値が範囲の外の定数でそのまま残すなら 0、知らない形は -1 */
static int instr_form(near_ctx *c, const char *line, const char *at, const char *p, const char *end, const char **after)
{
    const char *q, *pre, *opnd, *opnd_end, *rest, *dbg = NULL, *dbg_end = NULL;
    unsigned bits;
    unsigned long long v;
    int literal;
    /* 行の頭は「空白 %名前 = 」 */
    pre = skip_ws(line, at);
    if (!(q = value_name(pre, at))) return -1;
    q = skip_ws(q, at);
    if (q >= at || *q != '=') return -1;
    if (skip_ws(q + 1, at) != at) return -1;
    p = skip_ws(p, end);
    if (!(q = int_type(p, end, &bits))) return -1;
    opnd = skip_ws(q, end);
    if ((opnd_end = value_name(opnd, end))) literal = 0;
    else if ((opnd_end = int_value(opnd, end, bits, &v))) literal = 1;
    else return -1;
    if (!(rest = to_ptr(opnd_end, end))) return -1;
    q = skip_ws(rest, end);
    if (q < end && *q != ',' && *q != ';') return -1;
    if (literal) {
        /* 値が定数なら、定数の形と同じ規則 */
        if (!in_range(v)) return 0;
        sim_buf_add(c->out, line, (size_t)(at - line));
        sim_buf_printf(c->out, "inttoptr i64 %llu to ptr", v + NATIVE_NEAR_BASE);
        c->st->const_replaced++;
        *after = rest;
        return 1;
    }
    /* 元の命令のデバッグの印（, !dbg !N）を、差し込む命令にも付ける */
    for (q = rest; q + 6 <= end; q++) {
        if (*q == ';') break;
        if (!memcmp(q, "!dbg ", 5) && (q == rest || !name_char(q[-1]))) {
            dbg = q;
            for (dbg_end = q + 5; dbg_end < end && blank(*dbg_end); dbg_end++) {}
            if (dbg_end < end && *dbg_end == '!') dbg_end++;
            while (dbg_end < end && name_char(*dbg_end)) dbg_end++;
            break;
        }
    }
    {
        int n = c->seq++;
        size_t indent = (size_t)(pre - line);
        sim_buf *o = c->out;
        if (bits < 64) {
            sim_buf_add(o, line, indent);
            sim_buf_printf(o, "%%sim.near.z%d = zext i%u ", n, bits);
            sim_buf_add(o, opnd, (size_t)(opnd_end - opnd));
            sim_buf_adds(o, " to i64");
            if (dbg) { sim_buf_adds(o, ", "); sim_buf_add(o, dbg, (size_t)(dbg_end - dbg)); }
            sim_buf_adds(o, c->nl);
        }
        sim_buf_add(o, line, indent);
        sim_buf_printf(o, "%%sim.near.%d = call i64 @__sim_near(i64 ", n);
        if (bits < 64) sim_buf_printf(o, "%%sim.near.z%d", n);
        else sim_buf_add(o, opnd, (size_t)(opnd_end - opnd));
        sim_buf_addc(o, ')');
        if (dbg) { sim_buf_adds(o, ", "); sim_buf_add(o, dbg, (size_t)(dbg_end - dbg)); }
        sim_buf_adds(o, c->nl);
        sim_buf_add(o, line, (size_t)(at - line));
        sim_buf_printf(o, "inttoptr i64 %%sim.near.%d to ptr", n);
    }
    c->st->dyn_inserted++;
    *after = rest;
    return 1;
}

/* 1 行（line..end、改行なし）を書き直して出力する */
static void near_line(near_ctx *c, const char *line, const char *end)
{
    const char *p = line, *done = line;
    while (p < end) {
        char ch = *p;
        if (ch == ';') break;                          /* コメント */
        if (ch == '"') {                               /* 文字列。LLVM IR では " は \22 と書くので、次の " まで */
            for (p++; p < end && *p != '"'; p++) {}
            if (p < end) p++;
            continue;
        }
        if (ch == 'i' && (p == line || (!name_char(p[-1]) && p[-1] != '%' && p[-1] != '@' && p[-1] != '!'))) {
            const char *q = word(p, end, "inttoptr");
            if (q) {
                const char *r = skip_ws(q, end), *after = NULL;
                if (r < end && *r == '(') {
                    unsigned long long v = 0;
                    int k = const_form(r, end, &v, &after);
                    if (k < 0) c->st->kept++;
                    else if (k > 0) {
                        sim_buf_add(c->out, done, (size_t)(p - done));
                        sim_buf_printf(c->out, "inttoptr (i64 %llu to ptr)", v + NATIVE_NEAR_BASE);
                        c->st->const_replaced++;
                        done = after;
                    }
                    p = k < 0 ? q : after;
                    continue;
                }
                {
                    int k = done == line ? instr_form(c, line, p, q, end, &after) : -1;
                    if (k > 0) {
                        done = after;
                        p = after;
                        continue;
                    }
                    if (k < 0) c->st->kept++;
                }
                p = q;
                continue;
            }
        }
        p++;
    }
    sim_buf_add(c->out, done, (size_t)(end - done));
}

/* 行頭の declare で @__sim_near を宣言しているか */
static int declares_near(const char *p, const char *end)
{
    static const char name[] = "@__sim_near(";
    size_t n = sizeof name - 1;
    if (!word(p, end, "declare")) return 0;
    for (; (size_t)(end - p) >= n; p++)
        if (!memcmp(p, name, n)) return 1;
    return 0;
}

int native_near(const char *text, size_t len, sim_buf *out, native_near_stats *st)
{
    near_ctx c;
    const char *p = text, *end = text ? text + len : NULL;
    int has_decl = 0;
    memset(st, 0, sizeof *st);
    if (!text || !out) return -1;
    c.out = out;
    c.st = st;
    c.seq = 0;
    c.nl = "\n";
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *le;
        if (!e) e = end;
        le = e;
        if (le > p && le[-1] == '\r') le--;
        c.nl = le < e ? "\r\n" : "\n";
        if (declares_near(p, le)) has_decl = 1;
        near_line(&c, p, le);
        sim_buf_add(out, le, (size_t)(e < end ? e + 1 - le : e - le));
        p = e < end ? e + 1 : end;
    }
    if (st->dyn_inserted && !has_decl) {
        if (len && text[len - 1] != '\n') sim_buf_adds(out, c.nl);
        /* 中身は sim_native_rt.c。メモリを読み書きしない関数なので、最適化で同じ呼び出しをまとめてよい */
        sim_buf_printf(out, "%sdeclare i64 @__sim_near(i64) nounwind willreturn memory(none)%s", c.nl, c.nl);
    }
    return 0;
}
