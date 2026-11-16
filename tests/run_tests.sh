#!/bin/sh
# tests/run_tests.sh - 自動テスト
#
#   tests/run_tests.sh                 ビルドしてからテストする（前半: モック・バックエンドで共通部分、
#                                      後半: 本物の RL78 バックエンドで .abs の読み込み・呼び出し規約・GUI）
#   SIM=パス tests/run_tests.sh        ビルド済みのモック版 sim を使う
#   SIM_RL78=パス tests/run_tests.sh   ビルド済みの本物の sim を使う
#   RL78_PERF_ROWS=N                   性能の実測で呼ぶ回数（既定 20000）
#
# 最後に実行件数と失敗件数を出し、失敗があれば 1 で終わる。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/tests"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/simtest.XXXXXX")
PASS=0
FAIL=0
GUI_PID=""

cleanup() {
    if [ -n "$GUI_PID" ]; then kill "$GUI_PID" 2>/dev/null; wait "$GUI_PID" 2>/dev/null; fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

ok() { PASS=$((PASS + 1)); echo "  OK    $1"; }
ng() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; [ $# -ge 2 ] && echo "        $2"; return 0; }
check() {
    _name=$1; shift
    if "$@"; then ok "$_name"; else ng "$_name"; fi
}
contains() { grep -F -q -- "$2" "$1"; }
lacks() { ! grep -F -q -- "$2" "$1"; }

# ---------------------------------------------------------------- defines のコンパイルスイッチ（モックと本物で共通）
# tests/defines/cond のヘッダは番地を 4 桁（0xF960 など）で書いてある。モックは defines_near_base = 0 で 0x0F960、
# 本物は既定の 0xF0000 を足して 0xFF960 になる。
COND="$T/defines/cond"
# cond_cfg 行...: $WORK/cond.cfg を作る（COND_HI が 0F9 ならモック用）
cond_cfg() {
    {
        printf 'defines = %s\n' "$COND"
        [ "$COND_HI" = 0F9 ] && printf 'defines_near_base = 0\n'
        for _l in "$@"; do printf '%s\n' "$_l"; done
    } > "$WORK/cond.cfg"
}
# cond_list sim の引数...: sim list の結果を $WORK/cond_list.log に、sim info を $WORK/cond_info.log に取る
cond_list() {
    "$COND_SIM" list $COND_ARGS --cfg "$WORK/cond.cfg" "$@" COND_ > "$WORK/cond_list.log" 2>&1
    "$COND_SIM" info $COND_ARGS --cfg "$WORK/cond.cfg" "$@" > "$WORK/cond_info.log" 2>&1
}
# scope_run 設定の行...: tests/defines_scope（品種ごとに同じ名前のファイルをフォルダで切り替える構成）で
# sim list / sim info を取る。結果は $WORK/scope_list.log と $WORK/scope_info.log、info の終了コードは SCOPE_RC
SCOPE="$T/defines_scope"
scope_run() {
    {
        [ "$COND_HI" = 0F9 ] && printf 'defines_near_base = 0\n'
        for _l in "$@"; do printf '%s\n' "$_l"; done
    } > "$WORK/scope.cfg"
    "$COND_SIM" list $COND_ARGS --cfg "$WORK/scope.cfg" SCOPE_ > "$WORK/scope_list.log" 2>&1
    "$COND_SIM" info $COND_ARGS --cfg "$WORK/scope.cfg" > "$WORK/scope_info.log" 2>&1
    SCOPE_RC=$?
}
# cond_at 番地の下 2 桁 名前 型: 取り込まれた番地と型
cond_at() { grep -q -E "^var  0x${COND_HI}$1 $2 +$3 " "$WORK/cond_list.log"; }
# cond_suite 名前 sim 番地の上位（0F9 / FF9） テスト CSV [sim の共通引数]
cond_suite() {
    _t=$1; COND_SIM=$2; COND_HI=$3; _csv=$4; COND_ARGS=${5:-}
    # スイッチを与えない: #else の枝。未定義の識別子の一覧に出る
    cond_cfg
    cond_list
    check "$_t: no switch takes the #else branches" sh -c 'grep -q -E "^var  0x${1}64 COND_REG +u8 " "$2" && grep -q -E "^var  0x${1}67 COND_SIM +u8 " "$2" && grep -q -E "^var  0x${1}69 COND_AB +u8 " "$2" && grep -q -E "^var  0x${1}6D COND_NEST +u8 " "$2"' sh "$COND_HI" "$WORK/cond_list.log"
    check "$_t: switch defined in an included header (#elif COND_BOARD == 3)" cond_at 6F COND_INC u8
    check "$_t: short-circuit keeps division by an undefined name determinate" cond_at 77 COND_SHORT u8
    check "$_t: indeterminate #if reads every branch, first definition wins" cond_at 72 COND_UNKNOWN u8
    check "$_t: same-named header nearest to the includer (near)" cond_at 78 COND_NEAR_OK u8
    check "$_t: same-named header nearest to the includer (far)" cond_at 7A COND_FAR_OK u8
    check "$_t: include cycle stops and both files are read" sh -c 'grep -q "COND_CYCLE " "$1" && grep -q "COND_CYCLE_B " "$1"' sh "$WORK/cond_list.log"
    check "$_t: macro in a .c after its includes" cond_at 84 COND_LOCAL u8
    check "$_t: info shows cond=on and the time breakdown" sh -c 'grep -q "^defines cond=on roots=4 " "$1" && grep -q -E "^defines time: scan=[0-9.]+ s read=[0-9.]+ s cond=[0-9.]+ s resolve=[0-9.]+ s" "$1"' sh "$WORK/cond_info.log"
    check "$_t: info counts evaluated and indeterminate #if, missing and cut includes" sh -c 'grep -q -E "include-missing=2 include-cut=[1-9][0-9]* if-evaluated=[1-9][0-9]* if-indeterminate=1$" "$1"' sh "$WORK/cond_info.log"
    check "$_t: undefined identifiers listed with reference counts" sh -c 'grep -q -x "  PRODUCT (4)" "$1" && grep -q -x "  SIM (2)" "$1" && grep -q -x "  COND_UNSET_SWITCH (1)" "$1" && grep -q -x "  COND_A (1)" "$1"' sh "$WORK/cond_info.log"
    check "$_t: include guards are not listed as undefined" sh -c '! grep -q -E "COND_PRODUCT_H|COND_SWITCH_H|COND_TYPES_H" "$1"' sh "$WORK/cond_info.log"
    cond_list -v
    check "$_t: verbose lists the indeterminate #if with file and line" sh -c 'grep -q "判定不能: .*cond_product.h:58: #if COND_FUNC(1) > 0" "$1"' sh "$WORK/cond_info.log"
    check "$_t: verbose lists names defined elsewhere but not visible" contains "$WORK/cond_info.log" "  COND_HAS_SIM (1)"
    check "$_t: verbose lists differing duplicates with file and line" sh -c 'grep -q "異なる重複 COND_UNKNOWN: .*cond_product.h:59 / .*cond_product.h:61" "$1"' sh "$WORK/cond_info.log"
    # PRODUCT = 1
    cond_cfg 'cdef = PRODUCT=1'
    cond_list
    check "$_t: cdef PRODUCT=1 selects the #if branch and the 8-bit typedef" sh -c 'grep -q -E "^var  0x${1}60 COND_REG +u8 " "$2" && grep -q -E "^var  0x${1}6A COND_NEST +u8 " "$2"' sh "$COND_HI" "$WORK/cond_list.log"
    check "$_t: PRODUCT no longer listed as undefined" sh -c '! grep -q "^  PRODUCT (" "$1"' sh "$WORK/cond_info.log"
    "$COND_SIM" run $COND_ARGS --cfg "$WORK/cond.cfg" "$_csv" -o "$WORK/cond_out" > "$WORK/cond_run.log" 2>&1
    check "$_t: PRODUCT=1 has neither the 16-bit register nor the member (exit 2)" test $? -eq 2
    # PRODUCT = 2、SIM、COND_A（sim.cfg）と COND_DIV（コマンドライン）
    cond_cfg 'cdef = PRODUCT=2' 'cdef = SIM' 'cdef = COND_A'
    cond_list --cdef COND_DIV=2
    check "$_t: cdef PRODUCT=2 selects the #elif branch and the 16-bit typedef" cond_at 62 COND_REG u16
    check "$_t: #ifdef SIM" cond_at 66 COND_SIM u8
    check "$_t: defined(A) && !defined(B)" cond_at 68 COND_AB u8
    check "$_t: nested #if / #ifdef" cond_at 6B COND_NEST u8
    check "$_t: switch from an included header sees SIM (COND_HAS_SIM)" cond_at 6E COND_INC u8
    check "$_t: --cdef value used in arithmetic (8 / COND_DIV)" cond_at 76 COND_SHORT u8
    check "$_t: effective cdef list in info" sh -c 'grep -q "有効な定義マクロ（cdef）: PRODUCT=2, SIM=1, COND_A=1" "$1" && grep -q "有効な定義マクロ（--cdef）: COND_DIV=2" "$1"' sh "$WORK/cond_info.log"
    "$COND_SIM" run $COND_ARGS --cfg "$WORK/cond.cfg" "$_csv" -o "$WORK/cond_out" > "$WORK/cond_run.log" 2>&1
    check "$_t: PRODUCT=2 reads and writes the 16-bit register and the struct member" sh -c '[ "$1" -eq 0 ] && grep -F -q "総数 1, OK 1, NG 0" "$2"' sh "$?" "$WORK/cond_run.log"
    "$COND_SIM" list $COND_ARGS --cfg "$WORK/cond.cfg" --where COND_REG > "$WORK/cond_where.log" 2>&1
    check "$_t: list --where shows file and line of the active definition" contains "$WORK/cond_where.log" "cond_product.h:15]"
    check "$_t: list without --where shows the file only" sh -c 'grep -q "cond_product.h\]" "$1" && ! grep -q "cond_product.h:" "$1"' sh "$WORK/cond_list.log"
    cond_list --cdef PRODUCT=1
    check "$_t: --cdef overrides cdef in sim.cfg" cond_at 60 COND_REG u8
    # cundef と B の定義
    cond_cfg 'cdef = SIM' 'cundef = SIM' 'cdef = COND_A' 'cdef = COND_B'
    cond_list
    check "$_t: cundef removes a cdef" cond_at 67 COND_SIM u8
    check "$_t: defined(A) && !defined(B) with B defined" cond_at 69 COND_AB u8
    check "$_t: cundef shown in info" contains "$WORK/cond_info.log" "  cundef: SIM"
    # defines_cond = off: 従来どおりすべての枝を読む（最初の定義を使い、違う内容を重複として数える）
    cond_cfg 'cdef = PRODUCT=2' 'defines_cond = off'
    cond_list
    check "$_t: defines_cond = off reads every branch (first definition, first typedef)" cond_at 60 COND_REG u16
    check "$_t: defines_cond = off evaluates nothing and counts duplicates" sh -c 'grep -q "^defines cond=off roots=10 includes=0 .*if-evaluated=0 if-indeterminate=0$" "$1" && grep -q -E "duplicates=1[0-9] " "$1"' sh "$WORK/cond_info.log"
    # CS+ のプロジェクトファイル
    cond_cfg "mtpj = $COND/cond_test.mtpj"
    cond_list
    check "$_t: mtpj macro definitions of the current build mode" sh -c 'grep -q -E "^var  0x${1}60 COND_REG +u8 " "$2" && grep -q -E "^var  0x${1}66 COND_SIM +u8 " "$2"' sh "$COND_HI" "$WORK/cond_list.log"
    check "$_t: mtpj macro undefinition removes COND_A" cond_at 69 COND_AB u8
    check "$_t: mtpj value with XML escapes ((1<<4))" cond_at 82 COND_ESC u8
    check "$_t: info lists what was read from the mtpj" sh -c 'grep -q -F "mtpj macro definitions (5): PRODUCT=1, SIM, COND_A, COND_SHIFT=(1<<4), MTPJ_ONLY=0x10" "$1" && grep -q -F "mtpj macro undefinitions (1): COND_A" "$1" && grep -q "ビルド・モード DefaultBuild（2 個中 1 番目）" "$1" && grep -q "全モード: DefaultBuild, ProductB" "$1"' sh "$WORK/cond_info.log"
    check "$_t: info says the mtpj format is inferred and names the element" sh -c 'grep -q "<MacroDefinitions-0> から読みました" "$1" && grep -q "書式.*推定" "$1"' sh "$WORK/cond_info.log"
    check "$_t: assembler macro definitions are not used" lacks "$WORK/cond_info.log" "ASM_ONLY"
    cond_cfg "mtpj = $COND/cond_test.mtpj" 'mtpj_mode = ProductB' 'cdef = SIM'
    cond_list
    check "$_t: mtpj_mode selects another build mode" sh -c 'grep -q -E "^var  0x${1}62 COND_REG +u16 " "$2" && grep -q -E "^var  0x${1}68 COND_AB +u8 " "$2" && grep -q -E "^var  0x${1}6B COND_NEST +u8 " "$2"' sh "$COND_HI" "$WORK/cond_list.log"
    check "$_t: unreadable mtpj line is reported" contains "$WORK/cond_info.log" "名前として読めない行を無視しました: 9BAD LINE"
    cond_cfg
    cond_list --mtpj "$COND/cond_test.mtpj"
    check "$_t: --mtpj on the command line" cond_at 60 COND_REG u8
    cond_list --mtpj "$T/ca78k0r_probe/ca78k0r_probe.mtpj"
    check "$_t: real CS+ project file without macro definitions" sh -c 'grep -q -F "mtpj macro definitions (0): なし" "$1" && grep -q "ビルド・モード DefaultBuild（1 個中 1 番目" "$1"' sh "$WORK/cond_info.log"
    check "$_t: mtpj のマクロだけ読む場合は読む範囲の見出しを出さない" lacks "$WORK/cond_info.log" "== 読む範囲（defines_scope = mtpj）"
    # defines_scope = mtpj: プロジェクトに登録されたファイルと、#include でたどれるファイルだけを読む
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj'
    check "$_t: 定義・スイッチ・詳細の見出しを表示する" sh -c 'grep -q -x "== ヘッダの定義" "$1" && grep -q -x "== コンパイルスイッチ" "$1" && grep -q -x "== 詳細（数値）" "$1"' sh "$WORK/scope_info.log"
    check "$_t: mtpj の範囲なら読む範囲の見出しを表示する" contains "$WORK/scope_info.log" "== 読む範囲（defines_scope = mtpj）"
    check "$_t: 項目名の後に空白を置いて値を表示する" sh -c 'grep -q -E "^読んだファイル {2,}[0-9]+ 個" "$1" && grep -q -E "^取り込んだ定義 {2,}変数 [0-9]+" "$1"' sh "$WORK/scope_info.log"
    check "$_t: 重複なしでは内訳を表示しない" lacks "$WORK/scope_info.log" "  同じファイルの中"
    check "$_t: scope reads build targets of the current mode and follows include paths" sh -c 'grep -q -E "^var  0x${1}A0 SCOPE_APP " "$2" && grep -q -E "^var  0x${1}A1 SCOPE_DEV " "$2" && grep -q -E "^var  0x${1}A2 SCOPE_MODE " "$2" && grep -q -E "^var  0x${1}A3 SCOPE_REG " "$2"' sh "$COND_HI" "$WORK/scope_list.log"
    check "$_t: scope does not read unregistered files or inactive includes" sh -c '! grep -q -E "SCOPE_STRAY|SCOPE_OLD" "$1" && grep -q "duplicates=0 " "$2"' sh "$WORK/scope_list.log" "$WORK/scope_info.log"
    check "$_t: scope info counts registered / excluded / missing / followed files" sh -c 'grep -q "登録ファイル  *5 個（読んだ 2、.h / .c / .inc 以外 1、ビルド対象外 1、除外 0、見つからない 1）" "$1" && grep -q "#include から足した  *3 個" "$1"' sh "$WORK/scope_info.log"
    check "$_t: scope info lists the include paths of the compiler, not the assembler" sh -c 'grep -q "インクルード・パス  *2 個（mtpj 2、defines_include 0）" "$1" && ! grep -q "asm_only" "$1" && grep -q "common/gone.c" "$1"' sh "$WORK/scope_info.log"
    check "$_t: scope ignores #pragma pack in a header that is only included from an inactive branch" sh -c 'grep -q -E "^var  0x${1}A7 SCOPE_PK " "$2"' sh "$COND_HI" "$WORK/scope_list.log"
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj' 'cdef = USE_OLD'
    check "$_t: scope applies #pragma pack once the header is read through an active branch" sh -c 'grep -q "SCOPE_OLD" "$1" && ! grep -q "SCOPE_PK" "$1"' sh "$WORK/scope_list.log"
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj' 'mtpj_mode = ProductB'
    check "$_t: scope follows the build mode (other folder with the same file names)" sh -c 'grep -q -E "^var  0x${1}B1 SCOPE_DEV " "$2" && grep -q -E "^var  0x${1}B2 SCOPE_MODE " "$2" && grep -q "duplicates=0 " "$3"' sh "$COND_HI" "$WORK/scope_list.log" "$WORK/scope_info.log"
    check "$_t: scope expands %BuildModeName% and flags a missing include folder" contains "$WORK/scope_info.log" "no_such_ProductB（フォルダがありません）"
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj' 'mtpj_mode = NoInc'
    check "$_t: 見つからない include の箇所数と種類数" contains "$WORK/scope_info.log" "見つからない #include     1 箇所、1 種類"
    check "$_t: 見つからない include の名前と回数" contains "$WORK/scope_info.log" "reg.h × 1"
    check "$_t: 見つからない include の最初の場所" contains "$WORK/scope_info.log" "（最初: $SCOPE/common/app.c:3）"
    check "$_t: scope without include paths falls back to the file name and says so" sh -c 'grep -q "名前で探した #include  *1 箇所" "$1" && ! grep -q "SCOPE_REG" "$2" && grep -q -E "SCOPE_MODE" "$2"' sh "$WORK/scope_info.log" "$WORK/scope_list.log"
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj' 'mtpj_mode = NoInc' "defines_include = $SCOPE/inc"
    check "$_t: defines_include adds an include path" sh -c 'grep -q -E "^var  0x${1}A3 SCOPE_REG " "$2" && grep -q "defines_include 1）" "$3"' sh "$COND_HI" "$WORK/scope_list.log" "$WORK/scope_info.log"
    scope_run "mtpj = $SCOPE/proj/scope_test.mtpj" 'defines_scope = mtpj' "defines = $SCOPE/unused/stray.h" 'defines_exclude = prodA/dev.c'
    check "$_t: scope keeps defines = (added) and defines_exclude (removed)" sh -c 'grep -q "SCOPE_STRAY" "$1" && ! grep -q "SCOPE_DEV" "$1" && grep -q "除外 1、" "$2"' sh "$WORK/scope_list.log" "$WORK/scope_info.log"
    scope_run "defines = $SCOPE"
    check "$_t: フォルダを読む場合は読む範囲の見出しを出さない" lacks "$WORK/scope_info.log" "== 読む範囲（defines_scope = mtpj）"
    check "$_t: without scope the whole folder is read (duplicates)" sh -c 'grep -q "SCOPE_STRAY" "$1" && grep -q -E "duplicates=[1-9]" "$2"' sh "$WORK/scope_list.log" "$WORK/scope_info.log"
    check "$_t: 同名で別フォルダの重複がある" sh -c 'grep -q -E "^  同じ名前で別のフォルダ  +[1-9][0-9]*  （品種違い" "$1"' sh "$WORK/scope_info.log"
    check "$_t: 重複の内訳の合計が duplicates と一致する" sh -c '
        total=$(sed -n "s/^defines files=.* duplicates=\([0-9]*\) .*/\1/p" "$1")
        parts=$(sed -n -e "s/^  同じファイルの中  *\([0-9][0-9]*\).*/\1/p" -e "s/^  同じ名前で別のフォルダ  *\([0-9][0-9]*\).*/\1/p" -e "s/^  名前の違うファイル同士  *\([0-9][0-9]*\).*/\1/p" "$1")
        [ -n "$total" ] && [ -n "$parts" ] || exit 1
        set -- $parts
        [ "$#" -eq 3 ] || exit 1
        [ $(($1 + $2 + $3)) -eq "$total" ]' sh "$WORK/scope_info.log"
    # ガードのない同じヘッダを、別の起点から違う条件で読む
    scope_run "defines = $T/defines_dup"
    check "$_t: 同じファイルの異なる枝の重複は 1 件" sh -c 'grep -q "duplicates=1 " "$1" && grep -q "^  同じファイルの中  *1  （インクルード元" "$1" && grep -q "^  同じ名前で別のフォルダ  *0$" "$1" && grep -q "^  名前の違うファイル同士  *0$" "$1"' sh "$WORK/scope_info.log"
    # 一覧は有効な枝だけ、走査時の統計は無効な枝も含める
    scope_run "defines = $T/defines_missing/active"
    check "$_t: 有効な枝の見つからない include を一覧に出す" contains "$WORK/scope_info.log" "no_such_active.h × 1"
    check "$_t: 無効な枝の見つからない include は一覧に出さない" lacks "$WORK/scope_info.log" "no_such_inactive.h"
    check "$_t: 有効な枝だけの箇所数と種類数" contains "$WORK/scope_info.log" "見つからない #include     1 箇所、1 種類"
    check "$_t: 走査時の統計は無効な枝も含めて 2 箇所" contains "$WORK/scope_info.log" "include-missing=2 "
    check "$_t: 条件付きコンパイルの説明も無効な枝を含めて 2 箇所" contains "$WORK/scope_info.log" "無効な枝を含めると 2 箇所"
    scope_run "defines = $T/defines_missing/active" 'cdef = NEVER_DEFINED'
    check "$_t: cdef で有効にした枝の include も一覧に出す" contains "$WORK/scope_info.log" "no_such_inactive.h × 1"
    check "$_t: cdef で有効になった後も元の include は 1 箇所" contains "$WORK/scope_info.log" "no_such_active.h × 1"
    check "$_t: 両方の枝が有効なら一覧も 2 箇所 2 種類" contains "$WORK/scope_info.log" "見つからない #include     2 箇所、2 種類"
    check "$_t: cdef でも走査時の統計は 2 箇所のまま" contains "$WORK/scope_info.log" "include-missing=2 "
    scope_run "defines = $T/defines_missing/inactive"
    check "$_t: すべて無効な枝なら一覧の代わりに説明を出す" contains "$WORK/scope_info.log" "見つからない #include     0 箇所（有効な枝のみ。無効な枝を含めると 1 箇所）"
    check "$_t: すべて無効な枝なら include の名前は出さない" lacks "$WORK/scope_info.log" "no_such_inactive.h"
    check "$_t: すべて無効な枝でも走査時の統計は 1 箇所" contains "$WORK/scope_info.log" "include-missing=1 "
    for _bad in 'defines_scope = mtpj' 'defines_scope = all' "defines_include = $SCOPE/inc"; do
        scope_run "$_bad"
        check "$_t: rejects $_bad without what it needs (exit 2)" test "$SCOPE_RC" -eq 2
    done
    for _bad in 'mtpj_mode = NoSuchMode' 'cdef = 1BAD' 'cdef = A B' 'cundef = A=1' 'defines_cond = maybe' 'mtpj = no_such.mtpj'; do
        if [ "$_bad" = 'mtpj_mode = NoSuchMode' ]; then cond_cfg "mtpj = $COND/cond_test.mtpj" "$_bad"; else cond_cfg "$_bad"; fi
        "$COND_SIM" info $COND_ARGS --cfg "$WORK/cond.cfg" > "$WORK/cond_info.log" 2>&1
        check "$_t: rejects $_bad (exit 2)" test $? -eq 2
    done
}

echo "== ビルド"
if [ -z "${SIM:-}" ]; then
    if "$ROOT/build.sh" mock "$WORK/build" > "$WORK/build.log" 2>&1; then ok "build.sh mock"; else ng "build.sh mock"; cat "$WORK/build.log"; fi
    if grep -q -i "warning" "$WORK/build.log"; then ng "警告なしでビルド" "$(grep -i warning "$WORK/build.log" | head -3)"; else ok "警告なしでビルド"; fi
    SIM="$WORK/build/sim"
fi
if [ ! -x "$SIM" ]; then echo "sim がありません: $SIM"; echo "実行 $((PASS + FAIL)) 件、失敗 $((FAIL + 1)) 件"; exit 1; fi

# 結果 CSV を期待する結果とバイト単位で比べる
run_cmp() {
    _csv=$1; _code=$2; _out="$WORK/out_$(basename "$_csv" .csv)"
    "$SIM" run "$T/csv/$_csv" -o "$_out" --fixed-time --quiet > "$WORK/run.log" 2>&1
    _rc=$?
    if [ "$_rc" -eq "$_code" ]; then ok "$_csv: 終了コード $_code"; else ng "$_csv: 終了コード $_code" "実際は $_rc: $(tail -3 "$WORK/run.log")"; fi
    if cmp -s "$_out/$_csv" "$T/expected/$_csv"; then ok "$_csv: 結果 CSV が期待どおり（バイト単位）"
    else ng "$_csv: 結果 CSV が期待どおり（バイト単位）" "$(diff "$T/expected/$_csv" "$_out/$_csv" | head -5)"; fi
}


check "build GUI CSV table checks" "${CC:-clang}" -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" \
    "$T/tools/check_csv_table.c" "$ROOT/runtime/src/sim_csv.c" "$ROOT/runtime/src/sim_expr.c" "$ROOT/runtime/src/sim_util.c" "$ROOT/runtime/src/sim_platform.c" "$ROOT/runtime/src/sim_cfg.c" "$ROOT/runtime/backends/mock/mock_backend.c" -o "$WORK/csv_table" -lpthread -lm
check "GUI CSV initialization expansion" "$WORK/csv_table"
check "build const argument checks" "${CC:-clang}" -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" \
    "$T/tools/check_const_refs.c" "$ROOT/runtime/src/sim_expr.c" "$ROOT/runtime/src/sim_util.c" "$ROOT/runtime/src/sim_platform.c" "$ROOT/runtime/src/sim_cfg.c" "$ROOT/runtime/backends/mock/mock_backend.c" -o "$WORK/const_refs" -lpthread -lm
check "mock const argument preparation and symbol writable" "$WORK/const_refs"

echo "== header defines (mock)"
"$SIM" run "$T/defines/mock.csv" --cfg "$T/defines/mock.cfg" -o "$WORK/header_mock" > "$WORK/header_mock.log" 2>&1
RC=$?
check "header names and modi initializers mock" test "$RC" -eq 0
if [ "$RC" -ne 0 ]; then cat "$WORK/header_mock.log"; fi
"$SIM" list --cfg "$T/defines/mock.cfg" > "$WORK/header_list.log" 2>&1
check "header names in list mock" contains "$WORK/header_list.log" "df_unit"
check "constant member index is registered" sh -c 'grep -F -q unsupported_index "$1"' sh "$WORK/header_list.log"
"$SIM" list --cfg "$T/defines/expansion.cfg" > "$WORK/expansion.log" 2>&1
check "unparenthesized macro array has five elements" contains "$WORK/expansion.log" "u8[5]"
"$SIM" list --cfg "$T/defines/encoding.cfg" > "$WORK/encoding.log" 2>&1
check "UTF8 comment preserves next definition" contains "$WORK/encoding.log" "UTF8_REG"
echo "== defines: text and malformed input contracts"
TEXT_SRCS="$T/tools/check_defines_text.c $ROOT/runtime/src/sim_defines.c $ROOT/runtime/src/sim_ppexpr.c $ROOT/runtime/src/sim_mtpj.c $ROOT/runtime/src/sim_expr.c $ROOT/runtime/src/sim_cfg.c $ROOT/runtime/src/sim_util.c $ROOT/runtime/src/sim_platform.c $ROOT/runtime/backends/mock/mock_backend.c"
# shellcheck disable=SC2086
check "build defines text checks" "${CC:-clang}" -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" $TEXT_SRCS -o "$WORK/check_defines_text" -lpthread -lm
check "definition addresses counts and malformed inputs" "$WORK/check_defines_text" "$T" "$WORK"
# shellcheck disable=SC2086
check "build defines text ASan UBSan" "${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -D_DEFAULT_SOURCE -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" $TEXT_SRCS -o "$WORK/check_defines_sanitize" -lpthread -lm
check "definition text ASan UBSan 200 malformed inputs" "$WORK/check_defines_sanitize" "$T" "$WORK"
echo "== defines: const paths reject CSV inputs"
while read -r const_name; do
    printf 'mod,TEST_cycle,const path,1,0\n#COMMENT,%s\n,123\n' "$const_name" > "$WORK/const_path.csv"
    "$SIM" run "$WORK/const_path.csv" --cfg "$T/defines/type_contract.cfg" -o "$WORK/const_path" > "$WORK/const_path.log" 2>&1
    const_rc=$?
    check "const CSV $const_name exits 2" test "$const_rc" -eq 2
done < "$T/defines/type_contract_ro.txt"
echo "== defines: value expressions (mock)"
"$SIM" run "$T/defines/value_forms.csv" --cfg "$T/defines/value_forms.cfg" -o "$WORK/value_good" > "$WORK/value_good.log" 2>&1
RC=$?
check "direct lvalues typed CSV and physical offsets" sh -c '[ "$1" -eq 0 ] && grep -F -q "総数 1, OK 1, NG 0" "$2"' sh "$RC" "$WORK/value_good.log"

"$SIM" info -v --cfg "$T/defines/value_forms.cfg" > "$WORK/value_info.log" 2>&1
RC=$?
check "value forms load" test "$RC" -eq 0
check "value forms counted separately" contains "$WORK/value_info.log" "value-excluded=18"
while read -r value_name; do
    check "$value_name verbose source" grep -E -q "対象外）: $value_name .*value_forms.h:[0-9]+" "$WORK/value_info.log"
    printf 'mod,intr_tm00,rejected,1,0\n#COMMENT,%s\n,1\n' "$value_name" > "$WORK/value_bad.csv"
    "$SIM" run "$WORK/value_bad.csv" --cfg "$T/defines/value_forms.cfg" -o "$WORK/value_bad" > "$WORK/value_bad.log" 2>&1
    RC=$?
    check "$value_name CSV unusable" test "$RC" -eq 2
done < "$T/defines/value_rejected.txt"
echo "== defines: declarators (mock)"
for decl_csv in declarators declarator_supported declarator_members; do
    if [ "$decl_csv" = declarators ]; then decl_cfg=declarators; else decl_cfg=declarator_supported; fi
    "$SIM" run "$T/defines/$decl_csv.csv" --cfg "$T/defines/$decl_cfg.cfg" -o "$WORK/decl" > "$WORK/decl.log" 2>&1
    RC=$?
    check "$decl_csv CSV typed read write and physical offsets" sh -c '[ "$1" -eq 0 ] && grep -F -q "総数 1, OK 1, NG 0" "$2"' sh "$RC" "$WORK/decl.log"
    if [ "$RC" -ne 0 ]; then cat "$WORK/decl.log"; fi
done
"$SIM" list --cfg "$T/defines/declarators.cfg" > "$WORK/decl_list.log" 2>&1
check "long long is u64" grep -E -q 'WIDE +u64 ' "$WORK/decl_list.log"
"$SIM" list --cfg "$T/defines/declarator_supported.cfg" > "$WORK/decl_list.log" 2>&1
while read -r decl_name decl_type; do
    check "$decl_name type $decl_type" grep -E -q " $decl_name +$decl_type " "$WORK/decl_list.log"
done < "$T/defines/declarator_types.txt"
"$SIM" info -v --cfg "$T/defines/declarator_rejected.cfg" > "$WORK/decl_info.log" 2>&1
RC=$?
check "unsupported declarations are unresolved not load errors" test "$RC" -eq 0
check "all 50 unsupported declarations counted" contains "$WORK/decl_info.log" "unresolved=50 "
check "void pointer constant is skipped as a pointer" contains "$WORK/decl_info.log" "pointer-skipped=1 "
"$SIM" list --cfg "$T/defines/declarator_rejected.cfg" > "$WORK/decl_list.log" 2>&1
decl_reject() {
    lacks "$WORK/decl_list.log" " $1 " && contains "$WORK/decl_info.log" "定義 $1 (" || return 1
    printf 'mod,intr_tm00,rejected,1,0\n#COMMENT,%s\n,1\n' "$1" > "$WORK/decl_bad.csv"
    "$SIM" run "$WORK/decl_bad.csv" --cfg "$T/defines/declarator_rejected.cfg" -o "$WORK/decl_bad" > "$WORK/decl_bad.log" 2>&1
    _decl_rc=$?
    [ "$_decl_rc" -eq 2 ]
}
while read -r decl_name; do check "$decl_name counted and unusable" decl_reject "$decl_name"; done < "$T/defines/declarator_rejected.txt"
for decl_mode in packed lmf; do
    "$SIM" info -v --cfg "$T/defines/declarator_$decl_mode.cfg" > "$WORK/decl_info.log" 2>&1
    RC=$?
    check "$decl_mode unsupported types counted without load errors" sh -c '[ "$1" -eq 0 ] && grep -F -q "unresolved=2 " "$2"' sh "$RC" "$WORK/decl_info.log"
    "$SIM" list --cfg "$T/defines/declarator_$decl_mode.cfg" > "$WORK/decl_list.log" 2>&1
    if [ "$decl_mode" = packed ]; then decl_names="DECLPACKED DECLEXPLICITPACKED"; else decl_names="WIDE SIGNEDWIDE"; fi
    for decl_name in $decl_names; do
        printf 'mod,intr_tm00,rejected,1,0\n#COMMENT,%s\n,1\n' "$decl_name" > "$WORK/decl_bad.csv"
        "$SIM" run "$WORK/decl_bad.csv" --cfg "$T/defines/declarator_$decl_mode.cfg" -o "$WORK/decl_bad" > "$WORK/decl_bad.log" 2>&1
        RC=$?
        check "$decl_mode $decl_name unusable" sh -c '[ "$1" -eq 2 ] && ! grep -F -q " $2 " "$3"' sh "$RC" "$decl_name" "$WORK/decl_list.log"
    done
done
echo "== defines: #if の式の評価器（単体テスト）"
check "build check_ppexpr" "${CC:-clang}" -std=c11 -O2 -Wall -Wextra -I"$ROOT/runtime/src" "$T/tools/check_ppexpr.c" "$ROOT/runtime/src/sim_ppexpr.c" -o "$WORK/check_ppexpr"
"$WORK/check_ppexpr" > "$WORK/ppexpr.log" 2>&1
RC=$?
check "preprocessor expression evaluator boundary cases" test "$RC" -eq 0
check "preprocessor expression evaluator ran its cases (0 failed)" grep -q -E "^ppexpr: [1-9][0-9]* cases, 0 failed" "$WORK/ppexpr.log"
if [ "$RC" -ne 0 ]; then cat "$WORK/ppexpr.log"; fi
echo "== defines: コンパイルスイッチ（モック）"
cond_suite "cond mock" "$SIM" 0F9 "$T/defines/cond_mock.csv"
cond_cfg
cond_list
check "cond mock: no implicit macros without a load module" cond_at 7F COND_K0R u8
check "build gen_defines_bench" "${CC:-clang}" -std=c11 -O2 -Wall -Wextra "$T/tools/gen_defines_bench.c" -o "$WORK/gen_defines_bench"
"$WORK/gen_defines_bench" "$WORK/bench" 200 40 > "$WORK/bench_gen.log" 2>&1
printf 'defines = %s\ncdef = PRODUCT=2\n' "$WORK/bench" > "$WORK/bench.cfg"
"$SIM" info --cfg "$WORK/bench.cfg" > "$WORK/bench_info.log" 2>&1
check "generated 200-file tree: all 4800 macros resolved, includes reused" sh -c 'grep -q "^defines files=200 variables=3240 bits=1560 .*unresolved=0 duplicates=0 " "$1" && grep -q -E "include-reused=[1-9]" "$1"' sh "$WORK/bench_info.log"
grep "^defines time:" "$WORK/bench_info.log"
echo "== テスト CSV の実行と結果 CSV"
run_cmp unit_all.csv 0
run_cmp ng_case.csv 1
run_cmp sjis_comment.csv 0
run_cmp cycle_2000.csv 0

echo "== 件数の表示"
"$SIM" run "$T/csv/unit_all.csv" -o "$WORK/o_cnt" --quiet > "$WORK/cnt.log" 2>&1
check "unit_all の件数（総数 35, OK 35）" contains "$WORK/cnt.log" "合計: 総数 35, OK 35, NG 0, NO Check 0"
"$SIM" run "$T/csv/ng_case.csv" -o "$WORK/o_cnt" > "$WORK/cnt2.log" 2>&1
check "ng_case の件数（NG 3, NO Check 1）" contains "$WORK/cnt2.log" "合計: 総数 6, OK 2, NG 3, NO Check 1"
check "NG の詳細を表示" contains "$WORK/cnt2.log" "NG ng_case.csv:4 eng_judge_gear@@ = 2（期待 3）"
check "実行時間の列（ms）" grep -q -E ',OK,[0-9]+\.[0-9]{4}ms' "$WORK/o_cnt/unit_all.csv"

echo "== 2000 行の周期実行"
"$SIM" run "$T/csv/cycle_2000.csv" -o "$WORK/o_cycle" --quiet > "$WORK/cycle.log" 2>&1
check "cycle_2000: 2100 行がすべて OK" contains "$WORK/cycle.log" "合計: 総数 2100, OK 2100, NG 0"
CYCLE_TIME=$(grep "実行時間" "$WORK/cycle.log")
echo "        2100 行の $CYCLE_TIME"

echo "== 結果 CSV の読み直し"
"$SIM" run "$WORK/out_unit_all/unit_all.csv" -o "$WORK/rb1" --fixed-time --quiet > "$WORK/rb.log" 2>&1
check "unit_all の結果を入力にして同じ結果" cmp -s "$WORK/rb1/unit_all.csv" "$T/expected/unit_all.csv"
"$SIM" run "$WORK/out_ng_case/ng_case.csv" -o "$WORK/rb2" --fixed-time > "$WORK/rb.log" 2>&1
RC=$?
check "NG の結果を読み直すと期待側を採る（終了コード 1）" test "$RC" -eq 1
check "NG の結果の読み直し: NG はそのまま、期待値なしだった行は実測が期待値になる" contains "$WORK/rb.log" "総数 6, OK 3, NG 3, NO Check 0"
"$SIM" run "$WORK/out_ng_case/ng_case.csv" -o "$WORK/rb3" --fixed-time --expect-actual --quiet > "$WORK/rb3.log" 2>&1
RC=$?
check "--expect-actual で実測側を採ると NG なし（終了コード 0）" test "$RC" -eq 0
check "--expect-actual の件数" contains "$WORK/rb3.log" "総数 6, OK 6, NG 0, NO Check 0"

echo "== Shift_JIS と改行コード"
grep -a '^;' "$T/csv/sjis_comment.csv" > "$WORK/sj_in.txt"
grep -a '^;' "$WORK/out_sjis_comment/sjis_comment.csv" > "$WORK/sj_out.txt"
check "Shift_JIS のコメント行がバイトそのまま" cmp -s "$WORK/sj_in.txt" "$WORK/sj_out.txt"
head -1 "$T/csv/sjis_comment.csv" > "$WORK/sj_mod_in.txt"
head -1 "$WORK/out_sjis_comment/sjis_comment.csv" > "$WORK/sj_mod_out.txt"
check "Shift_JIS の mod 行（題名と右のセル）がそのまま" cmp -s "$WORK/sj_mod_in.txt" "$WORK/sj_mod_out.txt"
CR_IN=$(LC_ALL=C tr -cd '\r' < "$T/csv/sjis_comment.csv" | wc -c | tr -d ' ')
CR_OUT=$(LC_ALL=C tr -cd '\r' < "$WORK/out_sjis_comment/sjis_comment.csv" | wc -c | tr -d ' ')
check "CRLF の改行が保たれる（$CR_IN 個）" test "$CR_IN" -eq "$CR_OUT" -a "$CR_IN" -gt 0
CR_U=$(LC_ALL=C tr -cd '\r' < "$WORK/out_unit_all/unit_all.csv" | wc -c | tr -d ' ')
check "LF の改行は LF のまま" test "$CR_U" -eq 0

echo "== エラー系（終了コード 2 と分かるメッセージ）"
err_case() {
    _f=$1; _msg=$2
    "$SIM" run "$T/csv/err/$_f" -o "$WORK/oerr" --quiet > "$WORK/err.log" 2>&1
    _rc=$?
    if [ "$_rc" -eq 2 ] && grep -F -q -- "$_msg" "$WORK/err.log"; then ok "$_f"
    else ng "$_f" "終了コード ${_rc}、出力: $(head -2 "$WORK/err.log")"; fi
}
err_case unknown_var.csv "変数 'g_engx' が見つかりません（候補: g_eng）"
err_case unknown_func.csv "関数 'no_such_func' が見つかりません"
err_case unknown_arg.csv "引数 'speed' はありません（引数: @rpm, @temp, @cur_gear）"
err_case float_arg.csv "浮動小数の引数には対応していません"
err_case bad_value.csv "値 '12abc' を数値として読めません"
err_case struct_no_member.csv "メンバを指定してください"
err_case ambiguous_static.csv "engine.c/s_cnt sensor.c/s_cnt"
err_case write_const.csv "'g_lut[0]' には書き込めません"
err_case index_range.csv "添字 5 は配列の要素数 5 の範囲外です"
err_case overflow.csv "値 300 は収まりません"
err_case null_deref.csv "NULL ポインタの先は読み書きできません"
err_case bad_mod.csv "入力数）'x' が数値ではありません"
# 64 ビットを超える 10 進は飽和させて別の値（-1 = 255）にしない
err_case big_decimal.csv "4 列目 'g_mode': 'g_mode' に書く値が 64 ビットの整数の範囲を超えています"

echo "== 文字コードと行の識別（ブロックを黙って無視しない）"
# UTF-8 の BOM は行の識別では読み飛ばし、結果 CSV には BOM を含めて元のバイト列のまま写す
run_cmp bom_mul16.csv 1
"$SIM" run "$T/csv/bom_mul16.csv" -o "$WORK/obom" --quiet > "$WORK/bom.log" 2>&1
check "BOM 付きの CSV の先頭ブロックを実行する（総数 2, OK 1, NG 1）" contains "$WORK/bom.log" "合計: 総数 2, OK 1, NG 1, NO Check 0"
check "BOM 付きの結果 CSV の先頭 3 バイトが BOM のまま" sh -c "[ \"\$(head -c 3 '$WORK/out_bom_mul16/bom_mul16.csv' | od -An -tx1 | tr -d ' \n')\" = efbbbf ]"
"$SIM" run "$WORK/out_bom_mul16/bom_mul16.csv" -o "$WORK/obom_rb" --fixed-time --quiet > "$WORK/bom_rb.log" 2>&1
RC=$?
check "BOM 付きの結果 CSV を読み直しても同じ結果（終了コード 1、総数 2, NG 1）" sh -c "[ $RC -eq 1 ] && grep -F -q '合計: 総数 2, OK 1, NG 1, NO Check 0' '$WORK/bom_rb.log' && cmp -s '$WORK/obom_rb/bom_mul16.csv' '$T/expected/bom_mul16.csv'"
err_case utf16le_bom.csv "UTF-16 の CSV には対応していません"
err_case utf16be_bom.csv "UTF-16 の CSV には対応していません"
err_case nul_byte.csv "nul_byte.csv:3: NUL バイト（0x00）を含む行があります"
err_case no_mod.csv "no_mod.csv: mod 行がありません"
err_case no_comment.csv "mod 'mul16' に #COMMENT 行がありません"
# 大文字小文字・前後の空白とタブ・二重引用符・行頭の全角空白（UTF-8 / Shift_JIS）・末尾の余分なカンマを受け付ける
run_cmp lenient_ident.csv 0
"$SIM" run "$T/csv/lenient_ident.csv" -o "$WORK/olen" --quiet > "$WORK/len.log" 2>&1
check "mod / #COMMENT の表記揺れの 8 ブロックをすべて実行する（総数 8, OK 8）" contains "$WORK/len.log" "合計: 総数 8, OK 8, NG 0, NO Check 0"
# CR だけの改行と、改行のない最終行
run_cmp cr_only.csv 0
"$SIM" run "$T/csv/cr_only.csv" -o "$WORK/ocr" --quiet > "$WORK/cr.log" 2>&1
check "CR だけの改行・改行のない最終行も実行する（総数 2, OK 2）" contains "$WORK/cr.log" "合計: 総数 2, OK 2, NG 0, NO Check 0"
# 実行するデータ行が 0 行のブロックはエラーにせず警告（コメントアウトだけの雛形を壊さない）
run_cmp only_comments.csv 0
"$SIM" run "$T/csv/only_comments.csv" -o "$WORK/ooc" --quiet > "$WORK/oc.log" 2>&1
check "データ行が 0 行のブロックを警告する" contains "$WORK/oc.log" "警告: $T/csv/only_comments.csv:1: mod 'mul16' に実行するデータ行がありません"
# 最初の mod 行より前のデータらしい行は警告する
run_cmp data_before_mod.csv 0
"$SIM" run "$T/csv/data_before_mod.csv" -o "$WORK/odb" --quiet > "$WORK/db.log" 2>&1
check "最初の mod 行より前のデータらしい行を警告する" contains "$WORK/db.log" "警告: $T/csv/data_before_mod.csv:1: 最初の mod 行より前に、どのテストにも属さない行があります（1〜2 行目）"
# 複数のファイル: ほかのファイルは実行を続け、最後の終了コードに反映する
"$SIM" run "$T/csv/err/no_mod.csv" "$T/csv/unit_all.csv" -o "$WORK/omulti" --quiet > "$WORK/multi.log" 2>&1
RC=$?
check "mod 行のないファイルと正常なファイル: 正常な方は実行して終了コード 2" sh -c "[ $RC -eq 2 ] && [ -f '$WORK/omulti/unit_all.csv' ] && grep -F -q '合計: 総数 35, OK 35, NG 0, NO Check 0, エラー 1' '$WORK/multi.log'"
"$SIM" run "$T/csv/only_comments.csv" "$T/csv/ng_case.csv" -o "$WORK/omulti2" --quiet > "$WORK/multi2.log" 2>&1
RC=$?
check "データ行が 0 行のファイルと NG のファイル: 警告は終了コードを変えない（1）" test "$RC" -eq 1

echo "== 数値の範囲と表記（黙って別の値にしない）"
"$SIM" run "$T/csv/err/num_range.csv" -o "$WORK/onr" --quiet > "$WORK/nr.log" 2>&1
RC=$?
check "範囲外の値・添字・アドレスはそれぞれエラー（終了コード 2、エラー 7）" sh -c "[ $RC -eq 2 ] && grep -F -q 'エラー 7' '$WORK/nr.log'"
check "INT64_MAX を超える 10 進は 8 ビットに収まらない（-1 = 255 にしない）" contains "$WORK/nr.log" "'g_mode' は 8 ビットです。値 18446744073709551615 は収まりません"
check "-0xFFFFFFFFFFFFFFFF を折り返して 1 にしない" contains "$WORK/nr.log" "num_range.csv:6: 2 列目 'g_mode': 'g_mode' に書く値が 64 ビットの整数の範囲を超えています"
check "1e400 を inf にしない" contains "$WORK/nr.log" "'g_fan_duty' に書く値が double の範囲を超えています"
check "float に収まらない 1e39 を inf にしない" contains "$WORK/nr.log" "'g_fan_duty' は float です。値 1e+39 は収まりません"
check "添字 010 を 8 進の 8 と読まない" contains "$WORK/nr.log" "添字 '010' が 0 で始まる 10 進です"
check "64 ビットを超える添字を飽和させない" contains "$WORK/nr.log" "添字 '99999999999999999999' が大きすぎます"
check "アドレス 0100 を 8 進の 0x40 と読まない" contains "$WORK/nr.log" "'0100:1': アドレスが 0 で始まる 10 進です"
run_cmp expect_num_range.csv 1
"$SIM" run "$T/csv/expect_num_range.csv" -o "$WORK/oenr" --quiet > "$WORK/enr.log" 2>&1
check "範囲外の期待値は NG、INT64_MAX を超える 10 進は 64 ビットで一致（総数 6, OK 2, NG 4）" contains "$WORK/enr.log" "合計: 総数 6, OK 2, NG 4, NO Check 0"
"$SIM" run "$WORK/nonexistent.csv" -o "$WORK/oerr" > "$WORK/err.log" 2>&1
RC=$?
check "存在しない CSV（終了コード 2）" test "$RC" -eq 2
"$SIM" run "$T/csv/unit_all.csv" "$T/csv/err/unknown_var.csv" -o "$WORK/omix" --quiet > "$WORK/mix.log" 2>&1
RC=$?
check "正常とエラーが混在すると 2、正常な方は結果を書く" test "$RC" -eq 2 -a -f "$WORK/omix/unit_all.csv"
"$SIM" run "$T/csv/unit_all.csv" -o "$T/csv" --quiet > "$WORK/same.log" 2>&1
RC=$?
check "入力と同じ場所には書かない" test "$RC" -eq 2
check "入力が上書きされていない" grep -q "^mod,eng_judge_gear,ギア段判定,4,2,右のセルも保つ,x$" "$T/csv/unit_all.csv"

echo "== 入力と同じファイルには書かない（パスの表記が違っても）"
mkdir -p "$WORK/same" "$WORK/other"
ln -s "$WORK/same" "$WORK/same_link" 2>/dev/null
# 入力を作り直し、sim run を実行して「終了コード 2 で入力がそのまま」かを確かめる
same_case() {
    _name=$1; shift
    cp "$T/csv/unit_all.csv" "$WORK/same/unit_all.csv"
    (cd "$WORK/same" && "$SIM" run "$@" --quiet > "$WORK/same.log" 2>&1)
    _rc=$?
    if [ "$_rc" -eq 2 ] && cmp -s "$WORK/same/unit_all.csv" "$T/csv/unit_all.csv" && { contains "$WORK/same.log" "入力と同じです" || contains "$WORK/same.log" "出力先が衝突します"; }; then ok "$_name"
    else ng "$_name" "終了コード ${_rc}、出力: $(head -2 "$WORK/same.log")"; fi
}
same_case "入力を ./ 付きで指定（/x/./a.csv -o /x）" "$WORK/same/./unit_all.csv" -o "$WORK/same"
same_case "入力を相対、出力を絶対で指定" unit_all.csv -o "$WORK/same"
same_case "出力を相対（.）で指定" "$WORK/same/unit_all.csv" -o .
same_case "出力を .. を含むパスで指定" "$WORK/same/unit_all.csv" -o "$WORK/same/sub/.."
same_case "出力をシンボリックリンク経由で指定" "$WORK/same/unit_all.csv" -o "$WORK/same_link"
same_case "後の入力を前の結果で上書きしない（複数の入力）" "$T/csv/unit_all.csv" "$WORK/same/unit_all.csv" -o "$WORK/same"
rm -rf "$WORK/fresh"
"$SIM" run "$WORK/same/./unit_all.csv" -o "$WORK/fresh/./" --fixed-time --quiet > "$WORK/fresh.log" 2>&1
RC=$?
check "出力先にまだファイルがなければ書く（終了コード 0）" test "$RC" -eq 0 -a -f "$WORK/fresh/unit_all.csv"
cp "$T/csv/unit_all.csv" "$WORK/other/unit_all.csv"
"$SIM" run "$T/csv/unit_all.csv" -o "$WORK/other" --fixed-time --quiet > "$WORK/other.log" 2>&1
RC=$?
check "入力と同じ内容の別ファイルは上書きする（終了コード 0）" sh -c "[ $RC -eq 0 ] && cmp -s '$WORK/other/unit_all.csv' '$T/expected/unit_all.csv'"

echo "== 複数入力の出力先衝突（実行前に拒否）"
mkdir -p "$WORK/collision/a" "$WORK/collision/b" "$WORK/collision/c"
cp "$T/csv/sjis_comment.csv" "$WORK/collision/a/test.csv"
cp "$T/csv/sjis_comment.csv" "$WORK/collision/b/test.csv"
cp "$T/csv/sjis_comment.csv" "$WORK/collision/c/Test.csv"
cp "$T/csv/sjis_comment.csv" "$WORK/collision/b/other.csv"
collision_case() {
    collision_name=$1; _second=$2; _out="$WORK/collision/out_$3"
    "$SIM" run "$WORK/collision/a/test.csv" "$_second" -o "$_out" --quiet > "$WORK/collision.log" 2>&1
    _rc=$?
    check "出力衝突: $collision_name は終了コード 2" test "$_rc" -eq 2
    check "出力衝突: $collision_name は出力フォルダも作らない" test ! -e "$_out"
    check "出力衝突: $collision_name は実行しない" sh -c '! grep -q "合計:" "$1"' sh "$WORK/collision.log"
    check "出力衝突: $collision_name の入力 1 を表示" contains "$WORK/collision.log" "$WORK/collision/a/test.csv"
    check "出力衝突: $collision_name の入力 2 を表示" contains "$WORK/collision.log" "$_second"
    check "出力衝突: $collision_name の出力先を表示" contains "$WORK/collision.log" "$_out/test.csv"
    check "出力衝突: $collision_name の対処を案内" contains "$WORK/collision.log" "別々に実行するか、出力フォルダを分けてください"
}
collision_case "同名" "$WORK/collision/b/test.csv" same
collision_case "大文字小文字違い" "$WORK/collision/c/Test.csv" case
collision_case "同一入力の重複" "$WORK/collision/a/test.csv" duplicate
"$SIM" run "$WORK/collision/a/test.csv" "$WORK/collision/b/other.csv" -o "$WORK/collision/distinct" --fixed-time --quiet > "$WORK/collision.log" 2>&1
RC=$?
check "出力衝突: 別名なら終了コード 0" test "$RC" -eq 0
check "出力衝突: 別名の入力 1 は従来どおり実行" cmp -s "$WORK/collision/distinct/test.csv" "$T/expected/sjis_comment.csv"
check "出力衝突: 別名の入力 2 も従来どおり実行" cmp -s "$WORK/collision/distinct/other.csv" "$T/expected/sjis_comment.csv"

echo "== 配列・ポインタの長い値（4096 要素を超える）"
"$SIM" run "$T/csv/long_array.csv" -o "$WORK/olong" --quiet > "$WORK/long.log" 2>&1
RC=$?
check "4097 要素目の不一致を NG にする（終了コード 1）" test "$RC" -eq 1
check "4096 要素を超える期待値と入力を全部比べる（OK 2, NG 1）" contains "$WORK/long.log" "合計: 総数 3, OK 2, NG 1, NO Check 0"

echo "== 期待値の範囲（入力と同じ規則で解釈し、収まらない値は NG）"
run_cmp expect_range.csv 1
"$SIM" run "$T/csv/expect_range.csv" -o "$WORK/orange" --quiet > "$WORK/range.log" 2>&1
check "範囲外の期待値はエラーで止めずに NG（総数 31, OK 15, NG 16）" contains "$WORK/range.log" "合計: 総数 31, OK 15, NG 16, NO Check 0"
check "mul16@@ の期待値 65536 は実測 0 と不一致（0?(65536)）" contains "$WORK/orange/expect_range.csv" ",0,0,0?(65536),NG,"
check "uint8 の期待値 -1.0 は入力と同じく 255 と解釈する" sh -c "sed -n 16p '$WORK/orange/expect_range.csv' | grep -q '^,255,255,OK,'"

echo "== 空文字列 '' と空セル"
run_cmp empty_string.csv 1
"$SIM" run "$T/csv/empty_string.csv" -o "$WORK/oempty" --quiet > "$WORK/empty.log" 2>&1
check "'' は終端の 0 を書き、期待値の '' は空文字列と比べる（総数 7, OK 6, NG 1）" contains "$WORK/empty.log" "合計: 総数 7, OK 6, NG 1, NO Check 0"
check "入力 '' の行で g_eng_name[0] が 0（前の 'AB' が残らない）" contains "$WORK/oempty/empty_string.csv" ",'',0,'',OK,"
check "期待値 '' は空でない文字列と不一致（'XY'?('')）" contains "$WORK/oempty/empty_string.csv" ",'XY',88,'XY'?(''),NG,"

echo "== 不一致の結果セル（実測?(期待)）の CSV の引用"
run_cmp expect_quote.csv 1
"$SIM" run "$WORK/out_expect_quote/expect_quote.csv" -o "$WORK/oquote_rb" --fixed-time --quiet > "$WORK/quote_rb.log" 2>&1
RC=$?
check "引用した結果 CSV を読み直すと期待側を採る（終了コード 1、OK 1, NG 3）" sh -c "[ $RC -eq 1 ] && grep -F -q '合計: 総数 4, OK 1, NG 3, NO Check 0' '$WORK/quote_rb.log'"
"$SIM" run "$WORK/out_expect_quote/expect_quote.csv" -o "$WORK/oquote_rb2" --fixed-time --expect-actual --quiet > "$WORK/quote_rb2.log" 2>&1
RC=$?
check "引用した結果 CSV を --expect-actual で読み直すと全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 4, OK 4, NG 0, NO Check 0' '$WORK/quote_rb2.log'"
check "読み直した結果でも入力セル（\"'A,B'\"）はバイトそのまま" contains "$WORK/oquote_rb/expect_quote.csv" ",\"'A,B'\",65|44|66?('ABC'),NG,"

echo "== sim gui の数値オプション（範囲外・末尾のごみはエラー）"
gui_opt() {
    _name=$1; _msg=$2; shift 2
    # --stop-here は不明なオプション。値を受け付けてしまっても、サーバを立てずにそこで終わる
    "$SIM" gui --no-browser "$@" --stop-here > "$WORK/gopt.log" 2>&1
    _rc=$?
    if [ "$_rc" -eq 2 ] && grep -F -q -- "$_msg" "$WORK/gopt.log"; then ok "$_name"
    else ng "$_name" "終了コード ${_rc}、出力: $(head -2 "$WORK/gopt.log")"; fi
}
gui_opt "--port 70000（範囲外）は分かるエラー" "--port 70000 は 1〜65535 の整数にしてください" --port 70000
gui_opt "--port 8765x を 8765 にしない" "--port 8765x は" --port 8765x
gui_opt "--period-ms 1e400（inf）は受け付けない" "--period-ms 1e400 は 0.01〜60000" --period-ms 1e400
gui_opt "--speed -1 は受け付けない" "--speed -1 は 0（最速）か 0.1〜100" --speed -1
gui_opt "--record-max 4294967296 を切り詰めない" "--record-max 4294967296 は" --record-max 4294967296

# C11 helper shares the runtime JSON parser; no Python/Node dependency.
if "${CC:-clang}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -I"$ROOT/runtime/src" \
    "$T/tools/check_symbols.c" "$ROOT/runtime/src/sim_json.c" "$ROOT/runtime/src/sim_util.c" \
    -lm -o "$WORK/check_symbols" > "$WORK/check_symbols_build.log" 2>&1; then
    ok "check_symbols.c をビルド"
else
    ng "check_symbols.c をビルド" "$(cat "$WORK/check_symbols_build.log")"
fi

echo "== GUI サーバの API"
if ! command -v curl > /dev/null 2>&1; then
    ng "curl がありません"
else
    PORT=$((20000 + $$ % 20000))
    mkdir -p "$WORK/nohtml"
    "$SIM" gui --no-browser --cfg "$T/defines/type_contract.cfg" --port "$PORT" --period-ms 2 --html-dir "$WORK/nohtml" > "$WORK/gui.log" 2>&1 &
    GUI_PID=$!
    U="http://127.0.0.1:$PORT"
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" > "$WORK/info.json" 2>/dev/null && break
        i=$((i + 1)); sleep 0.1
    done
    if ! kill -0 "$GUI_PID" 2>/dev/null; then cat "$WORK/gui.log"; fi
    post() { curl -s -o "$WORK/$2" -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data-binary "@$T/gui/$1" "$U$3"; }

    while read -r const_name; do
        curl -s -G --data-urlencode "q=$const_name" "$U/api/symbols" > "$WORK/const_symbol.json"
        check "const symbol $const_name readonly" contains "$WORK/const_symbol.json" '"writable":false'
        printf '{"name":"%s","value":"123"}' "$const_name" > "$WORK/const_write.json"
        CODE=$(curl -s -o "$WORK/const_write.out" -w '%{http_code}' -X POST --data-binary "@$WORK/const_write.json" "$U/api/write")
        check "const API $const_name rejects write" test "$CODE" = 400
    done < "$T/defines/type_contract_ro.txt"
    curl -s -G --data-urlencode 'q=CONST_STRUCT' "$U/api/symbols" > "$WORK/const_children.json"
    check "const symbols expand children" contains "$WORK/const_children.json" 'CONST_STRUCT.data[0]'
    curl -s -G --data-urlencode 'q=MIXED.mutable' "$U/api/symbols" > "$WORK/const_mutable.json"
    check "mutable sibling writable" contains "$WORK/const_mutable.json" '"writable":true'

    check "/api/info が応答" contains "$WORK/info.json" '"version":"0.1.0"'
    check "/api/info に functions と既定の関数" contains "$WORK/info.json" '"func":"TEST_cycle","functions":["eng_judge_gear"'
    check "/api/info に state" contains "$WORK/info.json" '"state":{"running":false,"step":0'

    curl -s "$U/api/symbols?q=flag.fan&limit=10" > "$WORK/sym.json"
    check "/api/symbols でメンバまで検索" contains "$WORK/sym.json" '{"name":"g_eng.flag.fan_req","type":"uint8","size":1,"kind":"uint","writable":true,"members":[]}'
    curl -s "$U/api/symbols?q=g_eng&limit=1" > "$WORK/sym2.json"
    check "/api/symbols の構造体と members" contains "$WORK/sym2.json" '[{"name":"g_eng","type":"eng_status_t","size":8,"kind":"struct","writable":false,"members":["g_eng.rpm","g_eng.water_temp","g_eng.state","g_eng.gear","g_eng.flag"]}]'
    curl -s "$U/api/symbols?q=%40&limit=5" > "$WORK/sym3.json"
    check "/api/symbols に関数内 static（func@name）" contains "$WORK/sym3.json" '"name":"eng_update@call_cnt"'

    check "symbols: 匿名 lo/hi・境界条件と全 members の再検索・型・watch/write" "$WORK/check_symbols" "$U" "$WORK" mock

    CODE=$(post watch.json watch.out /api/watch)
    check "/api/watch が 200" test "$CODE" = 200
    check "/api/watch が解決できた名前を返す" contains "$WORK/watch.out" '"names":["g_eng.state","TEST_cycle_cnt","g_ms_tick","AMSTB_adc_read@AMIN_adc[0]"]'
    check "/api/watch が解決できない名前を errors で返す" contains "$WORK/watch.out" '{"name":"nope_var","message":"'

    CODE=$(post write_rpm.json w1.out /api/write)
    check "/api/write（固定）" test "$CODE" = 200
    CODE=$(post write_temp.json w2.out /api/write)
    check "/api/write（16 進の文字列）" test "$CODE" = 200
    post write_ign.json w3.out /api/write > /dev/null
    CODE=$(post write_const.json w4.out /api/write)
    check "/api/write で const は 400" test "$CODE" = 400
    check "/api/write のエラーメッセージ" contains "$WORK/w4.out" "書き込めません"

    CODE=$(post step5.json step.out /api/control)
    check "/api/control step が 200" test "$CODE" = 200
    check "/api/control step で 5 ステップ進む" contains "$WORK/step.out" '"step":5'
    check "state に holds" contains "$WORK/step.out" '"holds":[{"name":"AMSTB_adc_read@AMIN_adc[0]","value":"800"},{"name":"P2_bit.no0","value":"1"}]'

    curl -s "$U/api/export.csv" > "$WORK/export.csv"
    check "/api/export.csv の mod 行" grep -q "^mod,TEST_cycle,GUI記録,3,4" "$WORK/export.csv"
    check "/api/export.csv の #COMMENT 行" grep -q "^#COMMENT,AMSTB_adc_read@AMIN_adc\[0\],AMSTB_adc_read@AMIN_adc\[1\],P2_bit.no0,g_eng.state,TEST_cycle_cnt,g_ms_tick,AMSTB_adc_read@AMIN_adc\[0\]" "$WORK/export.csv"
    # 1 ステップ目だけ 1 回書き込みの AMIN_adc[1] が入る（書いたときの表記 0x78 のまま）。状態は STOP->CRANK->IDLE
    tr -d '\r' < "$WORK/export.csv" | sed -n '3,7p' > "$WORK/export_rows.txt"
    printf ',800,0x78,1,1,1,10,800\n,800,,1,2,2,20,800\n,800,,1,2,3,30,800\n,800,,1,2,4,40,800\n,800,,1,2,5,50,800\n' > "$WORK/export_expect.txt"
    check "/api/export.csv の記録（5 ステップ）" cmp -s "$WORK/export_rows.txt" "$WORK/export_expect.txt"
    check "/api/export.csv を sim run で読み直せる" sh -c "\"$SIM\" run \"$WORK/export.csv\" -o \"$WORK/oexp\" --quiet > /dev/null 2>&1"

    curl -s -N --max-time 2 "$U/api/stream" > "$WORK/sse.txt" 2>/dev/null &
    SSE_PID=$!
    sleep 0.3
    CODE=$(post run.json run.out /api/control)
    check "/api/control run" test "$CODE" = 200
    sleep 1.2
    post pause.json pause.out /api/control > /dev/null
    wait "$SSE_PID" 2>/dev/null
    check "/api/stream の hello" contains "$WORK/sse.txt" 'event: hello
data: {"periodMs":2,"func":"TEST_cycle","names":["g_eng.state","TEST_cycle_cnt","g_ms_tick","AMSTB_adc_read@AMIN_adc[0]"]}'
    check "/api/stream の samples" grep -q '^data: {"step0":[0-9]*,"names":\["g_eng.state",.*"rows":\[\[2,' "$WORK/sse.txt"
    check "/api/stream の state（running）" contains "$WORK/sse.txt" '"running":true'
    check "/api/stream の state（pause 後）" contains "$WORK/sse.txt" '"running":false'
    curl -s "$U/api/info" > "$WORK/info2.json"
    check "run で 50 ステップ以上進む" grep -q -E '"step":([5-9][0-9]|[0-9]{3,})' "$WORK/info2.json"

    CODE=$(post release_ign.json rel.out /api/write)
    check "/api/write release" test "$CODE" = 200
    printf '{"name":"AMSTB_adc_read@AMIN_adc[0]","release":true}' > "$WORK/rel2.json"
    curl -s -X POST --data-binary "@$WORK/rel2.json" "$U/api/write" > /dev/null
    CODE=$(post reset.json reset.out /api/control)
    check "/api/control reset で step 0" contains "$WORK/reset.out" '"step":0'
    check "reset で固定が空" contains "$WORK/reset.out" '"holds":[]'
    printf '{"cmd":"step"}' > "$WORK/step1.json"
    curl -s -X POST --data-binary "@$WORK/step1.json" "$U/api/control" > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' | sed -n '3p' > "$WORK/after_reset.txt"
    check "reset で変数が初期値に戻る（TEST_cycle_cnt 1, g_ms_tick 10）" grep -q ',1,10,' "$WORK/after_reset.txt"

    printf '{"name":"AMSTB_adc_read@AMIN_adc[0]","value":800}' > "$WORK/w800.json"
    curl -s -X POST --data-binary "@$WORK/w800.json" "$U/api/write" > /dev/null
    curl -s -X POST --data-binary "@$T/gui/reset.json" "$U/api/control" > /dev/null
    curl -s -X POST --data-binary "@$WORK/step1.json" "$U/api/control" > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' | sed -n '3p' > "$WORK/after_reset2.txt"
    check "reset は未消化の単発書き込みを捨てる" sh -c "! grep -q '800' '$WORK/after_reset2.txt'"

    CODE=$(curl -s -o "$WORK/load.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play.csv" "$U/api/load-csv")
    check "/api/load-csv が 200" test "$CODE" = 200
    check "/api/load-csv の応答" contains "$WORK/load.out" '{"rows":4,"names":["AMSTB_adc_read@AMIN_adc[0]","P2_bit.no0","g_eng.state","TEST_cycle_cnt"]}'
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    curl -s "$U/api/info" > "$WORK/info3.json"
    check "CSV の再生が 4 ステップで止まる" contains "$WORK/info3.json" '"step":4'
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/play_export.csv"
    STATES=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="g_eng.state") c=i} NR>2{printf "%s ", $c}' "$WORK/play_export.csv")
    check "再生した入力で状態が STOP->CRANK->IDLE->RUN（${STATES}）" test "$STATES" = "0 1 2 3 "

    CODE=$(post bad_cmd.json bad.out /api/control)
    check "不明な cmd は 400" test "$CODE" = 400
    CODE=$(curl -s -o "$WORK/nf.out" -w '%{http_code}' "$U/api/nope")
    check "不明な API は 404" test "$CODE" = 404
    CODE=$(curl -s -o "$WORK/badjson.out" -w '%{http_code}' -X POST --data-binary '{"names":' "$U/api/watch")
    check "壊れた JSON は 400" test "$CODE" = 400

    # 関数の変更が失敗したら、以前の関数・引数・設定をそのまま保つ
    post watch_call_cnt.json wcc.out /api/watch > /dev/null
    post reset.json rs.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    CODE=$(post func_bad.json fb.out /api/control)
    check "未対応の引数の関数への変更は 400" test "$CODE" = 400
    post step1.json s2.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/fb_export.csv"
    CALLS=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="eng_update@call_cnt") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/fb_export.csv")
    check "変更が失敗しても以前の関数が呼ばれ続ける（eng_update@call_cnt: ${CALLS}）" test "$CALLS" = "1 2 "
    curl -s "$U/api/info" > "$WORK/fb_info.json"
    check "変更が失敗しても /api/info の関数は TEST_cycle" contains "$WORK/fb_info.json" '"func":"TEST_cycle"'
    SP0=$(grep -o '"speed":[^,]*,"holds"' "$WORK/fb_info.json"; grep -o '"periodMs":[^,]*' "$WORK/fb_info.json")
    CODE=$(post func_bad_speed.json fbs.out /api/control)
    curl -s "$U/api/info" > "$WORK/fbs_info.json"
    SP1=$(grep -o '"speed":[^,]*,"holds"' "$WORK/fbs_info.json"; grep -o '"periodMs":[^,]*' "$WORK/fbs_info.json")
    check "関数の変更が 400 なら speed / periodMs も変えない" test "$CODE" = 400 -a "$SP0" = "$SP1"
    # 256 文字を超える値を黙って切り詰めない（"1<空白 254 個>|2" が "1" に切り詰められて 200 になっていた）
    CODE=$(post long_value.json longval.out /api/write)
    check "/api/write の長い値は切り詰めずにエラー（400）" test "$CODE" = 400
    CODE=$(post func_bad_cmd.json fbc.out /api/control)
    curl -s "$U/api/info" > "$WORK/fbc_info.json"
    check "不明な cmd で 400 なら関数も変えない" sh -c "[ '$CODE' = 400 ] && grep -q '\"func\":\"TEST_cycle\"' '$WORK/fbc_info.json'"
    CODE=$(curl -s -o "$WORK/bp.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/bad_play.csv" "$U/api/load-csv")
    curl -s "$U/api/info" > "$WORK/bp_info.json"
    check "/api/load-csv が 400 なら関数も変えない" sh -c "[ '$CODE' = 400 ] && grep -q '\"func\":\"TEST_cycle\"' '$WORK/bp_info.json'"
    curl -s "$U/" > "$WORK/index.html"
    check "sim_gui.html がなければ「GUI が未ビルド」のページ" contains "$WORK/index.html" "GUI が未ビルドです"
    if [ -f "$ROOT/gui/dist/sim_gui.html" ]; then
        cp "$ROOT/gui/dist/sim_gui.html" "$WORK/nohtml/sim_gui.html"
        curl -s "$U/" > "$WORK/index2.html"
        check "sim_gui.html があればそれを返す" cmp -s "$WORK/index2.html" "$ROOT/gui/dist/sim_gui.html"
    fi
    # 配列・文字列の入力（'XYZ'、65|66|67）を記録から落とさない。保存した CSV を再実行すると同じ結果になる
    post watch_name0.json wn.out /api/watch > /dev/null
    post reset.json rs.out /api/control > /dev/null
    post write_name_str.json wns.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post write_name_arr.json wna.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/str_export.csv"
    check "文字列・配列の入力を保存した CSV に残す（&g_eng_name 列）" sh -c "sed -n 2p '$WORK/str_export.csv' | grep -q '^#COMMENT,&g_eng_name,g_eng_name\[0\]\$'"
    printf "mod,TEST_cycle,GUI記録,1,1\n#COMMENT,&g_eng_name,g_eng_name[0]\n,'XYZ',88\n,65|66|67,65\n" > "$WORK/str_expect.csv"
    check "文字列・配列の入力は元の文字列のまま保存する" cmp -s "$WORK/str_export.csv" "$WORK/str_expect.csv"
    "$SIM" run "$WORK/str_export.csv" -o "$WORK/ostr" --quiet > "$WORK/str_rerun.log" 2>&1
    RC=$?
    check "文字列・配列の入力の記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 2, OK 2, NG 0, NO Check 0' '$WORK/str_rerun.log'"

    # 対象関数を変えたら記録を区切る（ターゲットもリセット）。保存した CSV を再実行すると全行 OK
    post watch_call_cnt.json wcc.out /api/watch > /dev/null
    post reset.json rs.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    CODE=$(post step_func_name_len.json sfn.out /api/control)
    check "関数を変えて step（200、step 1 から数え直す）" sh -c "[ '$CODE' = 200 ] && grep -q '\"step\":1,' '$WORK/sfn.out' && grep -q '\"func\":\"eng_name_len\"' '$WORK/sfn.out'"
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/fn_export.csv"
    printf 'mod,eng_name_len,GUI記録,0,1\n#COMMENT,eng_update@call_cnt\n,0\n' > "$WORK/fn_expect.csv"
    check "関数を変える前の記録を新しい関数の下に出さない" cmp -s "$WORK/fn_export.csv" "$WORK/fn_expect.csv"
    "$SIM" run "$WORK/fn_export.csv" -o "$WORK/ofn" --quiet > "$WORK/fn_rerun.log" 2>&1
    RC=$?
    check "関数を変えた後の記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/fn_rerun.log'"
    post step_func_cycle.json sfc.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/fn_export2.csv"
    CALLS=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="eng_update@call_cnt") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/fn_export2.csv")
    check "TEST_cycle に戻すとリセットしてから数える（eng_update@call_cnt: ${CALLS}）" test "$CALLS" = "1 2 "

    # 往復で失われていたほかの情報: 符号付きへの 16 進（0xFFFF）、止まっているときの $（二重に確保しない）、',' を含む文字列
    post watch_rt.json wrt.out /api/watch > /dev/null
    post reset_func_name_len.json rfn.out /api/control > /dev/null
    CODE=$(post write_alloc_bad.json wab.out /api/write)
    check "/api/write で \$ に負の要素数は 400" test "$CODE" = 400
    post write_temp_hex.json wth.out /api/write > /dev/null
    post write_alloc.json wal.out /api/write > /dev/null
    post write_name_comma.json wnc.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/rt_export.csv"
    check "16 進の入力は書いた表記（0xFFFF）のまま保存する" grep -q ',0xFFFF,' "$WORK/rt_export.csv"
    check "',' を含む文字列の入力は \"...\" で囲んで保存する" grep -q ",\"'A,B'\"," "$WORK/rt_export.csv"
    # gp_log はヒープの先頭 0xE000（57344）。二重に確保すると 2 個目の 0xE004（57348）になる
    check "止まっているときの \$ は次のステップで 1 回だけ確保する（gp_log = 57344）" sh -c "sed -n 3p '$WORK/rt_export.csv' | grep -q ',57344,' && ! grep -q '57348' '$WORK/rt_export.csv'"
    "$SIM" run "$WORK/rt_export.csv" -o "$WORK/ort" --quiet > "$WORK/rt_rerun.log" 2>&1
    RC=$?
    check "16 進・\$・',' を含む文字列の記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/rt_rerun.log'"

    # 関数を変えたら、新しい関数で解決できない固定・監視（@rpm）を外す。古い引数番号（@a）へ書き続けない
    post reset_func_gear.json rfg.out /api/control > /dev/null
    CODE=$(post watch_rpm.json wr.out /api/watch)
    check "eng_judge_gear の @rpm を監視できる" contains "$WORK/wr.out" '"names":["@rpm","g_ms_tick"],"errors":[]'
    post write_rpm_hold.json wrh.out /api/write > /dev/null
    curl -s -N --max-time 2 "$U/api/stream" > "$WORK/drop_sse.txt" 2>/dev/null &
    SSE_PID=$!
    sleep 0.3
    CODE=$(post reset_func_mul16.json rfm.out /api/control)
    check "固定した引数のない関数（mul16）への変更は 200" test "$CODE" = 200
    check "/api/control の dropped に外した固定（@rpm, hold）" contains "$WORK/rfm.out" '{"name":"@rpm","kind":"hold","message":"'
    check "/api/control の dropped に外した監視（@rpm, watch）" contains "$WORK/rfm.out" '{"name":"@rpm","kind":"watch","message":"'
    check "/api/control の holds に @rpm が残らない" contains "$WORK/rfm.out" '"holds":[]'
    sleep 0.3
    post watch_mul16.json wm.out /api/watch > /dev/null
    post write_a3.json wa.out /api/write > /dev/null
    post write_b2.json wb.out /api/write > /dev/null
    post step1.json dstep.out /api/control > /dev/null
    wait "$SSE_PID" 2>/dev/null
    check "SSE の hello から外した監視（@rpm）が消える" contains "$WORK/drop_sse.txt" '"func":"mul16","names":["g_ms_tick"]}'
    check "SSE の state に dropped" contains "$WORK/drop_sse.txt" '"dropped":[{"name":"@rpm","kind":"watch"'
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/drop_export.csv"
    RETS=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="mul16@@") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/drop_export.csv")
    check "外した固定が @a を上書きしない（mul16(3, 2) = ${RETS}）" test "$RETS" = "6 "
    check "保存した CSV に @rpm の列がない" sh -c "! grep -q '@rpm' '$WORK/drop_export.csv'"
    "$SIM" run "$WORK/drop_export.csv" -o "$WORK/odrop" --quiet > "$WORK/drop_rerun.log" 2>&1
    RC=$?
    check "固定を外した後の記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/drop_rerun.log'"
    # CSV の読み込みで関数が変わるときも同じ
    post reset_func_gear.json rfg.out /api/control > /dev/null
    post write_rpm_hold.json wrh.out /api/write > /dev/null
    CODE=$(curl -s -o "$WORK/lm.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_mul16.csv" "$U/api/load-csv")
    check "固定した引数のない関数の CSV の読み込みは 200" test "$CODE" = 200
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    curl -s "$U/api/info" > "$WORK/lm_info.json"
    check "CSV の読み込みで外した固定を state の dropped に出す" contains "$WORK/lm_info.json" '"dropped":[{"name":"@rpm","kind":"hold"'
    check "CSV の読み込み後の holds に @rpm が残らない" contains "$WORK/lm_info.json" '"holds":[]'
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/lm_export.csv"
    check "CSV の再生の記録に @rpm の列がない" sh -c "! grep -q '@rpm' '$WORK/lm_export.csv'"
    "$SIM" run "$WORK/lm_export.csv" -o "$WORK/olm" --quiet > "$WORK/lm_rerun.log" 2>&1
    RC=$?
    check "CSV の再生の記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 2, OK 2, NG 0, NO Check 0' '$WORK/lm_rerun.log'"

    # 同じメモリへの固定（P1）と単発書き込み（P1_bit.no0）: 1 ステップの入力は保存 CSV の入力列の順に適用する（sim run と同じ）。
    # 書き込みは次のステップの開始時にだけ反映するので、止まっているときに固定してすぐ外した値（g_mode = 5）や、
    # 単発で書いてから固定を解除した値（g_mode = 7）は書かれず、記録にも残らない。'' は終端の 0 を書く
    post reset_func_mul16.json r16.out /api/control > /dev/null
    post watch_p1.json wp1.out /api/watch > /dev/null
    post write_p1_hold.json wp1h.out /api/write > /dev/null
    post write_p1bit0.json wp1b.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post release_p1.json rp1.out /api/write > /dev/null
    post write_mode_hold.json wmh.out /api/write > /dev/null
    post release_mode.json rmh.out /api/write > /dev/null
    post write_mode7.json wm7.out /api/write > /dev/null
    curl -s "$U/api/info" > "$WORK/m7_info.json"
    check "反映待ちの単発書き込みを state の pending に出す（g_mode = 7）" contains "$WORK/m7_info.json" '"pending":[{"name":"g_mode","value":"7"}]'
    post release_mode.json rmh.out /api/write > /dev/null
    curl -s "$U/api/info" > "$WORK/m7r_info.json"
    check "固定の解除（release）でその名前の反映待ちも消える" contains "$WORK/m7r_info.json" '"pending":[]'
    post step1.json s1.out /api/control > /dev/null
    post write_name_str.json wns.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    CODE=$(post write_name_empty.json wne.out /api/write)
    check "/api/write で '' を書ける（200）" test "$CODE" = 200
    post step1.json s1.out /api/control > /dev/null
    CODE=$(post write_alloc_empty.json wae.out /api/write)
    check "/api/write で \$ に '' は 400" test "$CODE" = 400
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/ov_export.csv"
    # 出力列の値（同じ名前の入力列もあるので、最後に出てくる列を採る）
    outcol() { awk -F, -v n="$1" 'NR==2{for(i=1;i<=NF;i++) if($i==n) c=i} NR>2&&c{printf "%s ", $c}' "$WORK/ov_export.csv"; }
    P1S=$(outcol P1); MODES=$(outcol g_mode); NAMES=$(outcol 'g_eng_name[0]')
    check "P1 を固定して P1_bit.no0 = 0 を単発で書くと列の順に適用して P1 = 0（P1: ${P1S}）" test "$P1S" = "0 0 0 0 "
    MODECOLS=$(sed -n 2p "$WORK/ov_export.csv" | tr ',' '\n' | grep -c '^g_mode$')
    check "止まっているときに固定してすぐ外した値・解除した名前の反映待ちは書かない（g_mode の列 ${MODECOLS} 個（出力だけ）、出力: ${MODES}）" test "$MODECOLS" = 1 -a "$MODES" = "0 0 0 0 "
    check "'' を書くと g_eng_name[0] が 0（${NAMES}）" test "$NAMES" = "69 69 88 0 "
    "$SIM" run "$WORK/ov_export.csv" -o "$WORK/oov" --quiet > "$WORK/ov_rerun.log" 2>&1
    RC=$?
    check "重なる入力先の固定・単発書き込みの記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 4, OK 4, NG 0, NO Check 0' '$WORK/ov_rerun.log'"

    # 停止中の書き込みはすぐには反映しない。次のステップの開始時に、列ごとに最後の 1 個だけを反映して記録する
    # （'XYZ' に続けて 'A' を書くと、'A' だけが書かれて g_eng_name[2] は初期値 "ENG-A" の 'G' = 71 のまま）
    post reset_func_name_len.json rfn2.out /api/control > /dev/null
    post watch_name2.json wn2.out /api/watch > /dev/null
    post write_name_str.json wns2.out /api/write > /dev/null
    post write_name_a.json wna2.out /api/write > /dev/null
    curl -s "$U/api/info" > "$WORK/pa_info.json"
    check "同じ名前に書き直すと反映待ちが置き換わる（pending は 'A' の 1 個）" contains "$WORK/pa_info.json" "\"pending\":[{\"name\":\"&g_eng_name\",\"value\":\"'A'\"}]"
    curl -s -N --max-time 1 "$U/api/stream" > "$WORK/pa_sse.txt" 2>/dev/null
    check "停止中の書き込みでは values（メモリの値）は変わらない（g_eng_name[2] = 71）" contains "$WORK/pa_sse.txt" '"names":["g_eng_name[2]"],"values":[71]}'
    check "SSE の state に pending" contains "$WORK/pa_sse.txt" "\"pending\":[{\"name\":\"&g_eng_name\",\"value\":\"'A'\"}]"
    post step1.json pas.out /api/control > /dev/null
    check "ステップで反映すると pending が空になる" contains "$WORK/pas.out" '"pending":[]'
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/pa_export.csv"
    check "最後の 'A' だけを入力として記録し、出力は 71" sh -c "sed -n 3p '$WORK/pa_export.csv' | grep -q \"^,'A',71\\\$\""
    "$SIM" run "$WORK/pa_export.csv" -o "$WORK/opa" --quiet > "$WORK/pa_rerun.log" 2>&1
    RC=$?
    check "停止中に書き直した記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/pa_rerun.log'"

    # CSV の再生は、読み込んだ CSV の入力列の順に適用する（以前に GUI で P1、P1_bit.no0 の順に書いていても）
    post reset_func_mul16.json r16b.out /api/control > /dev/null
    post watch_p1only.json wpo.out /api/watch > /dev/null
    post write_p1_one.json wp1o.out /api/write > /dev/null
    post write_p1bit0.json wp1b2.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    CODE=$(curl -s -o "$WORK/po.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_order.csv" "$U/api/load-csv")
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/po_export.csv"
    POP1=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="P1") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/po_export.csv")
    check "CSV の再生は CSV の入力列の順（P1_bit.no0, P1）に書く（P1: ${POP1}）" test "$CODE" = 200 -a "$POP1" = "1 "
    check "再生の記録の入力列は CSV の順" sh -c "sed -n 2p '$WORK/po_export.csv' | grep -q '^#COMMENT,P1_bit.no0,P1,'"
    "$SIM" run "$T/gui/play_order.csv" -o "$WORK/opo" --quiet > "$WORK/po_run.log" 2>&1
    RC=$?
    check "同じ CSV を sim run で実行しても P1 = 1（終了コード 0）" test "$RC" -eq 0

    # 同じ名前の入力列が複数ある CSV: sim run は左から順に書く（P1=0, P1_bit.no0=1, P1=2 で P1 = 2）。
    # GUI の記録は入力 1 名 = 1 列なので /api/load-csv は 400 で拒否し、何も変えない
    "$SIM" run "$T/gui/play_dup.csv" -o "$WORK/odup" --quiet > "$WORK/dup_run.log" 2>&1
    RC=$?
    check "sim run は同じ名前の入力列を左から順に書く（P1 = 2 で OK、終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/dup_run.log'"
    post watch_p1only.json wdup.out /api/watch > /dev/null
    post rt_p1_hold.json hdup.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/info" > "$WORK/dup_info0.json"
    curl -s "$U/api/export.csv" > "$WORK/dup_export0.csv"
    CODE=$(curl -s -o "$WORK/dup.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_dup.csv" "$U/api/load-csv")
    check "/api/load-csv は同じ名前の入力列が複数ある CSV を 400 で拒否する（名前・列番号・sim run の案内）" sh -c "[ '$CODE' = 400 ] && grep -F -q \"'P1'\" '$WORK/dup.out' && grep -F -q '2 列目' '$WORK/dup.out' && grep -F -q '4 列目' '$WORK/dup.out' && grep -F -q 'sim run では実行できます' '$WORK/dup.out'"
    sleep 0.3
    curl -s "$U/api/info" > "$WORK/dup_info1.json"
    curl -s "$U/api/export.csv" > "$WORK/dup_export1.csv"
    check "同じ名前の入力列で 400 なら関数・再生・記録・固定・監視を変えない" sh -c "cmp -s '$WORK/dup_info0.json' '$WORK/dup_info1.json' && cmp -s '$WORK/dup_export0.csv' '$WORK/dup_export1.csv' && grep -q '\"playback\":false' '$WORK/dup_info1.json'"
    post release_p1.json rdup.out /api/write > /dev/null

    # 表記が違うだけで同じメモリを指す入力列（P1 と P1_bit.no0、a[0] と a[ 0 ]、g.x と g .x）は別の列として列順に書く。
    # 出力列の同名・入力と出力の同名は受け付ける
    CODE=$(curl -s -o "$WORK/al.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_alias.csv" "$U/api/load-csv")
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/al_export.csv"
    ALV=$(awk -F, 'NR==2{for(i=1;i<=NF;i++){if($i=="P1")a=i; if($i=="g_gear_up_rpm[0]")b=i; if($i=="g_eng.rpm")c=i}} NR>2{printf "%s %s %s", $a, $b, $c}' "$WORK/al_export.csv")
    check "表記の違う同じ先の入力列は列順に書く（P1 g_gear_up_rpm[0] g_eng.rpm: ${ALV}）" test "$CODE" = 200 -a "$ALV" = "1 7 200"
    "$SIM" run "$WORK/al_export.csv" -o "$WORK/oal" --quiet > "$WORK/al_rerun.log" 2>&1
    RC=$?
    check "その記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 1, OK 1, NG 0, NO Check 0' '$WORK/al_rerun.log'"

    # mod ブロックが複数ある CSV は先頭のブロックだけを再生し、そのことを応答で知らせる
    CODE=$(curl -s -o "$WORK/mb.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_multi.csv" "$U/api/load-csv")
    check "/api/load-csv で mod ブロックが 2 つなら先頭だけ再生し、応答の blocks と warning で知らせる" sh -c "[ '$CODE' = 200 ] && grep -F -q '{\"rows\":1,\"names\":[\"@a\",\"@b\",\"mul16@@\"],\"blocks\":2,\"warning\":\"' '$WORK/mb.out' && grep -F -q '2 つ目以降' '$WORK/mb.out'"
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done

    # 往復の性質: GUI で操作 -> /api/export.csv -> sim run（--expect-actual なし）で全行 OK。
    # 重なる入力先・文字列/配列・$ptr・固定と単発の併用・固定の解除・途中のリセット・CSV の再生を混ぜる
    post reset_func_mul16.json rt0.out /api/control > /dev/null
    post rt_watch.json rtw.out /api/watch > /dev/null
    post rt_p1_hold.json rt1.out /api/write > /dev/null
    post write_p1bit0.json rt2.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post write_name_str.json rt3.out /api/write > /dev/null
    post rt_name_q.json rt4.out /api/write > /dev/null
    post rt_a_hold.json rt5.out /api/write > /dev/null
    post write_b2.json rt6.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post reset.json rt7.out /api/control > /dev/null
    post write_p1bit0.json rt8.out /api/write > /dev/null
    post write_alloc.json rt9.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post release_p1.json rt10.out /api/write > /dev/null
    post rt_b5.json rt11.out /api/write > /dev/null
    post write_name_arr.json rt12.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/rt1_export.csv"
    RTP1=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="P1") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/rt1_export.csv")
    RTMUL=$(awk -F, 'NR==2{for(i=1;i<=NF;i++) if($i=="mul16@@") c=i} NR>2&&c{printf "%s ", $c}' "$WORK/rt1_export.csv")
    check "往復: リセット後も固定（P1 = 0xFF、@a = 3）を書き、列の順に P1_bit.no0 = 0 を重ねる（P1: ${RTP1}、mul16@@: ${RTMUL}）" test "$RTP1" = "254 254 " -a "$RTMUL" = "0 15 "
    "$SIM" run "$WORK/rt1_export.csv" -o "$WORK/ort1" --quiet > "$WORK/rt1_rerun.log" 2>&1
    RC=$?
    check "往復: 途中でリセットした記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 2, OK 2, NG 0, NO Check 0' '$WORK/rt1_rerun.log'"
    curl -s -o /dev/null -X POST --data-binary "@$T/gui/rt_play.csv" "$U/api/load-csv"
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    post rt_p1_16.json rt13.out /api/write > /dev/null
    post write_name_a.json rt14.out /api/write > /dev/null
    post write_alloc.json rt15.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post rt_release_a.json rt16.out /api/write > /dev/null
    post write_b2.json rt17.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/rt2_export.csv"
    "$SIM" run "$WORK/rt2_export.csv" -o "$WORK/ort2" --quiet > "$WORK/rt2_rerun.log" 2>&1
    RC=$?
    check "往復: CSV の再生に続けて操作した記録を sim run で再実行すると全行 OK（終了コード 0）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 5, OK 5, NG 0, NO Check 0' '$WORK/rt2_rerun.log'"

    # 入出力のセルがすべて空になる周期も 1 回の呼び出しとして保存する（1 列目に step N の目印を入れる）。
    # 空セルだけの行で保存すると sim run が空行として読み飛ばし、呼び出し回数が減って eng_update@call_cnt が NG になっていた
    post watch_none.json en0.out /api/watch > /dev/null
    post reset_func_cycle.json en1.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post watch_call_cnt.json en2.out /api/watch > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/en_export.csv"
    printf 'mod,TEST_cycle,GUI記録,0,1\n#COMMENT,eng_update@call_cnt\nstep 1,\n,2\n' > "$WORK/en_expect.csv"
    check "監視の前の空の周期は 1 列目に step 1 を入れて保存する" cmp -s "$WORK/en_export.csv" "$WORK/en_expect.csv"
    "$SIM" run "$WORK/en_export.csv" -o "$WORK/oen" --quiet > "$WORK/en_rerun.log" 2>&1
    RC=$?
    check "空の周期を含む記録を sim run で再実行すると呼び出し回数が一致（終了コード 0、総数 2, OK 1, NO Check 1）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 2, OK 1, NG 0, NO Check 1' '$WORK/en_rerun.log'"
    # 監視も入力もないまま保存した入力数 0・出力数 0 のブロック。波形の読み込みで列 1 つの行を落とさないよう区切りのカンマを付ける
    post watch_none.json en3.out /api/watch > /dev/null
    post reset_func_cycle.json en4.out /api/control > /dev/null
    post step3.json s3.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/e0_export.csv"
    printf 'mod,TEST_cycle,GUI記録,0,0\n#COMMENT\nstep 1,\nstep 2,\nstep 3,\n' > "$WORK/e0_expect.csv"
    check "入力数 0・出力数 0 の記録はすべての行に step N の目印" cmp -s "$WORK/e0_export.csv" "$WORK/e0_expect.csv"
    "$SIM" run "$WORK/e0_export.csv" -o "$WORK/oe0" --quiet > "$WORK/e0_rerun.log" 2>&1
    RC=$?
    check "入力数 0・出力数 0 の記録を sim run で再実行すると 3 回呼ぶ（終了コード 0、総数 3, NO Check 3）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 3, OK 0, NG 0, NO Check 3' '$WORK/e0_rerun.log'"
    # 記録の途中で入力と監視が増減する（前半の行は後から増えた列が空）。最後の行の eng_update@call_cnt で呼び出し回数を確かめる
    post watch_none.json en5.out /api/watch > /dev/null
    post reset_func_cycle.json en6.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post write_ign.json en7.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post release_ign.json en8.out /api/write > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post watch_call_cnt.json en9.out /api/watch > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post watch_none.json en10.out /api/watch > /dev/null
    post step1.json s1.out /api/control > /dev/null
    post watch_call_cnt.json en11.out /api/watch > /dev/null
    post step1.json s1.out /api/control > /dev/null
    curl -s "$U/api/export.csv" | tr -d '\r' > "$WORK/em_export.csv"
    printf 'mod,TEST_cycle,GUI記録,1,1\n#COMMENT,P2_bit.no0,eng_update@call_cnt\nstep 1,,\nstep 2,,\n,1,\nstep 4,,\n,,5\nstep 6,,\n,,7\n' > "$WORK/em_expect.csv"
    check "入力・監視が途中で増減した記録は空の周期だけに step N の目印" cmp -s "$WORK/em_export.csv" "$WORK/em_expect.csv"
    "$SIM" run "$WORK/em_export.csv" -o "$WORK/oem" --quiet > "$WORK/em_rerun.log" 2>&1
    RC=$?
    check "入力・監視が途中で増減した記録を sim run で再実行すると呼び出し回数が一致（終了コード 0、総数 7, OK 2, NO Check 5）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 7, OK 2, NG 0, NO Check 5' '$WORK/em_rerun.log'"
    # sim run は 1 列目（コメント欄）も含めて全セルが空の行だけを空行として読み飛ばす。1 列目だけに書いた行は実行する
    "$SIM" run "$T/csv/comment_col_only.csv" -o "$WORK/occ" --quiet > "$WORK/cc_rerun.log" 2>&1
    RC=$?
    check "1 列目だけに書いた行は実行し、全セルが空の行は読み飛ばす（総数 2, OK 1, NO Check 1）" sh -c "[ $RC -eq 0 ] && grep -F -q '合計: 総数 2, OK 1, NG 0, NO Check 1' '$WORK/cc_rerun.log' && sed -n 3p '$WORK/occ/comment_col_only.csv' | grep -q '^,,\$' && sed -n 4p '$WORK/occ/comment_col_only.csv' | grep -q '^note,1,NO Check,'"

    # 数値の範囲: 丸め・切り詰めをせず 400。400 なら何も変えない
    curl -s "$U/api/info" > "$WORK/nv0.json"
    CODE=$(post control_bad_speed.json nv1.out /api/control)
    check "/api/control の speed 1000 は 100 に丸めずに 400" sh -c "[ '$CODE' = 400 ] && grep -q 'speed' '$WORK/nv1.out'"
    CODE=$(post control_bad_steps.json nv2.out /api/control)
    check "/api/control の steps 0 は 1 にせずに 400" test "$CODE" = 400
    CODE=$(post control_bad_period.json nv3.out /api/control)
    check "/api/control の periodMs 1e400（inf）は 400" test "$CODE" = 400
    curl -s "$U/api/info" > "$WORK/nv4.json"
    NV0=$(grep -o '"step":[0-9]*,"speed":[^,]*\|"periodMs":[^,]*' "$WORK/nv0.json")
    NV4=$(grep -o '"step":[0-9]*,"speed":[^,]*\|"periodMs":[^,]*' "$WORK/nv4.json")
    check "400 の後も step・速度・周期は変わらない" test -n "$NV0" -a "$NV0" = "$NV4"
    CODE=$(post write_u64_exact.json nv5.out /api/write)
    curl -s "$U/api/info" > "$WORK/nv6.json"
    check "/api/write の整数 9007199254740993 を double に丸めない" sh -c "[ '$CODE' = 200 ] && grep -F -q '{\"name\":\"0x100:8\",\"value\":\"9007199254740993\"}' '$WORK/nv6.json'"
    post release_u64.json nv7.out /api/write > /dev/null
    # 固定の値も書いたセルの文字列のまま holds に出す（2^53 を超える整数を丸めない、16 進を 10 進にしない）
    curl -s -N --max-time 1.5 "$U/api/stream" > "$WORK/hh_sse.txt" 2>/dev/null &
    SSE_PID=$!
    sleep 0.3
    post write_u64_hold.json hh1.out /api/write > /dev/null
    post write_hex_hold.json hh2.out /api/write > /dev/null
    HH='"holds":[{"name":"0x100:8","value":"9007199254740993"},{"name":"AMSTB_adc_read@AMIN_adc[1]","value":"0x78"}]'
    curl -s "$U/api/info" > "$WORK/hh_info.json"
    check "/api/info の holds に固定した値の表記のまま（9007199254740993、0x78）" contains "$WORK/hh_info.json" "$HH"
    post pause.json hh_ctl.out /api/control > /dev/null
    check "/api/control の応答の holds に固定した値の表記のまま" contains "$WORK/hh_ctl.out" "$HH"
    wait "$SSE_PID" 2>/dev/null
    check "SSE の state の holds に固定した値の表記のまま" contains "$WORK/hh_sse.txt" "$HH"
    post release_u64.json hh3.out /api/write > /dev/null
    post release_hex.json hh4.out /api/write > /dev/null
    CODE=$(curl -s -o "$WORK/nv8.out" -w '%{http_code}' "$U/api/symbols?q=g_&limit=10x")
    check "/api/symbols の limit=10x は 400" test "$CODE" = 400
    CODE=$(curl -s -o "$WORK/nv9.out" -w '%{http_code}' -X POST -H 'Content-Length: 12abc' --data-binary '{"names":[]}' "$U/api/watch")
    check "Content-Length が数値でなければ 400" test "$CODE" = 400

    # BOM 付きの CSV も読み込める（先頭の mod 行を見つける）。UTF-16 は分かるエラーで 400
    CODE=$(curl -s -o "$WORK/lbom.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_bom.csv" "$U/api/load-csv")
    check "/api/load-csv で BOM 付きの CSV が 200（2 行）" sh -c "[ '$CODE' = 200 ] && grep -F -q '{\"rows\":2,\"names\":[\"@a\",\"@b\",\"mul16@@\"]}' '$WORK/lbom.out'"
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" | grep -q '"running":false' && break
        i=$((i + 1)); sleep 0.1
    done
    CODE=$(curl -s -o "$WORK/lu16.out" -w '%{http_code}' -X POST --data-binary "@$T/gui/play_utf16.csv" "$U/api/load-csv")
    check "/api/load-csv で UTF-16 の CSV は分かるエラーで 400" sh -c "[ '$CODE' = 400 ] && grep -F -q 'UTF-16 の CSV には対応していません' '$WORK/lu16.out'"

    # 入力エラーの拒否は原子的。先頭ブロックが空でも再生しない。
    post release_adc.json er.out /api/write > /dev/null
    post release_ign.json er.out /api/write > /dev/null
    post reset_func_cycle.json er.out /api/control > /dev/null
    post watch_call_cnt.json er.out /api/watch > /dev/null
    post step1.json er.out /api/control > /dev/null
    for fixture in play_out_of_range play_bad_value play_empty; do
        curl -s "$U/api/info" > "$WORK/reject_info0"
        curl -s "$U/api/export.csv" > "$WORK/reject_csv0"
        CODE=$(post "$fixture.csv" reject.out /api/load-csv)
        check "$fixture は 400" test "$CODE" = 400
        if [ "$fixture" != play_empty ]; then
            check "$fixture のエラーは入力名・値・データ行・列を示す" sh -c "grep -F -q '@a' '$WORK/reject.out' && grep -F -q '列目' '$WORK/reject.out' && grep -F -q '行目' '$WORK/reject.out'"
        else
            check "空 CSV のエラーはデータ行がない旨" contains "$WORK/reject.out" '再生するデータ行がありません'
        fi
        curl -s "$U/api/info" > "$WORK/reject_info1"
        curl -s "$U/api/export.csv" > "$WORK/reject_csv1"
        check "$fixture 拒否で info 不変" cmp -s "$WORK/reject_info0" "$WORK/reject_info1"
        check "$fixture 拒否で記録不変" cmp -s "$WORK/reject_csv0" "$WORK/reject_csv1"
    done

    # 1 周期目だけ自動再生してから step 5。3 行目の NULL 書き込みで止まり、進むのは 1 周期だけ。
    post period_slow.json er.out /api/control > /dev/null
    CODE=$(post play_null.csv null_load.out /api/load-csv)
    check "メモリ依存の入力は読み込み時には拒否しない" test "$CODE" = 200
    sleep 0.2
    post pause.json er.out /api/control > /dev/null
    post step5.json null_step.out /api/control > /dev/null
    check "step N は入力失敗で中断し進んだ数を返す" contains "$WORK/null_step.out" '"stepsDone":1,"timedOut":false'
    check "CSV 入力失敗は名前・値・理由・データ行を返し停止" sh -c "grep -F -q 'CSV 3' '$WORK/null_step.out' && grep -F -q '*gp_log' '$WORK/null_step.out' && grep -F -q '7' '$WORK/null_step.out' && grep -F -q 'NULL' '$WORK/null_step.out' && grep -F -q '\"running\":false' '$WORK/null_step.out' && grep -F -q '\"playback\":false' '$WORK/null_step.out' && grep -F -q '\"step\":2' '$WORK/null_step.out'"
    curl -s "$U/api/export.csv" > "$WORK/null_export.csv"
    "$SIM" run "$WORK/null_export.csv" -o "$WORK/onull" --quiet > "$WORK/null_run.log" 2>&1
    RC=$?
    check "失敗周期は呼び出しも記録も増えず 2 行 OK で再実行" sh -c "[ $RC -eq 0 ] && grep -F -q 'OK 2, NG 0, NO Check 0' '$WORK/null_run.log'"
    for fixture in write_null hold_null; do
        post reset.json er.out /api/control > /dev/null
        curl -s "$U/api/export.csv" > "$WORK/null_export.csv"
        post "$fixture.json" er.out /api/write > /dev/null
        post step5.json null_write.out /api/control > /dev/null
        check "$fixture の入力失敗は進んだ数 0 で停止し理由を返す" sh -c "grep -F -q '\"stepsDone\":0' '$WORK/null_write.out' && grep -F -q '\"running\":false' '$WORK/null_write.out' && grep -F -q 'NULL' '$WORK/null_write.out' && grep -F -q '*gp_log' '$WORK/null_write.out'"
        curl -s "$U/api/export.csv" > "$WORK/null_after.csv"
        check "$fixture の失敗周期は記録を追加しない" cmp -s "$WORK/null_export.csv" "$WORK/null_after.csv"
        post reset.json er.out /api/control > /dev/null
        post release_null.json er.out /api/write > /dev/null
    done
    post watch_call_cnt.json er.out /api/watch > /dev/null
    curl -s -N --max-time 0.3 "$U/api/stream" > "$WORK/null_values" 2>/dev/null
    check "リセット後の失敗した単発・固定の周期で関数カウンタが増えない" contains "$WORK/null_values" '"values":[0]'
    # 部分書き込みの後は実行も予約も拒否し、正常な記録に混ぜない。
    post reset_func_name_len.json er.out /api/control > /dev/null
    post watch_name2.json er.out /api/watch > /dev/null
    post write_name_str.json er.out /api/write > /dev/null
    post write_null_one.json er.out /api/write > /dev/null
    post step1.json nr_failed.json /api/control > /dev/null
    check "部分書き込み失敗で needReset true、step 0" sh -c "grep -F -q '\"needReset\":true' '$WORK/nr_failed.json' && grep -F -q '\"step\":0,' '$WORK/nr_failed.json' && grep -F -q 'NULL' '$WORK/nr_failed.json'"
    curl -s "$U/api/info" > "$WORK/nr_before.json"
    curl -s "$U/api/export.csv" > "$WORK/nr_before.csv"
    for fixture in write_name_a hold_null release_null; do
        CODE=$(post "$fixture.json" nr_rejected.json /api/write)
        check "要リセット中の $fixture は 400" test "$CODE" = 400
        check "拒否応答に直前の失敗とリセット案内" sh -c "grep -F -q 'NULL' '$WORK/nr_rejected.json' && grep -F -q 'リセット' '$WORK/nr_rejected.json'"
    done
    for fixture in run step1 run_blocked; do
        CODE=$(post "$fixture.json" nr_rejected.json /api/control)
        check "要リセット中の $fixture は 400" test "$CODE" = 400
    done
    curl -s "$U/api/info" > "$WORK/nr_after.json"
    curl -s "$U/api/export.csv" > "$WORK/nr_after.csv"
    check "拒否で step・関数・設定・pending・error・needReset 不変" cmp -s "$WORK/nr_before.json" "$WORK/nr_after.json"
    check "拒否で記録不変" cmp -s "$WORK/nr_before.csv" "$WORK/nr_after.csv"
    post pause.json nr_pause.json /api/control > /dev/null
    check "pause は needReset と失敗理由を消さない" sh -c "grep -F -q '\"needReset\":true' '$WORK/nr_pause.json' && grep -F -q 'NULL' '$WORK/nr_pause.json'"
    post reset.json nr_reset.json /api/control > /dev/null
    check "reset で needReset false と error 空、step 0" sh -c "grep -F -q '\"needReset\":false' '$WORK/nr_reset.json' && grep -F -q '\"error\":\"\"' '$WORK/nr_reset.json' && grep -F -q '\"step\":0,' '$WORK/nr_reset.json'"
    post write_name_a.json er.out /api/write > /dev/null
    post step1.json er.out /api/control > /dev/null
    curl -s "$U/api/export.csv" > "$WORK/nr_reset.csv"
    "$SIM" run "$WORK/nr_reset.csv" -o "$WORK/onr_reset" --quiet > "$WORK/nr_reset.log" 2>&1
    RC=$?
    check "reset 後の A は初期値から 1 行 OK で再実行" sh -c "[ $RC -eq 0 ] && grep -F -q 'OK 1, NG 0, NO Check 0' '$WORK/nr_reset.log'"
    post write_name_str.json er.out /api/write > /dev/null
    post write_null_one.json er.out /api/write > /dev/null
    post step1.json nr_prefix.json /api/control > /dev/null
    check "正常行の後の失敗も needReset true、step 1" sh -c "grep -F -q '\"needReset\":true' '$WORK/nr_prefix.json' && grep -F -q '\"step\":1,' '$WORK/nr_prefix.json'"
    curl -s "$U/api/export.csv" > "$WORK/nr_prefix.csv"
    "$SIM" run "$WORK/nr_prefix.csv" -o "$WORK/onr_prefix" --quiet > "$WORK/nr_prefix.log" 2>&1
    RC=$?
    check "部分書き込み失敗前の正常記録は 1 行 OK で再実行" sh -c "[ $RC -eq 0 ] && grep -F -q 'OK 1, NG 0, NO Check 0' '$WORK/nr_prefix.log'"
    CODE=$(post play_name_a.csv nr_load.json /api/load-csv)
    check "要リセット中も正常 CSV は読み込める" test "$CODE" = 200
    sleep 0.2
    curl -s "$U/api/info" > "$WORK/nr_load_info.json"
    check "CSV 読み込み成功で needReset と error を解除し再生" sh -c "grep -F -q '\"needReset\":false' '$WORK/nr_load_info.json' && grep -F -q '\"error\":\"\"' '$WORK/nr_load_info.json' && grep -F -q '\"step\":1,' '$WORK/nr_load_info.json'"
    post write_null_one.json er.out /api/write > /dev/null
    post step1.json er.out /api/control > /dev/null
    post func_cycle.json nr_func.json /api/control > /dev/null
    check "関数変更だけでも needReset と error を解除" sh -c "grep -F -q '\"needReset\":false' '$WORK/nr_func.json' && grep -F -q '\"error\":\"\"' '$WORK/nr_func.json'"
    post period_normal.json er.out /api/control > /dev/null
    post play_null.csv er.out /api/load-csv > /dev/null
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" > "$WORK/null_auto_info"
        grep -q '"running":false' "$WORK/null_auto_info" && break
        i=$((i + 1)); sleep 0.1
    done
    check "実行中の CSV 入力失敗でも step 2 で再生停止" sh -c "grep -F -q '\"step\":2' '$WORK/null_auto_info' && grep -F -q '\"running\":false' '$WORK/null_auto_info' && grep -F -q '\"playback\":false' '$WORK/null_auto_info' && grep -F -q 'NULL' '$WORK/null_auto_info'"
    post play_three.csv er.out /api/load-csv > /dev/null
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" > "$WORK/three_info"
        grep -q '"running":false' "$WORK/three_info" && break
        i=$((i + 1)); sleep 0.1
    done
    check "N 行 CSV は N 周期で再生を終えて一時停止" sh -c "grep -F -q '\"step\":3' '$WORK/three_info' && grep -F -q '\"running\":false' '$WORK/three_info' && grep -F -q '\"playback\":false' '$WORK/three_info'"
    curl -s "$U/api/export.csv" > "$WORK/three_export.csv"
    "$SIM" run "$WORK/three_export.csv" -o "$WORK/othree" --quiet > "$WORK/three_run.log" 2>&1
    RC=$?
    check "N 行 CSV の記録は余分な周期なしで 3 行 OK" sh -c "[ $RC -eq 0 ] && grep -F -q 'OK 3, NG 0, NO Check 0' '$WORK/three_run.log'"

    # 長い周期の待機は /api/control で起こせる。周期 5000ms で run（1 ステップ目はすぐ）→ pause → step が
    # 周期の終わりを待たずに応答し、ステップが 2 に進む（直す前は周期の終わりまで約 4.7 秒待たされた）
    ctl() { curl -s -o "$WORK/$2" -w '%{http_code}' --max-time "$3" -X POST --data-binary "@$T/gui/$1" "$U/api/control"; }
    step_of() { sed -n 's/.*"step":\([0-9]*\).*/\1/p' "$1"; }
    ctl reset.json lp0.out 2 > /dev/null
    ctl run_long.json lp1.out 2 > /dev/null
    sleep 0.3
    CODE=$(ctl pause.json lp2.out 1.5)
    check "長い周期: 実行中の pause がすぐ応答する" test "$CODE" = 200
    CODE=$(ctl step1.json lp3.out 1.5)
    check "長い周期: pause の後の step がすぐ応答する（1.5 秒以内に 200）" test "$CODE" = 200
    check "長い周期: pause の後の step でステップが 2 に進む" contains "$WORK/lp3.out" '"running":false,"step":2,'
    # 実行中に周期を短くすると、古い周期の終わりを待たずに新しい周期で進む
    ctl reset.json lp4.out 2 > /dev/null
    ctl run_long.json lp5.out 2 > /dev/null
    sleep 0.3
    ctl period_short.json lp6.out 1.5 > /dev/null
    sleep 0.5
    curl -s --max-time 2 "$U/api/info" > "$WORK/lp7.json"
    N=$(step_of "$WORK/lp7.json")
    check "長い周期: 実行中に周期を 2ms にするとすぐ進む（0.5 秒で 20 ステップ以上。実際 ${N:-?}）" test "${N:-0}" -ge 20
    # 長くした周期は前のステップから数える（0.5 秒の間に 1 ステップより多くは進まない）
    ctl period_long.json lp8.out 1.5 > /dev/null
    curl -s --max-time 2 "$U/api/info" > "$WORK/lp9.json"
    N1=$(step_of "$WORK/lp9.json")
    sleep 0.5
    curl -s --max-time 2 "$U/api/info" > "$WORK/lp10.json"
    N2=$(step_of "$WORK/lp10.json")
    check "長い周期: 周期を 5000ms に戻すと 0.5 秒では進まない（${N1:-?} -> ${N2:-?}）" test "$((${N2:-0} - ${N1:-0}))" -le 1
    # 長い周期の待機中に最速（speed 0）にするとすぐ最速で進む
    ctl speed_fast.json lp11.out 1.5 > /dev/null
    sleep 0.3
    CODE=$(ctl pause.json lp12.out 1.5)
    N3=$(step_of "$WORK/lp12.out")
    check "長い周期: 待機中に最速にするとすぐ進む（0.3 秒で 1000 ステップ以上。実際 ${N3:-?}）" sh -c "[ '$CODE' = 200 ] && [ '${N3:-0}' -ge 1000 ]"
    # 5 秒で終わらない step は、進んだステップ数・残り・実行中であることを応答で知らせ、続きは pause で止められる
    ctl speed_restore.json lp13.out 2 > /dev/null
    CODE=$(ctl step_huge.json lp14.out 8)
    check "終わらない step は 5 秒で 200 を返す" test "$CODE" = 200
    check "終わらない step の応答に timedOut と実行中" sh -c "grep -F -q '\"running\":true' '$WORK/lp14.out' && grep -F -q '\"timedOut\":true' '$WORK/lp14.out' && grep -q '\"stepsLeft\":[1-9][0-9]*,\"stepsDone\":[1-9]' '$WORK/lp14.out'"
    CODE=$(ctl pause.json lp15.out 1.5)
    check "終わらない step を pause で止められる" sh -c "[ '$CODE' = 200 ] && grep -F -q '\"running\":false' '$WORK/lp15.out' && grep -F -q '\"stepsLeft\":0' '$WORK/lp15.out'"
    CODE=$(ctl step1.json lp16.out 1.5)
    check "終わる step の応答は timedOut が false" sh -c "[ '$CODE' = 200 ] && grep -F -q '\"stepsLeft\":0,\"stepsDone\":1,\"timedOut\":false' '$WORK/lp16.out'"
    ctl reset.json lp17.out 2 > /dev/null

    PORT_USED_LOG="$WORK/gui2.log"
    "$SIM" gui --no-browser --port "$PORT" > "$PORT_USED_LOG" 2>&1
    RC=$?
    check "使用中のポートは分かるエラー（終了コード 2）" test "$RC" -eq 2
    check "使用中のポートのメッセージ" contains "$PORT_USED_LOG" "で待ち受けられません"
    kill "$GUI_PID" 2>/dev/null
    wait "$GUI_PID" 2>/dev/null
    GUI_PID=""
