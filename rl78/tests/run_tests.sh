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

echo "== translation to C (needs clang)"
XLCC=${XLCC:-clang}
if ! command -v "$XLCC" >/dev/null 2>&1; then
    echo "translation: FAIL ($XLCC not found; set XLCC)"
    status=1
else
    mkdir -p "$BUILD/xl"
    XLLIBS=""
    case "$(uname -s)" in
        Darwin*) XLDLL="$BUILD/xl/xl.dylib"; XLLINK="-dynamiclib" ;;             # libSystem is part of the OS
        *) XLDLL="$BUILD/xl/xl.so"; XLLINK="-shared -nostdlib"; XLLIBS="-ldl" ;;
    esac
    $CC $CFLAGS -Werror -I"$ROOT/include" -I"$HERE" "$ROOT"/src/*.c "$HERE"/tasm.c "$HERE"/xlgen.c -o "$BUILD/xlgen" || exit 1
    $CC $CFLAGS -Werror -I"$ROOT/include" -I"$HERE" "$ROOT"/src/*.c "$HERE"/tasm.c "$HERE"/xlrun.c -o "$BUILD/xlrun" $XLLIBS || exit 1
    if "$BUILD/xlgen" "$BUILD/xl" &&
       "$XLCC" -O2 -fno-unroll-loops -ffreestanding -fPIC $XLLINK -DRL78_XL_DLL -I"$ROOT/include" -I"$ROOT/src" \
           "$BUILD/xl/xl_unit.c" "$BUILD/xl/xl_main.c" -o "$XLDLL"; then
        "$BUILD/xlrun" "$BUILD/xl" "$XLDLL" || status=1
    else
        echo "translation: FAIL (cannot translate or build the DLL)"
        status=1
    fi
fi

if [ -n "${RL78_GNU_BIN:-}" ]; then
    echo "== difftest vs GNU rl78-elf-run"
    RL78DT="$BUILD/rl78dt" python3 -I "$HERE/difftest/difftest.py" ${DIFFTEST_ARGS:-} || status=1
else
    echo "== difftest: SKIP (RL78_GNU_BIN not set)"
fi

if [ $status -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit $status
