/* sim_csv.h - WinAMS 形式のテスト CSV の実行と結果 CSV の書き出し */
#ifndef SIM_CSV_H
#define SIM_CSV_H

#include "sim_expr.h"

typedef struct {
    const char *outdir;     /* 結果 CSV の出力フォルダ */
    int expect_actual;      /* 読み直しで "実測?(期待)" の実測側を期待値にする */
    int fixed_time;         /* 実行時間を 0.0000ms に固定（テスト用） */
    int quiet;              /* NG の詳細を出さない */
    const char *const *inputs;  /* 同じ実行で渡されたすべての入力（結果で上書きしないため。NULL 可） */
    int ninputs;
} sim_csv_opts;

typedef struct {
    long total, ok, ng, nocheck;
    int errors;             /* 実行できなかったブロックの数 */
    double seconds;         /* 対象関数の呼び出しにかかった合計時間 */
} sim_csv_stats;

/* 1 ファイルを実行して結果を書く。読めない/書けないなら -1 */
int sim_csv_run_file(sim_ctx *c, const char *path, const sim_csv_opts *o, sim_csv_stats *st);

/* GUI の読み込み用: 先頭の mod ブロックを表にする */
typedef struct {
    char *func;
    int nin, nout;
    char **names;           /* nin + nout 個 */
    int nrows;
    char ***cells;          /* nrows 行 × nin 列（空セルは NULL） */
} sim_csv_table;

int sim_csv_load_table(const char *data, size_t len, sim_csv_table *t, char *err, size_t errlen);
void sim_csv_table_free(sim_csv_table *t);

#endif
