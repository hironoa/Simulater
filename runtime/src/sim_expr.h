/*
 * sim_expr.h - 変数名の式と値
 *
 * 名前の書式（WinAMS 互換）:
 *   name            グローバル変数。見つからなければ一意なファイル内 static
 *   file.c/name     ファイル内 static（パスは末尾一致）
 *   func@name       関数内 static。func が対象関数で name が引数名なら引数
 *   file.c/func@name
 *   @name           対象関数の引数
 *   func@@          対象関数の戻り値
 *   後置: .m  ->m  [i]
 *   前置: $ptr（個数だけ確保してポインタに設定）、&arr / *ptr（複数要素）
 *
 * すべてのメモリアクセスはバックエンド（sim_backend.h）を通す。
 */
#ifndef SIM_EXPR_H
#define SIM_EXPR_H

#include "sim_backend.h"
#include "sim_util.h"

#define SIM_MAX_ARGS 8

typedef struct sim_ctx sim_ctx;
typedef struct sim_ref sim_ref;

sim_ctx *sim_ctx_new(sim_backend *b);
void sim_ctx_free(sim_ctx *c);
sim_backend *sim_ctx_backend(sim_ctx *c);

/* 関数を名前（"func" または "file.c/func"）で探す。見つからなければ NULL と err */
const sim_func *sim_ctx_find_func(sim_ctx *c, const char *name, char *err, size_t errlen);
/* 対象関数を設定する（引数を 0 に戻す）。対応外の引数・戻り値ならエラー。f = NULL で解除 */
int sim_ctx_set_func(sim_ctx *c, const sim_func *f, char *err, size_t errlen);
const sim_func *sim_ctx_func(sim_ctx *c);
/* 対象関数を呼ぶ */
int sim_ctx_call(sim_ctx *c, char *err, size_t errlen);
/* 引数を 0 に戻す */
void sim_ctx_clear_args(sim_ctx *c);

/* 表示名の一覧（検索用）。グローバル、ファイル内 static、関数内 static */
size_t sim_ctx_sym_count(sim_ctx *c);
const char *sim_ctx_sym_name(sim_ctx *c, size_t i);
const sim_type *sim_ctx_sym_type(sim_ctx *c, size_t i);
int sim_ctx_sym_const(sim_ctx *c, size_t i);

/* 式を解析して解決する。失敗なら NULL と err */
sim_ref *sim_ref_parse(sim_ctx *c, const char *text, char *err, size_t errlen);
void sim_ref_free(sim_ref *r);
const char *sim_ref_text(const sim_ref *r);
int sim_ref_prefix(const sim_ref *r);           /* '$' '&' '*' または 0 */
int sim_ref_is_multi(const sim_ref *r);         /* 複数要素（&arr、*ptr、配列） */
int sim_ref_is_return(const sim_ref *r);        /* func@@ */
int sim_ref_writable(const sim_ref *r);
const sim_type *sim_ref_type(const sim_ref *r); /* 式の型（複数要素なら要素の型） */

/* セルの値 */
enum { SIM_ITEM_INT, SIM_ITEM_FLOAT };
typedef struct {
    int kind;
    int64_t i;          /* 整数（16 進は 64 ビットの符号なしをそのまま入れる） */
    double f;
    int hex_digits;     /* 0x 表記なら桁数、違えば 0 */
    int hex_upper;
    int hex_prefix_upper; /* 0X */
} sim_item;

typedef struct {
    sim_item *items;
    int n;
    int is_string;      /* '文字列' */
} sim_cellval;

/* 空なら n = 0。失敗なら -1 と err */
int sim_parse_cell(const char *text, size_t len, sim_cellval *out, char *err, size_t errlen);
void sim_cellval_free(sim_cellval *v);

/* 書き込み（$ / & / * / スカラ）。失敗なら -1 と err */
int sim_ref_write(sim_ctx *c, const sim_ref *r, const sim_cellval *v, char *err, size_t errlen);

/* 16 進表示の状態（出力列ごと） */
typedef struct {
    int hex_digits;     /* 0 = 10 進 */
    int upper;
    int prefix_upper;
} sim_fmt;

/* 実測値を読んで期待値と比べ、表示用の文字列を out に追加する。
 * expected が NULL か n = 0 なら比較しない（*match = -1）。一致なら 1、不一致なら 0。
 * 読めないなどのエラーなら -1 と err */
int sim_ref_check(sim_ctx *c, const sim_ref *r, const sim_cellval *expected, const sim_fmt *fmt,
                  sim_buf *out, int *match, char *err, size_t errlen);

/* GUI 用: スカラの値を double で読む（複数要素なら先頭）。読めなければ -1 */
int sim_ref_read_double(sim_ctx *c, const sim_ref *r, double *out);

/* 型の分類名 "int|uint|float|ptr|struct|array" */
const char *sim_type_kind_name(const sim_type *t);
/* 型の表示名を buf に作る */
void sim_type_display(const sim_type *t, char *buf, size_t n);

#endif
