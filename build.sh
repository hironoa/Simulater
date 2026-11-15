#!/bin/sh
# build.sh - macOS / Linux 用ビルドスクリプト（clang を直接呼ぶ。CMake / Python / make は使わない）
#
#   ./build.sh [出力フォルダ]          本物のバックエンド（RL78 命令シミュレータ）で sim をビルドする（既定 build/sim）
#   ./build.sh mock [出力フォルダ]     モック・バックエンドで sim をビルドする（共通部分のテスト用。既定 build/mock）
#
# 環境変数 CC でコンパイラを変えられる（既定 clang）。
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-clang}
MODE=real
if [ "${1:-}" = "mock" ]; then MODE=mock; shift; fi
if [ "$MODE" = "mock" ]; then OUT=${1:-"$ROOT/build/mock"}; else OUT=${1:-"$ROOT/build/sim"}; fi
mkdir -p "$OUT"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "build.sh: コンパイラ '$CC' が見つかりません（環境変数 CC で指定できます）" >&2
    exit 2
fi

LIBS="-lpthread"
case "$(uname -s)" in
    Linux*) LIBS="$LIBS -lm -ldl" ;;
esac

S="$ROOT/runtime/src"
# -fno-unroll-loops: clang 20 の素の -O2 は x86-64 で命令実行ループを約 10 倍遅くする（Windows で 27 → 254 百万命令/秒）
CFLAGS="-std=c11 -O2 -fno-unroll-loops -g -Wall -Wextra -D_DEFAULT_SOURCE -I$ROOT/runtime/include -I$S"
COMMON="$S/sim_main.c $S/sim_util.c $S/sim_json.c $S/sim_platform.c $S/sim_expr.c $S/sim_csv.c $S/sim_gui.c $S/sim_cfg.c $S/sim_defines.c $S/sim_ppexpr.c $S/sim_mtpj.c"

if [ "$MODE" = "mock" ]; then
    SRCS="$COMMON $ROOT/runtime/backends/mock/mock_backend.c"
else
    B="$ROOT/runtime/backends/rl78"
    CFLAGS="$CFLAGS -I$ROOT/rl78/include -I$B"
    SRCS="$COMMON $B/rl78_backend.c $B/rl78_xl.c $B/rl78_abi.c $B/rl78_lmf.c $B/rl78_sym.c $B/rl78_elf.c $B/rl78_dwarf.c $B/rl78_map.c \
$ROOT/rl78/src/rl78_exec.c $ROOT/rl78/src/rl78_sys.c $ROOT/rl78/src/rl78_disasm.c $ROOT/rl78/src/rl78_xlate.c"
fi

echo "build.sh: $CC で $OUT/sim をビルドします（バックエンド: ${MODE}）"
# shellcheck disable=SC2086
"$CC" $CFLAGS $SRCS -o "$OUT/sim" $LIBS

if [ -f "$ROOT/gui/dist/sim_gui.html" ]; then
    cp "$ROOT/gui/dist/sim_gui.html" "$OUT/sim_gui.html"
    echo "build.sh: gui/dist/sim_gui.html をコピーしました"
else
    rm -f "$OUT/sim_gui.html"
    echo "build.sh: gui/dist/sim_gui.html がないので、sim gui は「GUI が未ビルド」のページを返します"
fi
if [ "$MODE" != "mock" ]; then
    # C への変換方式（exec = xlate）が clang に渡すソース
    mkdir -p "$OUT/xlate"
    cp "$ROOT/rl78/include/rl78.h" "$ROOT/rl78/src/rl78_internal.h" "$ROOT/rl78/src/rl78_exec_defs.h" \
       "$ROOT/rl78/src/rl78_exec_ops.inc" "$ROOT/rl78/src/rl78_xlate_rt.h" "$OUT/xlate/"
fi
echo "build.sh: 完了 -> $OUT/sim"
