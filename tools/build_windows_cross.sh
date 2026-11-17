#!/bin/sh
# build_windows_cross.sh - macOS / Linux の上で Windows 用の sim.exe を作り、配布用のフォルダにまとめる
#
#   tools/build_windows_cross.sh [出力フォルダ]     （既定 dist。その下の sim-windows-x64/ を作り直す）
#
# MinGW-w64 の gcc（x86_64-w64-mingw32-gcc。macOS なら brew install mingw-w64）が要る。
# 設定は build.bat の CC=gcc の場合と同じ（-static: MinGW の DLL に依存しない）。
# 普段の配布物は CI（GitHub Actions の windows ジョブ）が作る。これは CI を使えないときの代わりで、
# 作った sim.exe をこの機械では実行できない（Windows での動作は別に確かめること）。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=${1:-"$ROOT/dist"}
CC=${CC:-x86_64-w64-mingw32-gcc}
NAME=sim-windows-x64

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "build_windows_cross.sh: $CC が見つかりません（macOS なら brew install mingw-w64）" >&2
    exit 2
fi

S="$ROOT/runtime/src"
B="$ROOT/runtime/backends/rl78"
C="$ROOT/rl78/src"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/simwin.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
OUT="$WORK/$NAME"
mkdir -p "$OUT/xlate" "$DIST"
DIST=$(cd "$DIST" && pwd)

echo "build_windows_cross.sh: $CC で sim.exe をビルドします"
"$CC" -static -D__USE_MINGW_ANSI_STDIO=1 -std=c11 -O2 -fno-unroll-loops -Wall -Wextra \
    -I"$ROOT/runtime/include" -I"$S" -I"$ROOT/rl78/include" -I"$B" \
    "$S/sim_main.c" "$S/sim_util.c" "$S/sim_json.c" "$S/sim_platform.c" "$S/sim_expr.c" "$S/sim_csv.c" "$S/sim_gui.c" \
    "$S/sim_cfg.c" "$S/sim_defines.c" "$S/sim_ppexpr.c" "$S/sim_mtpj.c" "$S/sim_cc.c" \
    "$B/rl78_backend.c" "$B/rl78_xl.c" "$B/rl78_abi.c" "$B/rl78_lmf.c" "$B/rl78_sym.c" "$B/rl78_elf.c" "$B/rl78_dwarf.c" "$B/rl78_map.c" \
    "$C/rl78_exec.c" "$C/rl78_sys.c" "$C/rl78_disasm.c" "$C/rl78_xlate.c" \
    -o "$OUT/sim.exe" -lws2_32 -lshell32

# CI の「配布物をまとめる」と同じ中身
cp "$ROOT/gui/dist/sim_gui.html" "$OUT/"
cp "$ROOT/rl78/include/rl78.h" "$C/rl78_internal.h" "$C/rl78_exec_defs.h" "$C/rl78_exec_ops.inc" "$C/rl78_xlate_rt.h" "$OUT/xlate/"
cp "$ROOT/example/sim.cfg" "$OUT/sim.cfg.example"
cp "$ROOT/README.md" "$OUT/"

# 前の中身を残さないよう、フォルダごと置き換える（ビルドが成功してから消す）
rm -rf "$DIST/$NAME"
cp -R "$OUT" "$DIST/$NAME"
echo "build_windows_cross.sh: 完了 -> $DIST/$NAME/（$("$CC" --version | head -1)）"