fi


echo "== modi initialization"
run_cmp modi_every.csv 0
run_cmp modi_auto_once.csv 0
"$SIM" run "$T/csv/modi_once.csv" --csv-init=once -o "$WORK/modi_once" --fixed-time > "$WORK/modi.log" 2>&1
RC=$?
check "modi once distinct second row" test "$RC" -eq 0
check "modi once preserves header bytes" cmp -s "$WORK/modi_once/modi_once.csv" "$T/expected/modi_once.csv"
"$SIM" run "$WORK/modi_once/modi_once.csv" --csv-init=once -o "$WORK/modi_reread" --fixed-time > "$WORK/modi.log" 2>&1
RC=$?
check "modi result CSV reread" test "$RC" -eq 0
printf "csv_init = once\n" > "$WORK/csv_init.cfg"
"$SIM" run "$T/csv/modi_every.csv" --cfg "$WORK/csv_init.cfg" --csv-init=every -o "$WORK/modi_override" > "$WORK/modi.log" 2>&1
RC=$?
check "csv init CLI overrides cfg" test "$RC" -eq 0
for bad in missing range duplicate values; do
    case "$bad" in
    missing) printf "modi,intr_tm00,t,0,0\n#InitOnce,no_such_var\n0,1\n#COMMENT\nstep 1\n" ;;
    range) printf "modi,intr_tm00,t,0,0\n#InitOnce,g_mode\n0,256\n#COMMENT\nstep 1\n" ;;
    duplicate) printf "modi,intr_tm00,t,0,0\n#InitOnce,g_mode\n0,1\n#InitOnce,g_mode\n0,2\n#COMMENT\nstep 1\n" ;;
    values) printf "modi,intr_tm00,t,0,0\n#InitOnce,g_mode\n0,1\n0,2\n#COMMENT\nstep 1\n" ;;
    esac > "$WORK/modi_bad.csv"
    "$SIM" run "$WORK/modi_bad.csv" -o "$WORK/modi_bad" > "$WORK/modi.log" 2>&1
    RC=$?
    check "modi rejects $bad" test "$RC" -eq 2
