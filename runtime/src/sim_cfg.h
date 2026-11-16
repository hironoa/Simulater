/* sim_cfg.h - 設定ファイル sim.cfg（KEY=VALUE、# 以降はコメント） */
#ifndef SIM_CFG_H
#define SIM_CFG_H

#include <stddef.h>

typedef struct {
    char *key;      /* 小文字に正規化 */
    char *value;    /* 前後の空白を除いたもの */
    int line;
    int used;       /* 解釈した側が 1 にする（未知のキーの警告用） */
} sim_cfg_entry;

typedef struct {
    char *path;     /* 読んだファイル（なければ NULL） */
    char *dir;      /* そのフォルダ（相対パスの基準）。なければ "." */
    sim_cfg_entry *e;
    int n;
} sim_cfg;

/* --cfg、ロードモジュールのフォルダ、カレントの順に読むパスを決める（呼び出し側で free）。
 * --cfg は開けなくてもそのパスを返す。自動探索で見つからなければ "sim.cfg" を返す。 */
char *sim_cfg_locate(const char *cfg_path, const char *abs_path);
/* 読む。ファイルがなければ -1（cfg は空のまま使える）。書式の誤りは err に書いて -2 */
int sim_cfg_load(sim_cfg *c, const char *path, char *err, size_t errlen);
void sim_cfg_init(sim_cfg *c);
void sim_cfg_free(sim_cfg *c);
/* 0 auto, 1 once, 2 every; -1 invalid. CLI takes precedence. */
int sim_cfg_csv_init(sim_cfg *c, const char *override, char *err, size_t errlen);
/* 最後に現れた値（なければ NULL）。見つかれば used を立てる */
const char *sim_cfg_get(sim_cfg *c, const char *key);
/* 設定ファイルのフォルダを基準にしたパス（呼び出し側で free） */
char *sim_cfg_path(const sim_cfg *c, const char *rel);

#endif
