#!/bin/sh
# 任意の実物確認。サンプルは読み取りのみ。成果物は一時フォルダ。
set -eu
if [ -z "${LMF_SAMPLES:-}" ]; then echo 'SKIP: LMF_SAMPLES unset (0 samples)'; exit 0; fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/lmfsamples.XXXXXX")
trap 'rm -rf "$W"' EXIT
B="$ROOT/runtime/backends/rl78"
${CC:-clang} -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"$B" -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" -I"$ROOT/rl78/include" \
 "$ROOT/tests/tools/check_lmf.c" "$B/rl78_lmf.c" "$B/rl78_elf.c" "$B/rl78_dwarf.c" "$B/rl78_map.c" "$ROOT/runtime/src/sim_util.c" \
 "$ROOT/rl78/src/rl78_disasm.c" "$ROOT/rl78/src/rl78_exec.c" "$ROOT/rl78/src/rl78_sys.c" "$ROOT/rl78/src/rl78_xlate.c" -o "$W/check"
python3 -I "$ROOT/tests/rl78/check_lmf_samples.py" "$ROOT" "$LMF_SAMPLES" "$W" "$W/check"
