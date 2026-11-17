/*
 * native_backend.h - PC 用 DLL の方式（exec = native）
 *
 * 製品の C のソースを clang で PC 用にコンパイルした DLL を読み込み、関数を直接呼ぶ。RL78 の命令は再現しない。
 * 設計は docs/DESIGN.md の 4.3 節。
 */
#ifndef NATIVE_BACKEND_H
#define NATIVE_BACKEND_H

#include "sim_backend.h"

/* 実行方式（--exec、なければ設定ファイルの exec）が native なら、このバックエンドを開いて *handled = 1 にする
 * （失敗なら NULL と err）。native でなければ *handled = 0 にして NULL を返し、何もしない */
sim_backend *native_backend_open(const sim_open_opts *o, int *handled, char *err, size_t errlen);

#endif
