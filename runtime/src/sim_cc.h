/*
 * sim_cc.h - コンパイラ（clang）を並列に起動する部分と、キャッシュのフォルダの後始末
 *
 * 「C への変換方式」（exec = xlate、runtime/backends/rl78/rl78_xl.c）と「PC 用 DLL の方式」
 * （exec = native、runtime/backends/native/native_backend.c）が共通に使う。
 */
#ifndef SIM_CC_H
#define SIM_CC_H

#include <stddef.h>
#include <stdint.h>

#include "sim_platform.h"

#define SIM_CC_FNV_INIT 1469598103934665603ULL
/* FNV-1a（64 ビット）。キャッシュの鍵を作るのに使う */
uint64_t sim_cc_fnv(uint64_t h, const void *data, size_t n);

/* argv を起動して終わるまで待ち、終了コードを返す（起動できなければ -1）。出力は log に追記（NULL なら捨てる） */
int sim_cc_run_wait(const char *const *argv, const char *log);
/* `cc --version` が成功すれば 1 */
int sim_cc_works(const char *cc);

/* 1 つのコンパイル。argv と name は呼び出し側が用意し、sim_cc_run_jobs が終わるまで有効にしておく */
typedef struct {
    const char *const *argv;    /* NULL 終端。argv[0] がコンパイラ */
    const char *name;           /* 進み具合と cc_times.txt に出す名前（ソースのファイル名） */
    /* 以下は sim_cc_run_jobs が書く */
    sim_proc *proc;
    uint64_t started, elapsed;  /* ナノ秒 */
    int compiled;               /* 起動できた */
    int exit_code;              /* 終了コード（起動できなければ -1） */
} sim_cc_job;

/* 経過を 1 行ずつ渡す先（行末の改行つき）。NULL なら何も出さない */
typedef void (*sim_cc_say_fn)(void *ctx, const char *line);

/* jobs を最大 parallel 個ずつ並列に起動し、全部終わるまで待つ。遅いファイルがあっても他の枠を止めない。
 * 5 秒を超えたら、完了数・経過秒数・長く動いているファイルを say に渡す（行の先頭は「label: 」）。
 * 全部が終了コード 0 なら 0、1 つでも失敗（起動できないものを含む）なら -1 */
int sim_cc_run_jobs(sim_cc_job *jobs, size_t njobs, int parallel, const char *log,
                    const char *label, sim_cc_say_fn say, void *ctx);

/* 今回コンパイルしたファイルの秒数を、長い順に dir/cc_times.txt へ書く（ファイル名<TAB>秒）。
 * 30 秒以上かかったものは 5 個まで say にも渡す。コンパイルしたものが 2 個未満なら何もしない（前回の記録を残す） */
void sim_cc_write_times(const sim_cc_job *jobs, size_t njobs, const char *dir,
                        const char *label, sim_cc_say_fn say, void *ctx);

/* フォルダ dir の直下のファイルを消し、dir も消す（下のフォルダには入らない） */
void sim_cc_remove_dir_files(const char *dir);
/* base の下の「prefix で始まり keep ではない」フォルダ（古い鍵のキャッシュ）を消す */
void sim_cc_sweep_old(const char *base, const char *prefix, const char *keep);

#endif
