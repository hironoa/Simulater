/* native_rewrite.c - 字句の境界と行番号を保って PC 用に書き直す */
#include "native_rewrite.h"
#include <stdlib.h>
#include <string.h>

/* LINE: 行頭の # の行（行の情報、#pragma）。型の語の並びの途中にも入りうる（clang -E は空行が続くと
 * 宣言の途中でも行の情報を出す）ので、空白と同じく並びを切らない。消す範囲の中にあっても残す。 */
enum { OTHER, IDENT, SPACE, KEEP, LINE };
/* seen: 型の語の並びとして判定済みの型の語（読み飛ばしたかっこの中を後で見直すとき二重に数えない）。 */
typedef struct { size_t a, b; int kind, action, seen; } token;

static int alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int digit(char c) { return c >= '0' && c <= '9'; }
static int white(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static int eq(const char *s, token t, const char *w)
{
    return t.b - t.a == strlen(w) && !memcmp(s + t.a, w, t.b - t.a);
}
/* 型の語の種類。W_OTHER は別の型の語で、並びを切らずに int / short の書き換えを止める。
 * W_QUAL は修飾子・指定子（GNU の別名を含む）で、並びを切らず判定にも影響しない。
 * 表にない __ または _ + 英大文字で始まる語（__attribute__、__declspec、_Alignas など。__asm は除く）と、
 * それに続くかっこの組は、並びを切らないものとして読み飛ばす（出力からは消さず、かっこの中は後で処理する）。 */
enum { W_NONE, W_QUAL, W_SIGN, W_INT, W_LONGSHORT, W_OTHER };
static int type_word(const char *s, token t)
{
    static const struct { const char *w; int k; } words[] = {
        {"unsigned", W_SIGN}, {"signed", W_SIGN}, {"__signed", W_SIGN}, {"__signed__", W_SIGN},
        {"int", W_INT}, {"long", W_LONGSHORT}, {"short", W_LONGSHORT},
        {"char", W_OTHER}, {"_Bool", W_OTHER}, {"float", W_OTHER}, {"double", W_OTHER},
        {"void", W_OTHER}, {"_Complex", W_OTHER}, {"__int8", W_OTHER}, {"__int16", W_OTHER},
        {"__int32", W_OTHER}, {"__int64", W_OTHER}, {"__int128", W_OTHER},
        {"const", W_QUAL}, {"__const", W_QUAL}, {"__const__", W_QUAL},
        {"volatile", W_QUAL}, {"__volatile", W_QUAL}, {"__volatile__", W_QUAL},
        {"restrict", W_QUAL}, {"__restrict", W_QUAL}, {"__restrict__", W_QUAL},
        {"inline", W_QUAL}, {"__inline", W_QUAL}, {"__inline__", W_QUAL},
        {"__extension__", W_QUAL}, {"static", W_QUAL}, {"extern", W_QUAL},
        {"register", W_QUAL}, {"auto", W_QUAL}, {"typedef", W_QUAL}
    };
    size_t i;
    if (t.kind != IDENT) return W_NONE;
    for (i = 0; i < sizeof words / sizeof *words; i++) if (eq(s, t, words[i].w)) return words[i].k;
    return W_NONE;
}
/* 処理系の拡張の語（表にない __ または _ + 英大文字で始まる識別子。__asm は別の処理の対象なので除く）。 */
static int ext_word(const char *s, token t)
{
    const char *w = s + t.a;
    size_t len = t.b - t.a;
    if (t.kind != IDENT || len < 2 || w[0] != '_') return 0;
    if (w[1] != '_' && !(w[1] >= 'A' && w[1] <= 'Z')) return 0;
    return !eq(s, t, "__asm") && type_word(s, t) == W_NONE;
}
/* q の後ろの空白と行の情報を飛ばした位置。 */
static size_t skip_blank(const token *ts, size_t q, size_t n)
{
    while (q < n && (ts[q].kind == SPACE || ts[q].kind == LINE)) q++;
    return q;
}
/* ts[q] の ( に対応する ) の位置。なければ n。文字列・文字定数は一つの字句なので中のかっこは数えない。 */
static size_t close_paren(const char *s, const token *ts, size_t q, size_t n)
{
    int depth = 0;
    for (; q < n; q++) {
        if (ts[q].kind != OTHER) continue;
        if (eq(s, ts[q], "(")) depth++;
        if (eq(s, ts[q], ")") && --depth == 0) return q;
    }
    return n;
}
/* 行の情報のフラグだけを読む。pragma では現在の状態を変えない。 */
static void line_info(const char *s, size_t p, size_t end, int *system)
{
    int flag3 = 0;
    p++;
    while (p < end && white(s[p])) p++;
    if (p == end || !digit(s[p])) return;
    while (p < end && digit(s[p])) p++;
    while (p < end && white(s[p])) p++;
    if (p == end || s[p++] != '"') return;
    while (p < end && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < end) p++;
        p++;
    }
    if (p == end) return;
    p++;
    while (p < end) {
        size_t a;
        while (p < end && white(s[p])) p++;
        a = p;
        while (p < end && !white(s[p])) p++;
        if (p - a == 1 && s[a] == '3') flag3 = 1;
    }
    *system = flag3;
}

