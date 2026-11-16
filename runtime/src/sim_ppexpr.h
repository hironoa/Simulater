/*
 * sim_ppexpr.h - #if / #elif の条件式の評価（defines のコンパイルスイッチ用）
 *
 * C のプリプロセッサの式を、マクロを展開したうえで整数として評価する。
 *   - 整数定数（10 進・16 進・8 進・2 進 0b、U / L 接尾辞）、文字定数
 *   - defined(X) / defined X、単項 ! ~ - +、二項 * / % + - << >> < <= > >= == != & ^ | && ||、?:、括弧
 *   - 未定義の識別子は 0。オブジェクト形式マクロはトークン列として展開する（自己参照は展開しない）
 *   - 関数形式マクロの呼び出しは、仮引数を引数で置き換えた本体を展開する（引数 16 個まで）。
 *     lookup は SIM_PP_FUNCTION のとき *body に「(仮引数, …) 置き換えるトークン列」を返す（NULL なら展開できない）。
 *     可変長引数、# / ##、引数の数の違い、引数の中での同じマクロの呼び出しは展開せず、判定不能にする。
 * 評価できない式（0 除算、展開できない関数形式マクロの呼び出し、文字列、浮動小数、符号付きの桁あふれ、
 * 構文の誤りなど）は「判定不能」とする。&& || ?: で選ばれない側の判定不能は結果に影響しない。
 *
 * コンパイラによって式の幅が違う（C90 は long = 32 ビット、C99 は intmax_t = 64 ビット）ので、
 * sim_pp_eval は両方の幅で評価し、真偽が一致したときだけ結果を返す。
 */
#ifndef SIM_PPEXPR_H
#define SIM_PPEXPR_H

#include <stddef.h>
#include <stdint.h>

enum { SIM_PP_UNDEF = 0, SIM_PP_OBJECT = 1, SIM_PP_FUNCTION = 2 };

typedef struct {
    /* 名前（NUL 終端ではない。長さ len）の状態 SIM_PP_* を返す。SIM_PP_OBJECT なら *body に本体（NUL 終端）、
     * SIM_PP_FUNCTION なら *body に仮引数の並びから始まる本体（"(a, b) (a + b)"）か NULL。
     * in_defined は defined の被演算子として参照されたとき 1 */
    int (*lookup)(void *ctx, const char *name, size_t len, const char **body, int in_defined);
    void *ctx;
} sim_pp_env;

typedef struct {
    int64_t value;      /* 符号なしなら uint64_t として読む */
    int is_unsigned;
} sim_pp_value;

/* 真なら 1、偽なら 0、判定不能なら -1 */
int sim_pp_eval(const char *expr, const sim_pp_env *env);
/* bits（32 か 64）の幅で値を求める。求まれば 0、判定不能なら -1 */
int sim_pp_eval_width(const char *expr, const sim_pp_env *env, int bits, sim_pp_value *out);

/* 未定義・自己参照・関数形式マクロと負の結果を拒否する定数式。展開規則は #if と共通。 */
int sim_pp_eval_constant(const char *expr, const sim_pp_env *env, uint64_t *out);

/* 展開後の括弧深さで呼び出し側の区切り ) ] ; , } を検出。end は元の入力内。 */
int sim_pp_eval_constant_prefix(const char *expr, const sim_pp_env *env, uint64_t *out, const char **end);

#endif
