/* rl78_xl.h - 「C への変換方式」をバックエンドから使う部分（rl78_xl.c） */
#ifndef RL78_XL_H
#define RL78_XL_H

#include <stddef.h>
#include <stdint.h>

#include "rl78.h"

enum { RL78_XL_OFF = 0, RL78_XL_ON = 1, RL78_XL_VERIFY = 2 };

typedef struct rl78_xl rl78_xl;

typedef struct {
    int mode;               /* RL78_XL_ON / RL78_XL_VERIFY */
    int quiet;              /* 1 なら経過を表示しない */
    uint32_t rom_size;      /* 変換の対象（書き込み禁止のコードフラッシュ）の大きさ */
    const char *exe_dir;    /* sim の実行ファイルのフォルダ（その下の xlate/ に変換用のソースがある） */
    const char *rt_dir;     /* 変換用のソースのフォルダを直接指定するとき（NULL なら exe_dir/xlate） */
    const char *cc;         /* clang のパス（NULL なら環境変数 SIM_XLATE_CC、なければ PATH の clang） */
    const char *dir;        /* キャッシュのフォルダ（NULL ならロードモジュールの隣の .simxl） */
    const char *abs_path;   /* ロードモジュール */
    const char *core_desc;  /* CPU の設定の説明（鍵に入れる） */
    int jobs;               /* コンパイルの並列数（0 なら CPU の数） */
} rl78_xl_opts;

/* 前回の変換結果があれば読み込んで cpu に取り付け、CALL の飛び先の記録を始める。必ず非 NULL を返す
 * （変換できない事情があれば rl78_xl_why_off が理由を返し、実行は解釈実行のまま続けられる） */
rl78_xl *rl78_xl_open(rl78_cpu *cpu, const rl78_xl_opts *o);
int rl78_xl_loaded(const rl78_xl *x);          /* 変換結果を読み込んで使っているなら 1 */
size_t rl78_xl_known(const rl78_xl *x);        /* 変換済みの関数の数 */
const char *rl78_xl_dir(const rl78_xl *x);
const char *rl78_xl_why_off(const rl78_xl *x); /* 変換できない理由（できるなら ""） */
/* 対象関数として呼び出した入口を知らせる（CALL の飛び先ではないので記録に出ない） */
void rl78_xl_note_entry(rl78_xl *x, uint32_t addr);
/* まだ変換していない関数（記録された飛び先、知らされた入口、extra）があれば、全体を変換・コンパイルし直す。
 * 作り直したら新しい関数の数、何もなければ 0、失敗したら -1（経過と理由は標準出力に書く）。
 * できた変換結果は次に開いたときから使う */
int rl78_xl_update(rl78_xl *x, const uint32_t *extra, size_t nextra);
void rl78_xl_close(rl78_xl *x);

#endif