done


"$SIM" run "$T/defines/var_mock.csv" --cfg "$T/defines/var_mock.cfg" -o "$WORK/var_mock" > "$WORK/var_mock.log" 2>&1
RC=$?
check "var and varfile byte word bit range array float signed mock" test "$RC" -eq 0
"$SIM" list --cfg "$T/defines/var_mock.cfg" > "$WORK/var_list.log" 2>&1
check "configured vars in list" contains "$WORK/var_list.log" "TBL"

"$SIM" run "$T/defines/types_mock.csv" --cfg "$T/defines/mock.cfg" -o "$WORK/types_mock" > "$WORK/types_mock.log" 2>&1
RC=$?
check "header arrays nested union and mixed bit storage" test "$RC" -eq 0
if [ "$RC" -ne 0 ]; then cat "$WORK/types_mock.log"; fi
for bad in path address varbit vararray policy; do
    case "$bad" in
    path) printf "defines = missing/no_such_header.h\n" ;;
    address) printf "var = BAD : u16 @ 0xFFFFF\n" ;;
    varbit) printf "var = BAD : u8.8 @ 0xFF01\n" ;;
    vararray) printf "var = BAD : u8[0] @ 0xF900\n" ;;
    policy) printf "csv_init = invalid\n" ;;
    esac > "$WORK/bad_defs.cfg"
    "$SIM" info --cfg "$WORK/bad_defs.cfg" > "$WORK/bad_defs.log" 2>&1
    RC=$?
    check "definition config rejects $bad" test "$RC" -eq 2
done
MOCK_PASS=$PASS
MOCK_FAIL=$FAIL
. "$T/rl78/run_rl78_tests.sh"

echo
echo "モック・バックエンド: $((MOCK_PASS + MOCK_FAIL)) 件（失敗 $MOCK_FAIL 件）、本物のバックエンド: $((PASS + FAIL - MOCK_PASS - MOCK_FAIL)) 件（失敗 $((FAIL - MOCK_FAIL)) 件）"
echo "実行 $((PASS + FAIL)) 件、成功 $PASS 件、失敗 $FAIL 件"
[ -n "${CYCLE_TIME:-}" ] && echo "2000 行 CSV（2100 行）の $CYCLE_TIME"
[ -n "${PERF_SUMMARY:-}" ] && printf '%s\n' "$PERF_SUMMARY"
if [ $((PASS + FAIL)) -eq 0 ]; then exit 1; fi
[ "$FAIL" -eq 0 ]