int native_rewrite(const char *s, size_t len, sim_buf *out, native_rewrite_stats *st)
{
    token *ts = NULL;
    size_t n = 0, cap = 0, p = 0, i;
    int bol = 1, system = 0;
    native_rewrite_stats counts = {0};
    while (p < len) {
        size_t a = p, q = p;
        int kind = OTHER;
        if (bol && s[p] == '#') {
            while (p < len && s[p] != '\n') p++;
            line_info(s, a, p, &system);
            kind = LINE;
        } else if (white(s[p])) {
            kind = SPACE;
            do {
                if (s[p] == '\n') bol = 1;
                p++;
            } while (p < len && white(s[p]));
        } else {
            bol = 0;
            /* 接頭辞付きの文字列と文字定数も一つの字句にする。 */
            if ((s[p] == 'L' || s[p] == 'u' || s[p] == 'U') && p + 1 < len) {
                q++;
                if (s[p] == 'u' && s[q] == '8' && q + 1 < len) q++;
            }
            if (s[q] == '"' || s[q] == '\'') {
                char quote = s[q++];
                /* 閉じていない引用符は行末で終える（後ろの行頭の # を飲み込まない）。 */
                while (q < len && s[q] != '\n') {
                    if (s[q] == '\\' && q + 1 < len && s[q + 1] != '\n') { q += 2; continue; }
                    if (s[q++] == quote) break;
                }
                p = q;
                kind = KEEP;
            } else if (digit(s[p]) || (s[p] == '.' && p + 1 < len && digit(s[p + 1]))) {
                p++;
                while (p < len) {
                    char c = s[p], prev = s[p - 1];
                    if (alpha(c) || digit(c) || c == '.' ||
                        ((c == '+' || c == '-') && (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P'))) p++;
                    else break;
                }
                kind = KEEP;
            } else if (alpha(s[p])) {
                kind = IDENT;
                do { p++; } while (p < len && (alpha(s[p]) || digit(s[p])));
            } else p++;
            if (system) kind = KEEP;
        }
        if (n == cap) { cap = cap ? cap * 2 : 256; ts = sim_xrealloc(ts, cap * sizeof *ts); }
        ts[n].a = a; ts[n].b = p; ts[n].kind = kind; ts[n].action = 0; ts[n].seen = 0; n++;
    }
    for (i = 0; i < n;) {
        size_t j, k;
        if (ts[i].kind == IDENT && eq(s, ts[i], "__asm")) {
            /* clang -E は __asm と ( の間にも行の情報を出すことがある。LINE は残す（下の消す範囲で除く）。 */
            j = skip_blank(ts, i + 1, n);
            if (j < n && ts[j].kind == OTHER && eq(s, ts[j], "(")) {
                k = close_paren(s, ts, j, n);
                /* 閉じかっこがない入力も失敗にはせず、そのまま残す。 */
                if (k < n) {
                    for (j = i; j <= k; j++) if (ts[j].kind != LINE) ts[j].action = 1;
                    counts.asm_removed++; i = k + 1; continue;
                }
            }
        }
        if (type_word(s, ts[i]) != W_NONE && ts[i].seen) { i++; continue; }
        if (type_word(s, ts[i]) != W_NONE) {
            int has_int = 0, has_longshort = 0, has_other = 0, w;
            size_t sign = n, resume = n;
            j = i;
            while (j < n) {
                if (ts[j].kind == SPACE || ts[j].kind == LINE) { j++; continue; }
                w = type_word(s, ts[j]);
                if (w != W_NONE) {
                    if (w == W_INT) has_int = 1;
                    if (w == W_LONGSHORT) has_longshort = 1;
                    if (w == W_OTHER) has_other = 1;
                    if (sign == n && w == W_SIGN) sign = j;
                    ts[j].seen = 1; j++; continue;
                }
                if (ext_word(s, ts[j])) {
                    /* 拡張の語に続くかっこの組は先読みで飛ばすだけ。中は resume から通常どおり処理する。 */
                    size_t q = skip_blank(ts, j + 1, n), e;
                    if (q < n && ts[q].kind == OTHER && eq(s, ts[q], "(")) {
                        e = close_paren(s, ts, q, n);
                        if (e == n) break;
                        if (resume == n) resume = q;
                        j = e + 1; continue;
                    }
                    j++; continue;
                }
                break;
            }
            for (k = i; k < j && !has_other; k++) {
                if (ts[k].seen && eq(s, ts[k], "int")) {
                    ts[k].action = has_longshort ? 1 : 2;
                    if (has_longshort) counts.int_dropped++; else counts.int_replaced++;
                }
            }
            if (!has_int && !has_longshort && !has_other && sign != n) {
                ts[sign].action = 3; counts.short_added++;
            }
            i = resume < j ? resume : j; continue;
        }
        i++;
    }
    for (i = 0; i < n; i++) {
        token t = ts[i];
        if (t.action == 1) {
            for (p = t.a; p < t.b; p++) if (s[p] == '\n' || s[p] == '\r') sim_buf_addc(out, s[p]);
        } else if (t.action == 2) sim_buf_adds(out, "short");
        else {
            sim_buf_add(out, s + t.a, t.b - t.a);
            if (t.action == 3) sim_buf_adds(out, " short");
        }
    }
    free(ts);
    if (st) *st = counts;
    return 0;
}
