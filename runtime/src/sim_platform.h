/* sim_platform.h - スレッド、ソケット、時刻の薄い抽象（Win32 / POSIX） */
#ifndef SIM_PLATFORM_H
#define SIM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct sim_mutex sim_mutex;
typedef struct sim_cond sim_cond;

sim_mutex *sim_mutex_new(void);
void sim_mutex_lock(sim_mutex *m);
void sim_mutex_unlock(sim_mutex *m);
sim_cond *sim_cond_new(void);
/* 待つ。timeout_ms 経過で戻る（負なら時間切れなし）。m はロックした状態で呼ぶ。理由なく早く戻ることもあるので、
 * 呼び出し側は戻ったら条件を確かめ直す */
void sim_cond_wait(sim_cond *c, sim_mutex *m, int timeout_ms);
void sim_cond_broadcast(sim_cond *c);
/* sim_cond_wait の時間切れが予定より遅れうる幅の目安（ナノ秒）。期限つきで条件変数を待つときは、
 * 期限のこれだけ手前までを sim_cond_wait で待ち、残りを sim_sleep_until_ns で待つと、起こせる待機と
 * 期限の精度を両立できる。Windows の SleepConditionVariableSRW はシステムのタイマ刻み（既定 15.6ms）で
 * 起きるので大きく取る */
#ifdef _WIN32
#define SIM_COND_WAIT_SLACK_NS 25000000ULL
#else
#define SIM_COND_WAIT_SLACK_NS 2000000ULL
#endif

typedef void (*sim_thread_fn)(void *arg);
/* デタッチしたスレッドを作る。失敗なら -1 */
int sim_thread_start(sim_thread_fn fn, void *arg);

/* 単調増加の時刻（ナノ秒） */
uint64_t sim_now_ns(void);
void sim_sleep_ms(int ms);
/* 高精度に ns だけ待つ（ミリ秒未満は短いスリープとスピン） */
void sim_sleep_until_ns(uint64_t deadline_ns);

/* ソケット。sim_sock は Windows では SOCKET、POSIX では fd を入れる */
typedef intptr_t sim_sock;
#define SIM_SOCK_INVALID ((sim_sock)-1)

int sim_net_init(void);
/* 127.0.0.1:port で待ち受ける。失敗なら SIM_SOCK_INVALID と err */
sim_sock sim_listen_local(int port, char *err, size_t errlen);
sim_sock sim_accept(sim_sock s);
/* 読めたバイト数、0 = 相手が閉じた、-1 = エラー */
long sim_recv(sim_sock s, void *buf, size_t n);
/* 全部送る。成功 0、失敗 -1 */
int sim_send_all(sim_sock s, const void *buf, size_t n);
void sim_close_sock(sim_sock s);
/* 受信と送信の時間切れ（ミリ秒）。相手が読まずに止まっていても、送信が ms で失敗して戻る */
void sim_sock_set_timeout(sim_sock s, int ms);

/* 実行ファイルのあるフォルダ（末尾の区切りなし）。返す文字列は静的領域 */
const char *sim_exe_dir(void);
/* 既定のブラウザで URL を開く */
void sim_open_browser(const char *url);
/* 標準出力を UTF-8 で表示できるようにする（Windows のコンソール） */
void sim_console_utf8(void);

/* a と b が同じファイルを指すなら 1、違う（またはどちらかが存在しない・開けない）なら 0。
 * パスの表記（./、..、相対・絶対、シンボリックリンク、ハードリンク、大文字小文字）ではなく
 * ファイルそのもので比べる。POSIX は st_dev/st_ino、Windows はボリュームシリアルとファイルインデックス */
int sim_same_file(const char *a, const char *b);
/* 0 missing/unsupported, 1 regular file, 2 directory. Directory links are
   not followed, so recursive source scans cannot loop through symlinks. */
int sim_path_kind(const char *path);
int sim_dir_each(const char *path, void (*visit)(void *, const char *, int), void *ctx);

#endif
