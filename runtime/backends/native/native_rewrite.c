/* native_rewrite.c - 字句の境界と行番号を保って PC 用に書き直す */
#include "native_rewrite.h"
#include <stdlib.h>
#include <string.h>

/* LINE: 行頭の # の行（行の情報、#pragma）。型の語の並びの途中にも入りうる（clang -E は空行が続くと
 * 宣言の途中でも行の情報を出す）ので、空白と同じく並びを切らない。消す範囲の中にあっても残す。 */
enum { OTHER, IDENT, SPACE, KEEP, LINE };
typedef struct { size_t a, b; int kind, action; } token;

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
static int type_word(const char *s, token t)
{
    static const char *const words[] = {
        "unsigned", "signed", "long", "short", "int", "char", "const", "volatile",
        "static", "extern", "register", "auto", "typedef", "inline", "__inline",
        "__inline__", "__signed__", "__const", "__volatile__"
    };
    size_t i;
    if (t.kind != IDENT) return 0;
    for (i = 0; i < sizeof words / sizeof *words; i++) if (eq(s, t, words[i])) return 1;
    return 0;
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
        ts[n].a = a; ts[n].b = p; ts[n].kind = kind; ts[n].action = 0; n++;
    }
    for (i = 0; i < n;) {
        size_t j, k;
        if (ts[i].kind == IDENT && eq(s, ts[i], "__asm")) {
            int depth = 0;
            j = i + 1;
            while (j < n && ts[j].kind == SPACE) j++;
            if (j < n && ts[j].kind == OTHER && eq(s, ts[j], "(")) {
                for (k = j; k < n; k++) {
                    if (ts[k].kind != OTHER) continue;
                    if (eq(s, ts[k], "(")) depth++;
                    if (eq(s, ts[k], ")") && --depth == 0) break;
                }
                /* 閉じかっこがない入力も失敗にはせず、そのまま残す。 */
                if (k < n) {
                    for (j = i; j <= k; j++) if (ts[j].kind != LINE) ts[j].action = 1;
                    counts.asm_removed++; i = k + 1; continue;
                }
            }
        }
        if (type_word(s, ts[i])) {
            int has_int = 0, has_longshort = 0, has_char = 0;
            size_t sign = n;
            j = i;
            while (j < n && (ts[j].kind == SPACE || ts[j].kind == LINE || type_word(s, ts[j]))) {
                if (ts[j].kind == IDENT) {
                    if (eq(s, ts[j], "int")) has_int = 1;
                    if (eq(s, ts[j], "long") || eq(s, ts[j], "short")) has_longshort = 1;
                    if (eq(s, ts[j], "char")) has_char = 1;
                    if (sign == n && (eq(s, ts[j], "unsigned") || eq(s, ts[j], "signed") || eq(s, ts[j], "__signed__"))) sign = j;
                }
                j++;
            }
            for (k = i; k < j; k++) {
                if (ts[k].kind == IDENT && eq(s, ts[k], "int")) {
                    ts[k].action = has_longshort ? 1 : 2;
                    if (has_longshort) counts.int_dropped++; else counts.int_replaced++;
                }
            }
            if (!has_int && !has_longshort && !has_char && sign != n) {
                ts[sign].action = 3; counts.short_added++;
            }
            i = j; continue;
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
