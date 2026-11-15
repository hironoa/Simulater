#!/bin/sh
# Build and run all RL78 core tests and the benchmark.
#   CC        compiler (default: cc)
#   RL78_GNU_BIN  optional: directory with rl78-elf-as/ld/objcopy/objdump/run;
#                 when set, the differential test against the GNU simulator runs too
#   BENCH_INSNS   instructions for the benchmark (default 300000000)
#   BENCH_MIN     optional: fail if the benchmark is below this many M insns/s
# Exit status is non-zero if any check fails (0 checks counts as a failure).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD="$ROOT/build"
CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -fno-unroll-loops -Wall -Wextra -Wpedantic -Wshadow"
mkdir -p "$BUILD"
status=0

echo "== build ($CC $CFLAGS)"
$CC $CFLAGS -Werror -I"$ROOT/include" -I"$HERE" "$ROOT"/src/*.c "$HERE"/tasm.c "$HERE"/test_main.c "$HERE"/t_*.c \
    -o "$BUILD/rl78_tests" || exit 1
$CC $CFLAGS -Werror -I"$ROOT/include" -I"$HERE" "$ROOT"/src/*.c "$HERE"/tasm.c "$HERE"/bench.c \
    -o "$BUILD/rl78_bench" || exit 1
$CC $CFLAGS -Werror -I"$ROOT/include" -I"$HERE" "$ROOT"/src/*.c "$HERE"/tasm.c "$HERE"/difftest/rl78dt.c \
    -o "$BUILD/rl78dt" || exit 1

echo "== tests"
"$BUILD/rl78_tests" || status=1

echo "== benchmark"
"$BUILD/rl78_bench" "${BENCH_INSNS:-300000000}" ${BENCH_MIN:-} || status=1

if [ -n "${RL78_GNU_BIN:-}" ]; then
    echo "== difftest vs GNU rl78-elf-run"
    RL78DT="$BUILD/rl78dt" python3 -I "$HERE/difftest/difftest.py" ${DIFFTEST_ARGS:-} || status=1
else
    echo "== difftest: SKIP (RL78_GNU_BIN not set)"
fi

if [ $status -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit $status
