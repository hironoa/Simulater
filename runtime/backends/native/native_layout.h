/* native_layout.h - ロードモジュールの変数（マイコンの番地）と、DLL のデバッグ情報の変数（PC の番地）を名前で対応づける */
#ifndef NATIVE_LAYOUT_H
#define NATIVE_LAYOUT_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *file;       /* 定義しているソース（パス可）。不明なら NULL か "" */
    const char *func;       /* 関数の中の static なら関数名、違えば NULL */
    int is_external;        /* グローバルなら 1 */
    int product;            /* DLL の側だけ: 製品のプロジェクト（mtpj）に登録されたソースの変数なら 1。native_src のソースは 0 */
} native_layout_var;

#define NATIVE_LAYOUT_NONE (-1)     /* 対応する変数がない */
#define NATIVE_LAYOUT_SRC (-2)      /* native_src のソースの変数にだけ当たった（クリアの対象にしない） */

/* lm（ロードモジュールの変数）の 1 つずつに、対応する dll の添字を match に入れる。dll の 1 つの変数は 1 度だけ使う。
 * 名前と関数名（関数の中の static なら）はいつも合わせる。グローバルどうしはファイル名を見ない。
 * それ以外は、ファイル名の末尾の一致（区切りの / と \ と、Windows では大文字小文字の違いを吸収）、
 * なければファイル名の部分（大文字小文字を区別しない）の一致で選ぶ。
 * 対応づけの対象は product = 1 の dll の変数だけ。product = 0 の変数にだけ当たったものは NATIVE_LAYOUT_SRC。
 * 成功は 0。メモリが足りなければ sim_xmalloc の決まりで終了する */
int native_layout_match(const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll, long *match);

#endif
