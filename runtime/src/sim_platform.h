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
/* 待つ。timeout_ms 経過で戻る。m はロックした状態で呼ぶ */
void sim_cond_wait(sim_cond *c, sim_mutex *m, int timeout_ms);
void sim_cond_broadcast(sim_cond *c);

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
void sim_sock_set_timeout(sim_sock s, int recv_ms);

/* 実行ファイルのあるフォルダ（末尾の区切りなし）。返す文字列は静的領域 */
const char *sim_exe_dir(void);
/* 既定のブラウザで URL を開く */
void sim_open_browser(const char *url);
/* 標準出力を UTF-8 で表示できるようにする（Windows のコンソール） */
void sim_console_utf8(void);

#endif
