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

#define NATIVE_LAYOUT_NONE (-1)       /* 対応する変数がない */
#define NATIVE_LAYOUT_SRC (-2)        /* native_src のソースの変数が相手に選ばれた（クリアの対象にしない） */
#define NATIVE_LAYOUT_AMBIGUOUS (-3)  /* 同じ強さの相手が複数あり、決められない（対応づけない） */

/* lm（ロードモジュールの変数）の 1 つずつに、対応する dll の添字を match に入れる。
 * 相手の候補は、名前と関数名（関数の中の static なら）が同じ dll の変数。候補ごとに一致の強さを決める:
 *   - どちらもグローバル（lm は関数名なし）: いちばん強い（名前はプログラム全体で 1 つなので、ファイル名を見ない）
 *   - それ以外: ファイルのパスの末尾から数えて、同じ部分（区切りの / と \ を同じとみなし、Windows では大文字小文字を
 *     区別しない）がいくつ続くか。多いほど強い。0（ファイル名の部分も違う、ファイル名がない）は候補にしない
 * 製品（product = 1）と native_src（product = 0）の候補をまとめて、強い組から順に確定する。同じ強さで、lm の 1 つの
 * 変数に候補が複数ある、または dll の 1 つの変数が lm の複数の変数の候補になるときは、どれとも決めず
 * NATIVE_LAYOUT_AMBIGUOUS にする（その候補の dll の変数も、より弱い組には使わない）。
 * 相手が native_src の変数なら NATIVE_LAYOUT_SRC。dll の 1 つの変数は 1 度だけ使い、結果は lm の並びに左右されない。
 * 成功は 0。メモリが足りなければ sim_xmalloc の決まりで終了する */
int native_layout_match(const native_layout_var *lm, size_t nlm, const native_layout_var *dll, size_t ndll, long *match);

/* a と b のパスの末尾から数えて、同じ部分がいくつ続くか（上の「強さ」。どちらかが空なら 0） */
int native_layout_common_tail(const char *a, const char *b);

#endif
