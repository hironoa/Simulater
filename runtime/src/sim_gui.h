/* sim_gui.h - GUI モード（HTTP + SSE サーバと周期実行ループ） */
#ifndef SIM_GUI_H
#define SIM_GUI_H

#include "sim_expr.h"

typedef struct {
    int port;
    double period_ms;
    double speed;
    const char *func;        /* NULL ならバックエンドの既定 */
    int open_browser;
    int record_max;          /* 記録する最大ステップ数 */
    const char *html_dir;    /* sim_gui.html を探すフォルダ */
} sim_gui_opts;

/* サーバを動かす（戻らない。起動に失敗したら 2 を返す） */
int sim_gui_run(sim_ctx *c, const sim_gui_opts *o);

#endif
