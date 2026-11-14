#!/bin/sh
# gen_lmfprog.sh - lmfprog.s を GNU の rl78-elf-as / ld でアセンブルし、lmfprog_code.h を作り直す（開発時だけ）
#
#   tests/tools/gen_lmfprog.sh <GNU RL78 ツールの bin フォルダ>
#
# 出力の lmfprog_code.h はコミットする。CI と利用者の PC では GNU ツールは要らない。
# GNU binutils は GPL だが、ここで使うのは手書きのアセンブリをアセンブルした結果だけで、binutils のコードは入らない。
set -eu
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?GNU RL78 ツールの bin フォルダを指定してください}
W=$(mktemp -d "${TMPDIR:-/tmp}/lmfprog.XXXXXX")
trap 'rm -rf "$W"' EXIT
"$BIN/rl78-elf-as" -mg14 -o "$W/t.o" "$T/lmfprog.s"
printf 'SECTIONS { . = 0; .text : { *(.text) } }\n' > "$W/t.ld"
"$BIN/rl78-elf-ld" -T "$W/t.ld" -e _start --no-relax -o "$W/t.elf" "$W/t.o"
"$BIN/rl78-elf-objcopy" -O binary -j .text "$W/t.elf" "$W/t.bin"
"$BIN/rl78-elf-objdump" -d "$W/t.elf" > "$T/lmfprog.lst"
{
    echo "/* lmfprog_code.h - tests/tools/gen_lmfprog.sh が lmfprog.s から作る。手で直さない */"
    echo "#ifndef LMFPROG_CODE_H"
    echo "#define LMFPROG_CODE_H"
    "$BIN/rl78-elf-nm" "$W/t.elf" | awk '$2 ~ /^[TtAa]$/ && $3 ~ /^_/ && $3 !~ /^\.L/ {
        n = substr($3, 2); gsub(/[^A-Za-z0-9_]/, "_", n); printf "#define LP_%s 0x%sUL\n", n, toupper($1) }' | sort -u
    echo "static const unsigned char LP_IMAGE[] = {"
    od -An -v -tx1 "$W/t.bin" | awk '{ s = "   "; for (i = 1; i <= NF; i++) s = s " 0x" $i ","; print s }'
    echo "};"
    echo "#endif"
} > "$T/lmfprog_code.h"
echo "gen_lmfprog.sh: $T/lmfprog_code.h を作りました（$(wc -c < "$W/t.bin") バイト）"
