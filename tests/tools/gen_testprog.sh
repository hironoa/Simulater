#!/bin/sh
# gen_testprog.sh - testprog.s を GNU の rl78-elf-as / ld でアセンブルし、testprog_code.h を作り直す（開発時だけ）
#
#   tests/tools/gen_testprog.sh <GNU RL78 ツールの bin フォルダ>
#
# 出力の testprog_code.h はコミットする。CI と利用者の PC では GNU ツールは要らない。
# GNU binutils は GPL だが、ここで使うのは手書きのアセンブリをアセンブルした結果だけで、binutils のコードは入らない。
set -eu
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?GNU RL78 ツールの bin フォルダを指定してください}
W=$(mktemp -d "${TMPDIR:-/tmp}/testprog.XXXXXX")
trap 'rm -rf "$W"' EXIT
"$BIN/rl78-elf-as" -mg14 -o "$W/t.o" "$T/testprog.s"
printf 'SECTIONS { . = 0; .text : { *(.text) } }\n' > "$W/t.ld"
"$BIN/rl78-elf-ld" -T "$W/t.ld" -e _start --no-relax -o "$W/t.elf" "$W/t.o"
"$BIN/rl78-elf-objcopy" -O binary -j .text "$W/t.elf" "$W/t.bin"
"$BIN/rl78-elf-objdump" -d "$W/t.elf" > "$T/testprog.lst"
{
    echo "/* testprog_code.h - tests/tools/gen_testprog.sh が testprog.s から作る。手で直さない */"
    echo "#ifndef TESTPROG_CODE_H"
    echo "#define TESTPROG_CODE_H"
    "$BIN/rl78-elf-nm" "$W/t.elf" | awk '$2 ~ /^[TtAa]$/ && $3 ~ /^_/ && $3 !~ /^\.L/ {
        n = substr($3, 2); gsub(/[^A-Za-z0-9_]/, "_", n); printf "#define TP_%s 0x%sUL\n", n, toupper($1) }' | sort -u
    echo "static const unsigned char TP_IMAGE[] = {"
    od -An -v -tx1 "$W/t.bin" | awk '{ s = "   "; for (i = 1; i <= NF; i++) s = s " 0x" $i ","; print s }'
    echo "};"
    echo "#endif"
} > "$T/testprog_code.h"
echo "gen_testprog.sh: $T/testprog_code.h を作りました（$(wc -c < "$W/t.bin") バイト）"
