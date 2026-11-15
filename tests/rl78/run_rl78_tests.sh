# tests/rl78/run_rl78_tests.sh - 本物のバックエンド（RL78 命令シミュレータ）のテスト
#
# tests/run_tests.sh から . で読み込む（ok / ng / check / contains / WORK / ROOT / T / GUI_PID を共有する）。
#   - ルネサス公式サンプル（CC-RL でビルドされた .abs 2 件、tests/fixtures/renesas）
#   - tests/tools/mkabs.c で作るテスト用 .abs（DWARF 2/3/4 となし）。機械語は tests/tools/testprog.s
R="$T/rl78"
FX="$T/fixtures/renesas"
G22="$FX/g22_adc/r01an6640-rl78g22-adc.abs"
G23="$FX/g23_spi/r01an5890_rl78g23_serial_slave.abs"
CC=${CC:-clang}

echo "== 本物のバックエンド: ビルド"
if [ -z "${SIM_RL78:-}" ]; then
    if "$ROOT/build.sh" "$WORK/real" > "$WORK/build_real.log" 2>&1; then ok "build.sh（本物のバックエンド）"; else ng "build.sh（本物のバックエンド）"; cat "$WORK/build_real.log"; fi
    if grep -q -i "warning" "$WORK/build_real.log"; then ng "本物のバックエンドを警告なしでビルド" "$(grep -i warning "$WORK/build_real.log" | head -3)"; else ok "本物のバックエンドを警告なしでビルド"; fi
    SIM_RL78="$WORK/real/sim"
fi
RS="$SIM_RL78"
if "$CC" -std=c11 -O2 -Wall -Wextra -o "$WORK/mkabs" "$T/tools/mkabs.c" > "$WORK/mkabs.log" 2>&1; then ok "tests/tools/mkabs.c をビルド"; else ng "tests/tools/mkabs.c をビルド" "$(head -3 "$WORK/mkabs.log")"; fi
for k in dwarf2 dwarf3 dwarf4 nodwarf; do
    check "mkabs $k" "$WORK/mkabs" "$k" "$WORK/tp_$k.abs"
done
TP="$WORK/tp_dwarf2.abs"
for version in dwarf2 dwarf3 dwarf4; do
    for const_name in g_const_mixed.ro 'g_const_elements[0]' g_const_record.w; do
        printf 'mod,f_bits,const path,1,0\n#COMMENT,%s\n,1\n' "$const_name" > "$WORK/dwarf_const.csv"
        "$RS" run --abs "$WORK/tp_$version.abs" "$WORK/dwarf_const.csv" -o "$WORK/dwarf_const" > "$WORK/dwarf_const.log" 2>&1
        rc=$?
        check "$version const input $const_name rejected" test "$rc" -eq 2
    done
    printf 'mod,f_bits,const path,1,1\n#COMMENT,g_const_mixed.rw,g_const_mixed.rw\n,123,123\n' > "$WORK/dwarf_mutable.csv"
    "$RS" run --abs "$WORK/tp_$version.abs" "$WORK/dwarf_mutable.csv" -o "$WORK/dwarf_mutable" > "$WORK/dwarf_const.log" 2>&1
    rc=$?
    check "$version mutable sibling writable" test "$rc" -eq 0
done


# 結果の合計行を確かめる: rl78_run 名前 期待する終了コード 期待する合計 -- sim run の引数...
rl78_run() {
    _name=$1; _code=$2; _sum=$3; shift 4
    "$RS" run "$@" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
    _rc=$?
    if [ "$_rc" -eq "$_code" ] && grep -F -q -- "$_sum" "$WORK/r.log"; then ok "$_name"
    else ng "$_name" "終了コード ${_rc}（期待 ${_code}）、$(grep -E '合計|エラー' "$WORK/r.log" | head -3)"; fi
}

echo "== 本物の .abs（ルネサス公式サンプル、CC-RL V1.12）"
"$RS" info --abs "$G23" > "$WORK/g23_info.txt" 2>&1
check "G23: sim info" test $? -eq 0
check "G23: ELF の e_machine 197" contains "$WORK/g23_info.txt" "e_machine 197（RL78）"
check "G23: DWARF 2 と 3 の CU を読む" contains "$WORK/g23_info.txt" "CU 18 個（バージョン 2: 3、バージョン 3: 15）、飛ばした CU 0 個"
check "G23: スタートアップが main まで走る" grep -q "初期設定終了: main（0x001C9）。スタートアップ [0-9]* 命令" "$WORK/g23_info.txt"
check "G23: リンクマップのセクションと一致" contains "$WORK/g23_info.txt" "セクション 17 件一致 / 0 件不一致"
check "G23: ミラーは .const の位置から" contains "$WORK/g23_info.txt" "ミラー: 0xF3000〜0xFBEFF <- 0x03000"
check "G23: RAM とスタックは __RAM_ADDR_* / __STACK_ADDR_* から" contains "$WORK/g23_info.txt" "RAM: 0xFBF00〜0xFFEDF、スタック: 0xFBF4A〜0xFFE1F、呼び出し時の SP: 0xFFE1C"
"$RS" info --abs "$G22" > "$WORK/g22_info.txt" 2>&1
check "G22: スタートアップが main まで走る" contains "$WORK/g22_info.txt" "初期設定終了: main（0x00144）"
check "G22: リンクマップのセクションと一致" contains "$WORK/g22_info.txt" "セクション 17 件一致 / 0 件不一致"
"$RS" list --abs "$G23" > "$WORK/g23_list.txt" 2>/dev/null
check "G23: 変数 15・関数 64 が readelf で確かめた一覧どおり" cmp -s "$WORK/g23_list.txt" "$R/expected/g23_list.txt"
"$RS" list --abs "$G22" > "$WORK/g22_list.txt" 2>/dev/null
check "G22: 変数 3・関数 31 が readelf で確かめた一覧どおり" cmp -s "$WORK/g22_list.txt" "$R/expected/g22_list.txt"
"$RS" run --abs "$G23" "$R/csv/g23_spi.csv" -o "$WORK/r_g23" --fixed-time --quiet > "$WORK/r.log" 2>&1
check "G23: 関数呼び出し・near ポインタ・SFR・const（16 行すべて OK）" contains "$WORK/r.log" "合計: 総数 16, OK 16, NG 0, NO Check 0"
check "G23: 結果 CSV が期待どおり（バイト単位）" cmp -s "$WORK/r_g23/g23_spi.csv" "$R/expected/g23_spi.csv"
check "G23: 実行命令数と命令/秒を表示" grep -q "^実行命令数: [0-9]*（スタートアップ [0-9]* を除く）、呼び出し 16 回" "$WORK/r.log"
"$RS" run --abs "$G22" "$R/csv/g22_adc.csv" -o "$WORK/r_g22" --fixed-time --quiet > "$WORK/r.log" 2>&1
check "G22: enum 引数と MD_STATUS の戻り値、SFR の直接指定（18 行すべて OK）" contains "$WORK/r.log" "合計: 総数 18, OK 18, NG 0, NO Check 0"
check "G22: 結果 CSV が期待どおり（バイト単位）" cmp -s "$WORK/r_g22/g22_adc.csv" "$R/expected/g22_adc.csv"
rl78_run "G22: RAM を 0xA5 で埋めて始めても .data がコピーされる" 0 "合計: 総数 1, OK 1, NG 0" -- --cfg "$R/csv/g22_fill.cfg" "$R/csv/g22_startup.csv"
rl78_run "G23: RAM を 0xA5 で埋めて始めても bss が 0 になる" 0 "合計: 総数 1, OK 1, NG 0" -- --cfg "$R/csv/g23_fill.cfg" "$R/csv/g23_startup.csv"

echo "== テスト用の .abs（mkabs）: 型・変数・関数の読み取り"
"$RS" list --abs "$TP" > "$WORK/tp_list.txt" 2>/dev/null
check "DWARF2: 一覧が readelf で確かめたものどおり" cmp -s "$WORK/tp_list.txt" "$R/expected/synth_list.txt"
"$RS" list --abs "$WORK/tp_dwarf3.abs" > "$WORK/tp_list3.txt" 2>/dev/null
check "DWARF3: DWARF2 と同じ一覧" cmp -s "$WORK/tp_list3.txt" "$R/expected/synth_list.txt"
"$RS" list --abs "$WORK/tp_dwarf4.abs" > "$WORK/tp_list4.txt" 2>/dev/null
check "DWARF4（.debug_str、data_bit_offset、exprloc）: DWARF2 と同じ一覧" cmp -s "$WORK/tp_list4.txt" "$R/expected/synth_list.txt"
"$RS" list --abs "$WORK/tp_nodwarf.abs" > "$WORK/tp_listn.txt" 2>/dev/null
check "DWARF なし: .symtab と .map から一覧" cmp -s "$WORK/tp_listn.txt" "$R/expected/synth_nodwarf_list.txt"
"$RS" info --abs "$TP" -v > "$WORK/tp_info.txt" 2>&1
check "独自属性 0x2001 を読み飛ばして警告" contains "$WORK/tp_info.txt" "知らない属性 0x2001"
check "独自属性 0x3fe1（block1）を読み飛ばして警告" contains "$WORK/tp_info.txt" "知らない属性 0x3fe1"
check "知らないタグの中の属性も読み飛ばす" contains "$WORK/tp_info.txt" "知らない属性 0x2005"
check "ヒープは使われていない RAM から自動で選ぶ" contains "$WORK/tp_info.txt" "ヒープ（\$ptr）: 0xFE160〜0xFEFFF（自動: 使われていない RAM）"
check "番兵は未使用の領域" contains "$WORK/tp_info.txt" "戻り番地の番兵: 0xEFFF0"
"$RS" info --abs "$TP" > "$WORK/tp_info2.txt" 2>&1
check "--verbose なしでは警告の件数だけ" contains "$WORK/tp_info2.txt" "読み込みの警告が 3 件あります（--verbose で表示）"

echo "== テスト用の .abs: CC-RL の呼び出し規約と型"
for k in dwarf2 dwarf3 dwarf4; do
    rl78_run "$k: 引数・戻り値・スタック渡し・far/near ポインタ・static・RETI・MULHU・MDU・テストドライバ・型（43 行）" 0 "合計: 総数 43, OK 43, NG 0, NO Check 0" -- --abs "$WORK/tp_$k.abs" "$R/csv/synth_call.csv" "$R/csv/synth_types.csv"
done
"$RS" run --abs "$TP" "$R/csv/synth_call.csv" "$R/csv/synth_types.csv" -o "$WORK/r_tp" --fixed-time --quiet > /dev/null 2>&1
check "呼び出し規約の結果 CSV が期待どおり（バイト単位）" cmp -s "$WORK/r_tp/synth_call.csv" "$R/expected/synth_call.csv"
check "型の結果 CSV が期待どおり（バイト単位）" cmp -s "$WORK/r_tp/synth_types.csv" "$R/expected/synth_types.csv"
rl78_run "DWARF なし: 'name:バイト数' と .map だけのシンボル" 0 "合計: 総数 1, OK 1, NG 0" -- --abs "$WORK/tp_nodwarf.abs" "$R/csv/synth_nodwarf.csv"
printf 'mod,TEST_cycle,x,0,1\n#COMMENT,g_a\n,0\n' > "$WORK/nod_err.csv"
"$RS" run --abs "$WORK/tp_nodwarf.abs" "$WORK/nod_err.csv" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
RC=$?
check "DWARF なしで型のない変数はバイト数を求めるエラー（終了コード 2）" test "$RC" -eq 2
check "DWARF なしのエラーメッセージ" contains "$WORK/r.log" "'g_a:2' のようにバイト数を付けてください"

echo "== テスト用の .abs: 時間の列（クロック数と cpu_hz から）"
printf 'mod,TEST_cycle,x,0,1\n#COMMENT,TEST_cycle_cnt\n,1\n,2\n' > "$WORK/cyc2.csv"
"$RS" run --abs "$TP" "$WORK/cyc2.csv" -o "$WORK/r_t32" --quiet > /dev/null 2>&1
printf 'cpu_hz = 16000000\n' > "$WORK/hz16.cfg"
"$RS" run --abs "$TP" --cfg "$WORK/hz16.cfg" "$WORK/cyc2.csv" -o "$WORK/r_t16" --quiet > /dev/null 2>&1
T32=$(sed -n 3p "$WORK/r_t32/cyc2.csv" | awk -F, '{print $4}' | tr -d 'ms\r')
T32B=$(sed -n 4p "$WORK/r_t32/cyc2.csv" | awk -F, '{print $4}' | tr -d 'ms\r')
T16=$(sed -n 3p "$WORK/r_t16/cyc2.csv" | awk -F, '{print $4}' | tr -d 'ms\r')
check "時間の列は実行のたびに同じ（クロック数から換算、${T32}ms）" test -n "$T32" -a "$T32" = "$T32B"
check "cpu_hz を半分にすると時間は倍（${T32}ms -> ${T16}ms）" awk -v a="$T32" -v b="$T16" 'BEGIN { exit !(a > 0 && b > 0 && b / a > 1.99 && b / a < 2.01) }'

echo "== テスト用の .abs: 異常系"
rl78_run "無限ループと未定義命令はその行を NG にして続ける（終了コード 1）" 1 "合計: 総数 3, OK 1, NG 2, NO Check 0" -- --abs "$TP" --cfg "$R/cfg/limit.cfg" "$R/csv/synth_fault.csv"
check "暴走のメッセージに PC と逆アセンブル" contains "$WORK/r.log" "命令数の上限 100000 を超えました。無限ループか暴走の可能性があります（PC=0x002A5（f_loop 内）: EF FE  BR \$002A5H）"
check "未定義命令のメッセージに PC とバイト列" contains "$WORK/r.log" "未定義命令を実行しようとしました（PC=0x002A7、f_undef 内、バイト列 61 19"
rl78_run "halt = stop なら HALT で NG" 1 "合計: 総数 1, OK 0, NG 1" -- --abs "$TP" --cfg "$R/cfg/halt_stop.cfg" "$R/csv/synth_halt.csv"
check "HALT のメッセージ" contains "$WORK/r.log" "HALT 命令で止まりました"
printf 'init_end = no_such_func\n' > "$WORK/badinit.cfg"
"$RS" run --abs "$TP" --cfg "$WORK/badinit.cfg" "$R/csv/synth_halt.csv" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
RC=$?
check "init_end が見つからなければ終了コード 2" test "$RC" -eq 2
check "init_end のメッセージ" contains "$WORK/r.log" "init_end = no_such_func が見つかりません"
printf 'startup_max_insns = 50\n' > "$WORK/short.cfg"
"$RS" run --abs "$TP" --cfg "$WORK/short.cfg" "$R/csv/synth_halt.csv" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
RC=$?
check "スタートアップが終わらなければ終了コード 2" test "$RC" -eq 2
check "スタートアップのメッセージに PC" grep -q "スタートアップ（リセットから main まで）: 命令数の上限 50 を超えました.*PC=0x" "$WORK/r.log"
printf 'mod,engine_inc,x,0,1\n#COMMENT,s_cnt\n,0\n' > "$WORK/amb.csv"
"$RS" run --abs "$TP" "$WORK/amb.csv" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
check "同名のファイル内 static はファイル名を求める" contains "$WORK/r.log" "engine.c/s_cnt sensor.c/s_cnt"
printf 'mod,f_bits,x,1,0\n#COMMENT,g_lut[0]\n,1\n' > "$WORK/wconst.csv"
"$RS" run --abs "$TP" "$WORK/wconst.csv" -o "$WORK/r_out" > "$WORK/r.log" 2>&1
check "const（ROM の変数）には書けない" contains "$WORK/r.log" "'g_lut[0]' には書き込めません"
# ヒープの排他的終端が奇数でも、最後のバイトの後の確保をヒープ外（0xFE162）に置かない
rl78_run "1 バイトのヒープ（終端が奇数）で 2 回目の \$ の確保はエラー（終了コード 2）" 2 "ヒープ（0xFE160〜0xFE160）が足りません" -- --abs "$TP" --cfg "$R/cfg/heap_odd.cfg" "$R/csv/synth_heap_odd.csv"
check "ヒープ外の 2 回目の確保は結果に OK と書かない" sh -c "! grep -q '^,1,0,0,0xE162,OK' '$WORK/r_out/synth_heap_odd.csv'"
# 読めない設定値（範囲外・単位付き・負の値）は、既定値や自動の値に置き換えずにエラー（終了コード 2）
cfg_err() {
    _name=$1; _cfg=$2; _msg=$3
    "$RS" info --abs "$TP" --cfg "$R/cfg/$_cfg" > "$WORK/r.log" 2>&1
    _rc=$?
    if [ "$_rc" -eq 2 ] && grep -F -q -- "$_msg" "$WORK/r.log"; then ok "$_name"
    else ng "$_name" "終了コード ${_rc}、出力: $(head -2 "$WORK/r.log")"; fi
}
cfg_err "アドレス空間を越えるヒープの指定は自動で選び直さずにエラー" heap_over.cfg "sim.cfg: heap = 0xFFFF0:0x100 を読めません"
cfg_err "cpu_hz = 32MHz を 32 Hz にしない" bad_cpu_hz.cfg "sim.cfg: cpu_hz = 32MHz を読めません"
cfg_err "max_insns = -1 を 2^64-1 にしない" bad_max_insns.cfg "sim.cfg: max_insns = -1 を読めません"
cfg_err "sentinel = 0x1EFFF0 を 0xEFFF0 に切り詰めない" bad_sentinel.cfg "sim.cfg: sentinel = 0x1EFFF0 を読めません"
printf 'bogus_key = 1\n' > "$WORK/bogus.cfg"
"$RS" info --abs "$TP" --cfg "$WORK/bogus.cfg" -v > "$WORK/r.log" 2>&1
check "知らない設定は警告" contains "$WORK/r.log" "知らない設定 'bogus_key'"

echo "== テスト用の .abs: CPU コア種別とスタブ"
rl78_run "core = S2: MULHU の命令列は別の命令になり、乗除算器 SFR は使える" 0 "合計: 総数 2, OK 2, NG 0" -- --abs "$TP" --cfg "$R/cfg/core_s2.cfg" "$R/csv/synth_core.csv"
rl78_run "core = S3、mdu = off: MULHU は使え、乗除算器 SFR はただのメモリ" 0 "合計: 総数 2, OK 2, NG 0" -- --abs "$TP" --cfg "$R/cfg/core_s3_nomdu.cfg" "$R/csv/synth_nomdu.csv"
rl78_run "stub = read_io:AMSTB_read_io" 0 "合計: 総数 3, OK 3, NG 0" -- --abs "$TP" --cfg "$R/cfg/stub.cfg" "$R/csv/synth_stub.csv"
rl78_run "stub_auto = on" 0 "合計: 総数 3, OK 3, NG 0" -- --abs "$TP" --cfg "$R/cfg/stub_auto.cfg" "$R/csv/synth_stub.csv"
rl78_run "スタブなしなら元の関数" 0 "合計: 総数 1, OK 1, NG 0" -- --abs "$TP" "$R/csv/synth_nostub.csv"
rl78_run "CSV の % 行で置換（sim.cfg なし）" 0 "合計: 総数 3, OK 3, NG 0" -- --abs "$TP" "$R/csv/synth_stub_line.csv"
check "% 行の置換を表示する" contains "$WORK/r.log" "置換（CSV の % 行）: read_io -> AMSTB_read_io"
check "% 行は結果 CSV にそのまま残る" grep -q '^%,"AMSTB_read_io","read_io"' "$WORK/r_out/synth_stub_line.csv"
rl78_run "% 行の置換は次のブロック・次の CSV に持ち越さない" 0 "合計: 総数 4, OK 4, NG 0" -- --abs "$TP" "$R/csv/synth_stub_line.csv" "$R/csv/synth_nostub.csv"
rl78_run "% 行は sim.cfg の stub より優先し、ブロックが終われば sim.cfg に戻る" 0 "合計: 総数 6, OK 6, NG 0" -- --abs "$TP" --cfg "$R/cfg/stub.cfg" "$R/csv/synth_stub_line.csv" "$R/csv/synth_stub.csv"
printf 'mod,use_io,x,2,1\n%%,AMSTB_none,read_io\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@\n,0x0100,0x0222,0x0101\n' > "$WORK/stub_missing.csv"
rl78_run "% 行のスタブ関数がなければそのブロックはエラー" 2 "% 行: スタブ関数 'AMSTB_none' がロードモジュールに見つかりません" -- --abs "$TP" "$WORK/stub_missing.csv"
printf 'mod,use_io,x,2,1\n%%,AMSTB_read_io\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@\n,0x0100,0x0222,0x0101\n' > "$WORK/stub_short.csv"
rl78_run "% 行の列が足りなければエラー" 2 "% 行は %,スタブ関数,元の関数 の形にしてください" -- --abs "$TP" "$WORK/stub_short.csv"
printf 'mod,use_io,x,2,1\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@\n,0x0100,0x0222,0x0101\n%%,AMSTB_read_io,read_io\n' > "$WORK/stub_late.csv"
rl78_run "データ行の後の % 行はエラー" 2 "% 行（置換の指定）がデータ行の後にあります" -- --abs "$TP" "$WORK/stub_late.csv"
printf 'mod,use_io,x,2,2\n%%,"winams/AMSTB_SrcFile.c/AMSTB_read_io","ENGINE.C/read_io"\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@,0xFFF1E:2\n,0x0100,0x0222,0x0223,0x0100\n' > "$WORK/stub_file.csv"
rl78_run "% 行の「ファイル名/関数名」表記（大文字小文字は区別しない）" 0 "合計: 総数 1, OK 1, NG 0" -- --abs "$TP" "$WORK/stub_file.csv"
printf 'mod,use_io,x,2,1\n%%,AMSTB_read_io,sensor.c/read_io\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@\n,0x0100,0x0222,0x0101\n' > "$WORK/stub_wrongfile.csv"
rl78_run "% 行のファイル名が違えば別の関数を置き換えずにエラー" 2 "% 行: 置き換え元の関数 'sensor.c/read_io' がロードモジュールに見つかりません" -- --abs "$TP" "$WORK/stub_wrongfile.csv"
printf 'mod,use_io,x,2,1\n%%,AMSTB_read_io,1ngine.c/read_io\n#COMMENT,0xFFF1E:2,g_stub_val,use_io@@\n,0x0100,0x0222,0x0101\n' > "$WORK/stub_digitfile.csv"
rl78_run "% 行の数字で始まるファイル名をアドレスと取り違えない" 2 "% 行: 置き換え元の関数 '1ngine.c/read_io' がロードモジュールに見つかりません" -- --abs "$TP" "$WORK/stub_digitfile.csv"
mkdir -p "$WORK/autocfg"
cp "$TP" "$WORK/autocfg/prog.abs"
cp "$R/cfg/stub.cfg" "$WORK/autocfg/sim.cfg"
rl78_run ".abs と同じフォルダの sim.cfg を自動で読む" 0 "合計: 総数 3, OK 3, NG 0" -- --abs "$WORK/autocfg/prog.abs" "$R/csv/synth_stub.csv"
printf 'abs = prog.abs\n' > "$WORK/autocfg/only_abs.cfg"
rl78_run "sim.cfg の abs = で .abs を指定（--abs なし）" 0 "合計: 総数 1, OK 1, NG 0" -- --cfg "$WORK/autocfg/only_abs.cfg" "$R/csv/synth_nostub.csv"

echo "== LMF: CA78K0R"
check "mklmf build" "$CC" -std=c11 -O2 -Wall -Wextra -o "$WORK/mklmf" "$T/tools/mklmf.c"
check "mklmf normal" "$WORK/mklmf" normal "$WORK/test.lmf"
# 引数constは呼び出し側の準備を妨げず、通常の変数constは保護する。
rl78_run "LMF const arguments and returns (5 rows)" 0 "総数 5, OK 5, NG 0" -- --abs "$WORK/test.lmf" "$R/csv/const_lmf.csv"
for version in dwarf2 dwarf3 dwarf4; do
    rl78_run "$version const arguments and returns (10 rows)" 0 "総数 10, OK 10, NG 0" -- --abs "$WORK/tp_$version.abs" "$R/csv/const_dwarf.csv"
    for const_name in g_const_pointer 'g_const_target[0]' 'f_bits@constant' 'f_bits@local_const'; do
        printf 'mod,f_bits,const protection,3,0\n#COMMENT,0xFE190:2,0xFE192:2,%s\n,0xE11A,0xE11A,1\n' "$const_name" > "$WORK/const_reject.csv"
        "$RS" run --abs "$WORK/tp_$version.abs" "$WORK/const_reject.csv" -o "$WORK/const_out" > "$WORK/const.log" 2>&1
        rc=$?
        check "$version protects $const_name" test "$rc" -eq 2
    done
    # ポインタ自身が const でも、指す先（const でない型）には書ける
    printf 'mod,f_bits,const pointer target,3,0\n#COMMENT,0xFE190:2,0xFE192:2,g_const_pointer[0]\n,0xE11A,0xE11A,1\n' > "$WORK/const_ok.csv"
    "$RS" run --abs "$WORK/tp_$version.abs" "$WORK/const_ok.csv" -o "$WORK/const_out" > "$WORK/const.log" 2>&1
    rc=$?
    check "$version allows g_const_pointer[0]" test "$rc" -eq 0
    printf 'skip = constret:231\n' > "$WORK/const_skip.cfg"
    printf 'mod,constret,const skip,0,1\n#COMMENT,constret@@\n,231\n' > "$WORK/const_skip.csv"
    rl78_run "$version const return skip" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/tp_$version.abs" --cfg "$WORK/const_skip.cfg" "$WORK/const_skip.csv"
done
for const_name in constant const_pointer 'cycle@constant'; do
    printf 'mod,cycle,const protection,2,0\n#COMMENT,0xFE03E:2,%s\n,0xE004,1\n' "$const_name" > "$WORK/const_reject.csv"
    "$RS" run --abs "$WORK/test.lmf" "$WORK/const_reject.csv" -o "$WORK/const_out" > "$WORK/const.log" 2>&1
    rc=$?
    check "LMF protects $const_name" test "$rc" -eq 2
done
printf 'mod,cycle,const pointer target,2,0\n#COMMENT,0xFE03E:2,const_pointer[0]\n,0xE004,1\n' > "$WORK/const_ok.csv"
"$RS" run --abs "$WORK/test.lmf" "$WORK/const_ok.csv" -o "$WORK/const_out" > "$WORK/const.log" 2>&1
rc=$?
check "LMF allows const_pointer[0]" test "$rc" -eq 0
printf 'skip = constbyte:231\n' > "$WORK/const_skip.cfg"
printf 'mod,constbyte,const skip,0,1\n#COMMENT,constbyte@@\n,231\n' > "$WORK/const_skip.csv"
rl78_run "LMF const return skip" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/test.lmf" --cfg "$WORK/const_skip.cfg" "$WORK/const_skip.csv"

"$RS" info --abs "$WORK/test.lmf" -v > "$WORK/lmf.log" 2>&1
check "LMF info" test $? -eq 0
check "LMF ABI auto" contains "$WORK/lmf.log" "ABI: ca78k0r"
check "LMF unknown type warning" contains "$WORK/lmf.log" "0x0008"
check "LMF unknown storage warning" contains "$WORK/lmf.log" "0x55"
"$RS" list --abs "$WORK/test.lmf" > "$WORK/lmf.list" 2>&1
check "LMF list" test $? -eq 0
check "LMF array symbol" contains "$WORK/lmf.list" "arr"
rl78_run "LMF ABI/startup/driver/unknown size (16 rows)" 0 "総数 16, OK 16, NG 0" -- --abs "$WORK/test.lmf" "$R/csv/lmf_call.csv"
rl78_run "LMF first long accepted" 0 "総数 1, OK 0, NG 0, NO Check 1" -- --abs "$WORK/test.lmf" "$R/csv/lmf_first32.csv"
printf 'abi = ccrl\n' > "$WORK/abi.cfg"
"$RS" info --abs "$WORK/test.lmf" --cfg "$WORK/abi.cfg" > "$WORK/lmf.log" 2>&1
check "ABI override" contains "$WORK/lmf.log" "ABI: ccrl"
printf 'abi = bogus\n' > "$WORK/abi.cfg"
"$RS" info --abs "$WORK/test.lmf" --cfg "$WORK/abi.cfg" > "$WORK/lmf.log" 2>&1
check "Bad ABI rejected" test $? -eq 2
cp "$WORK/test.lmf" "$WORK/lmf.abs"
"$RS" info --abs "$WORK/lmf.abs" > "$WORK/lmf.log" 2>&1
check "LMF identified by bytes" contains "$WORK/lmf.log" "ABI: ca78k0r"
for mode in truncated section symbols aux string unterminated stringcut address lines magic; do
    check "mklmf $mode" "$WORK/mklmf" "$mode" "$WORK/bad.lmf"
    "$RS" info --abs "$WORK/bad.lmf" > "$WORK/lmf.log" 2>&1
    check "LMF malformed $mode rejected" test $? -eq 2
done
check "Unknown format magic bytes" contains "$WORK/lmf.log" "78 56 34 12"

printf '#05\n;FF PUBLIC\n02FE001_init\n=\n' > "$WORK/test.sym"
"$RS" info --abs "$WORK/test.lmf" -v > "$WORK/lmf.log" 2>&1
check "SYM mismatch warning" contains "$WORK/lmf.log" "食い違います"
rm "$WORK/test.sym"
check "mklmf no debug" "$WORK/mklmf" nodbg "$WORK/nodbg.lmf"
# lmfprog.s の main / cycle の固定アドレス（再生成時も確認する）。
printf '#05\n;FF PUBLIC\n010010E_main\n0100141_cycle\n02FE000_init\n02FE002_ticks\n=\n' > "$WORK/nodbg.sym"
printf 'mod,cycle,SYM fallback,1,2\n#COMMENT,ticks:2,init:2,ticks:2\n,4,0x1234,5\n' > "$WORK/sym.csv"
rl78_run "LMF without debug uses SYM addresses" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/nodbg.lmf" "$WORK/sym.csv"

echo "== CA78K0R probe: 型・サイズ・初期値・呼び出し"
PROBE="$T/ca78k0r_probe/DefaultBuild/ca78k0r_probe.lmf"
B="$ROOT/runtime/backends/rl78"
check "LMF type checker build" "$CC" -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"$B" -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" \
    "$T/tools/check_lmf_types.c" "$B/rl78_lmf.c" "$B/rl78_elf.c" "$B/rl78_dwarf.c" "$B/rl78_map.c" "$ROOT/runtime/src/sim_util.c" -o "$WORK/lmf_types"
check "LMF argument top const removed and variable return const retained" "$WORK/lmf_types" "$WORK/test.lmf" constargs
"$WORK/lmf_types" "$PROBE" probe > "$WORK/probe_types.txt" 2> "$WORK/probe_warnings.txt"
check "probe: 57 variables / 17 functions / 0 LMF warnings" test $? -eq 0
check "probe: reference types, addresses, sizes and function signatures" cmp -s "$WORK/probe_types.txt" "$R/expected/probe_types.txt"
if ! cmp -s "$WORK/probe_types.txt" "$R/expected/probe_types.txt"; then diff -u "$R/expected/probe_types.txt" "$WORK/probe_types.txt"; fi
"$RS" list --abs "$PROBE" > "$WORK/probe_list_raw.txt" 2> "$WORK/probe_stderr.txt"
check "probe: sim list exit 0" test $? -eq 0
"$WORK/lmf_types" "$WORK/probe_list_raw.txt" list > "$WORK/probe_list.txt"
check "probe: sim list has 57 typed variables" test $? -eq 0
check "probe: sim list matches lmfdump declarations / addresses" cmp -s "$WORK/probe_list.txt" "$R/expected/probe_list.txt"
"$RS" info --abs "$PROBE" -v > "$WORK/probe_info.txt" 2>&1
check "probe: startup reaches main" contains "$WORK/probe_info.txt" "main（0x00637）"
check "probe: no loader or SYM warnings" sh -c '! grep -E "LMF:|SYM:.*(食い違い|一致しません)|読み込みの警告" "$1"' sh "$WORK/probe_info.txt"
grep -E "^LMF 型診断:|^型情報なし" "$WORK/probe_info.txt"
rl78_run "probe initial values (50 rows)" 0 "総数 50, OK 50, NG 0" -- --abs "$PROBE" "$R/csv/probe_init.csv"
rl78_run "probe calls (17 rows)" 0 "総数 17, OK 17, NG 0" -- --abs "$PROBE" "$R/csv/probe_call.csv"
rl78_run "probe member / bitfield writes (2 rows)" 0 "総数 2, OK 2, NG 0" -- --abs "$PROBE" "$R/csv/probe_write.csv"
rl78_run "probe struct return rejected" 2 "戻り値は構造体" -- --abs "$PROBE" "$R/csv/probe_ret_struct.csv"
rl78_run "probe float argument rejected" 2 "浮動小数の引数には対応していません" -- --abs "$PROBE" "$R/csv/probe_float.csv"
rl78_run "probe char_unsigned" 0 "総数 1, OK 1, NG 0" -- --abs "$PROBE" --cfg "$R/cfg/probe_char.cfg" "$R/csv/probe_char.csv"
rl78_run "generated struct, bitfields and far pointer" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/test.lmf" "$R/csv/lmf_aggregate.csv"
for mode in tagref tagaux tagmismatch tagend tagcycle tagwidth; do
    check "mklmf $mode" "$WORK/mklmf" "$mode" "$WORK/badtag.lmf"
    "$WORK/lmf_types" "$WORK/badtag.lmf" bad > "$WORK/tag.log" 2>&1
    check "malformed $mode becomes unknown without recursion or crash" test $? -eq 0
done
# 型を解決できない引数（struct 型・不正なタグ参照・補助サイズ 2）を整数の規約で呼ばない。変数は従来どおり生の整数。
check "mklmf badarg" "$WORK/mklmf" badarg "$WORK/badarg.lmf"
rl78_run "unresolved LMF argument type is rejected instead of called as integer" 2 "関数 'badarg' の引数 'a'" -- --abs "$WORK/badarg.lmf" "$R/csv/lmf_badarg.csv"
check "unresolved LMF argument: no call was made" contains "$WORK/r.log" "総数 0, OK 0, NG 0, NO Check 0, エラー 1"
rl78_run "unresolved LMF variable still falls back to raw integer" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/badarg.lmf" "$R/csv/lmf_badvar.csv"
"$RS" list --abs "$WORK/badarg.lmf" bad > "$WORK/badarg.list" 2>/dev/null
check "unresolved LMF argument listed as unknown type" contains "$WORK/badarg.list" "badarg(? a)"
check "unresolved LMF variable listed as uint16" grep -q "badvar  *uint16" "$WORK/badarg.list"
rl78_run "probe 32-bit skip result" 0 "総数 1, OK 1, NG 0" -- --abs "$PROBE" --cfg "$R/cfg/probe_skip.cfg" "$R/csv/probe_skip.csv"

echo "== 本物のバックエンドの GUI サーバ"
if command -v curl > /dev/null 2>&1; then
    PORT=$((20000 + ($$ + 7) % 20000))
    U="http://127.0.0.1:$PORT"
    "$RS" gui --abs "$G22" --no-browser --port "$PORT" --html-dir "$WORK/nohtml" > "$WORK/nofunc.log" 2>&1 &
    GUI_PID=$!
    i=0
    while [ $i -lt 50 ]; do curl -s "$U/api/info" > "$WORK/nofunc_before.json" 2>/dev/null && break; i=$((i + 1)); sleep 0.1; done
    check "GUI 未設定: 起動時に対象関数の指定を案内" contains "$WORK/nofunc.log" '対象関数が未設定です'
    for CMD in step run; do
        CODE=$(curl -s -o "$WORK/nofunc_error.json" -w '%{http_code}' -X POST -d "{\"cmd\":\"$CMD\",\"speed\":2,\"periodMs\":10}" "$U/api/control")
        check "GUI 未設定: $CMD は 400" test "$CODE" = 400
        check "GUI 未設定: $CMD のエラーに func の案内" contains "$WORK/nofunc_error.json" 'func'
        curl -s "$U/api/info" > "$WORK/nofunc_after.json"
        check "GUI 未設定: $CMD 拒否で状態は変わらない" cmp -s "$WORK/nofunc_before.json" "$WORK/nofunc_after.json"
    done
    check "GUI 未設定: step は 0 のまま" contains "$WORK/nofunc_after.json" '"step":0'
    CODE=$(curl -s -o "$WORK/nofunc_error.json" -w '%{http_code}' "$U/api/export.csv")
    check "GUI 未設定: 記録のない CSV 保存は 400" test "$CODE" = 400
    check "GUI 未設定: CSV 保存エラーに func の案内" contains "$WORK/nofunc_error.json" 'func'
    CODE=$(curl -s -o "$WORK/nofunc_step.json" -w '%{http_code}' -X POST -d '{"func":"R_Config_ADC_Set_ADChannel","cmd":"step"}' "$U/api/control")
    check "GUI 未設定: func と step の同時指定は成功" test "$CODE" = 200
    check "GUI 未設定: 関数指定後は 1 周期実行" contains "$WORK/nofunc_step.json" '"stepsDone":1'
    kill "$GUI_PID" 2>/dev/null
    wait "$GUI_PID" 2>/dev/null
    GUI_PID=""
    "$RS" gui --abs "$TP" --cfg "$R/cfg/limit.cfg" --func TEST_cycle --no-browser --port "$PORT" --period-ms 2 --html-dir "$WORK/nohtml" > "$WORK/rgui.log" 2>&1 &
    GUI_PID=$!
    U="http://127.0.0.1:$PORT"
    i=0
    while [ $i -lt 50 ]; do curl -s "$U/api/info" > "$WORK/rinfo.json" 2>/dev/null && break; i=$((i + 1)); sleep 0.1; done
    check "GUI: /api/info に backend rl78" contains "$WORK/rinfo.json" '"backend":"rl78"'
    curl -s "$U/api/symbols?q=g_st&limit=2" > "$WORK/rsym.json"
    check "GUI: /api/symbols の members は子の完全名" contains "$WORK/rsym.json" '"name":"g_st","type":"struct S","size":6,"kind":"struct","writable":false,"members":["g_st.fa","g_st.fb","g_st.fc","g_st.pad","g_st.w","g_st.s"]'
    curl -s "$U/api/symbols?q=g_mat&limit=1" > "$WORK/rsym2.json"
    check "GUI: 配列の members は要素の完全名" contains "$WORK/rsym2.json" '"members":["g_mat[0]","g_mat[1]"]'
    check "GUI: RL78 匿名 lo/hi の型・writable・watch/write" "$WORK/check_symbols" "$U" "$WORK" rl78
    printf '{"names":["TEST_cycle_cnt","g_ms_tick","g_st.fb"]}' > "$WORK/rw.json"
    curl -s -X POST --data-binary "@$WORK/rw.json" "$U/api/watch" > /dev/null
    curl -s -N --max-time 2 "$U/api/stream" > "$WORK/rsse.txt" 2>/dev/null &
    SSE_PID=$!
    sleep 0.4
    printf '{"name":"g_st.fb","value":7}' > "$WORK/rw2.json"
    curl -s -X POST --data-binary "@$WORK/rw2.json" "$U/api/write" > /dev/null
    sleep 0.3
    printf '{"cmd":"step","steps":3}' > "$WORK/rs3.json"
    curl -s -X POST --data-binary "@$WORK/rs3.json" "$U/api/control" > "$WORK/rstep.json"
    sleep 0.3
    printf '{"cmd":"reset"}' > "$WORK/rr.json"
    curl -s -X POST --data-binary "@$WORK/rr.json" "$U/api/control" > /dev/null
    wait "$SSE_PID" 2>/dev/null
    check "GUI: step で TEST_cycle を 3 回" contains "$WORK/rstep.json" '"step":3'
    check "GUI: 接続時に event: values（現在値、ステップは進めない）" contains "$WORK/rsse.txt" 'event: values
data: {"step":0,"names":["TEST_cycle_cnt","g_ms_tick","g_st.fb"],"values":[0,0,0]}'
    check "GUI: 停止中の書き込みはすぐには反映せず state の pending に出す" contains "$WORK/rsse.txt" '"pending":[{"name":"g_st.fb","value":"7"}]'
    check "GUI: 停止中の書き込みの後も event: values はメモリの値（g_st.fb = 0）" sh -c "! grep -q '\"step\":0,\"names\":\[\"TEST_cycle_cnt\",\"g_ms_tick\",\"g_st.fb\"\],\"values\":\[0,0,7\]' '$WORK/rsse.txt'"
    check "GUI: ステップの後の samples（実行した値）" contains "$WORK/rsse.txt" '"rows":[[1,10,7],[2,20,7],[3,30,7]]'
    check "GUI: reset の後に event: values で初期状態の値" sh -c "grep -A1 'event: values' '$WORK/rsse.txt' | tail -1 | grep -q '\"values\":\[0,0,0\]'"
    # 呼び出し失敗は通常の測定点として保存しない。復旧後の記録は再実行できる。
    for fault in f_undef f_loop; do
        curl -s -X POST --data '{"names":["g_out"]}' "$U/api/watch" > /dev/null
        curl -s -X POST --data "{\"cmd\":\"reset\",\"func\":\"$fault\"}" "$U/api/control" > /dev/null
        curl -s -X POST --data '{"cmd":"step","steps":1}' "$U/api/control" > "$WORK/fault.json"
        check "GUI $fault: stepsDone は 0" contains "$WORK/fault.json" '"stepsDone":0'
        check "GUI $fault: step は 0 のまま" contains "$WORK/fault.json" '"step":0'
        check "GUI $fault: 実行停止" contains "$WORK/fault.json" '"running":false'
        check "GUI $fault: 残りの step も停止" contains "$WORK/fault.json" '"stepsLeft":0'
        check "GUI $fault: PC を含むエラー" contains "$WORK/fault.json" 'PC=0x002A'
        check "GUI $fault: エラーにリセットの案内" contains "$WORK/fault.json" 'リセットしてからやり直してください'
        if [ "$fault" = f_undef ]; then
            check "GUI f_undef: エラーに命令バイト列" contains "$WORK/fault.json" '61 19'
        else
            check "GUI f_loop: エラーに命令数上限と逆アセンブル" contains "$WORK/fault.json" 'BR $002A5H'
        fi
        curl -s "$U/api/export.csv" > "$WORK/fault.csv"
        check "GUI $fault: 保存 CSV にデータ行なし" sh -c '[ "$(wc -l < "$1")" -eq 2 ]' sh "$WORK/fault.csv"
        check "GUI $fault: 呼び出し失敗で needReset true" contains "$WORK/fault.json" '"needReset":true'
        CODE=$(curl -s -o "$WORK/fault_rejected.json" -w '%{http_code}' -X POST --data '{"cmd":"step","steps":3}' "$U/api/control")
        check "GUI $fault: リセット前の step は 400" test "$CODE" = 400
        curl -s "$U/api/info" > "$WORK/fault_info.json"
        check "GUI $fault: 拒否後も step 0、要リセット" sh -c "grep -F -q '\"step\":0,' '$WORK/fault_info.json' && grep -F -q '\"needReset\":true' '$WORK/fault_info.json'"
        curl -s -X POST --data '{"cmd":"reset","func":"TEST_cycle"}' "$U/api/control" > "$WORK/recovered.json"
        check "GUI $fault: reset で needReset 解除" contains "$WORK/recovered.json" '"needReset":false'
        check "GUI $fault: reset でエラー解除" contains "$WORK/recovered.json" '"error":""'
        curl -s -X POST --data '{"names":["TEST_cycle_cnt"]}' "$U/api/watch" > /dev/null
        curl -s -X POST --data '{"cmd":"step","steps":3}' "$U/api/control" > "$WORK/recovered.json"
        check "GUI $fault: reset 後は 3 周期進む" contains "$WORK/recovered.json" '"stepsDone":3'
        curl -s "$U/api/export.csv" > "$WORK/recovered.csv"
        rl78_run "GUI $fault: 復旧後の保存 CSV は 3 行すべて OK" 0 "OK 3, NG 0" -- --abs "$TP" --cfg "$R/cfg/limit.cfg" "$WORK/recovered.csv"

        # CSV 再生での失敗も再生・実行を停止する（先頭行は直ちに実行される）。
        printf 'mod,%s,fault,0,1\n#COMMENT,g_out\n,0\n,0\n' "$fault" > "$WORK/fault_play.csv"
        curl -s -X POST --data-binary "@$WORK/fault_play.csv" "$U/api/load-csv" > /dev/null
        sleep 0.2
        curl -s "$U/api/info" > "$WORK/fault_info.json"
        check "GUI $fault: 失敗した CSV 再生を停止" contains "$WORK/fault_info.json" '"playback":false'
        check "GUI $fault: 再生失敗で実行停止" contains "$WORK/fault_info.json" '"running":false'
        check "GUI $fault: 再生失敗で step は 0" contains "$WORK/fault_info.json" '"step":0'
    done
    kill "$GUI_PID" 2>/dev/null
    wait "$GUI_PID" 2>/dev/null
    GUI_PID=""
fi


echo "== スタートアップ診断と skip / poke"
check "mkabs startup" "$WORK/mkabs" startup "$WORK/startup.abs"
"$RS" info --abs "$WORK/startup.abs" --cfg "$R/cfg/startup_limit.cfg" --trace-startup --trace-head 1 --trace-tail 1 > "$WORK/trace.log" 2>&1
RC=$?
check "SFR待ち: 終了コード2" test "$RC" -eq 2
check "SFR待ち: 呼び出し履歴" contains "$WORK/trace.log" "#1"
check "SFR待ち: 停止した関数" contains "$WORK/trace.log" "wait_sfr+0x"
check "SFR待ち: ループ範囲" contains "$WORK/trace.log" "ループ候補: 0x"
check "SFR待ち: SFR番地" contains "$WORK/trace.log" "0xFFFA2 = 0x00"
check "SFR待ち: 拡張SFR番地" contains "$WORK/trace.log" "0xF00A8 = 0x00"
check "SFR待ち: バックトレース" contains "$WORK/trace.log" "trace_boot+0x"
"$RS" info --abs "$WORK/startup.abs" --cfg "$R/cfg/startup_limit.cfg" > "$WORK/plain.log" 2>&1
RC=$?
check "診断なしの失敗: 終了コード2" test "$RC" -eq 2
check "診断なしの失敗: 案内" contains "$WORK/plain.log" "--trace-startup"
for setting in poke skip; do
    "$RS" info --abs "$WORK/startup.abs" --cfg "$R/cfg/startup_$setting.cfg" --trace-startup > "$WORK/trace.log" 2>&1
    RC=$?
    check "$setting で起動成功" test "$RC" -eq 0
    check "$setting 成功時も診断表示" contains "$WORK/trace.log" "スタートアップ診断: 成功"
    check "$setting 設定一覧" contains "$WORK/trace.log" "$setting ="
done
rl78_run "CC-RL: nested/direct/byte/long skip and poke snapshot" 0 "総数 5, OK 5, NG 0" -- --abs "$TP" --cfg "$R/cfg/skip_ccrl.cfg" "$R/csv/skip_ccrl.csv"
rl78_run "CA78K0R: byte/word/long skip" 0 "総数 3, OK 3, NG 0" -- --abs "$WORK/test.lmf" --cfg "$R/cfg/skip_ca.cfg" "$R/csv/skip_ca.csv"
for setting in 'skip = missing_function' 'skip = read_io:garbage' 'skip = read_io:0x10000' 'skip = f_ll:1' 'poke = bad:1' 'poke = 0x100000:1' 'poke = 0xFFFA2:bad' 'poke = 0xFFFA2:256' 'poke = 0xFFFFF:1:2' 'poke = 0xFFFA2:1:0' 'poke = 0xFFFA2:1:5' 'poke = 0xFFFA2:1:1:2'; do
    printf '%s\n' "$setting" > "$WORK/bad_startup.cfg"
    "$RS" info --abs "$TP" --cfg "$WORK/bad_startup.cfg" > "$WORK/trace.log" 2>&1
    RC=$?
    check "設定エラー2: $setting" test "$RC" -eq 2
done
printf 'stub = read_io:AMSTB_read_io\nskip = read_io\n' > "$WORK/bad_startup.cfg"
"$RS" info --abs "$TP" --cfg "$WORK/bad_startup.cfg" > "$WORK/trace.log" 2>&1
RC=$?
check "stub と skip の重複はエラー2" test "$RC" -eq 2
# 診断のフックが内蔵SP/PSW/MDU動作を壊さないことは通常版と同じCSVで確かめる。
rl78_run "診断モードでも既存CC-RL ABI/MDUの43行が同じ" 0 "総数 43, OK 43, NG 0" -- --abs "$TP" --trace-startup "$R/csv/synth_call.csv" "$R/csv/synth_types.csv"

echo "== 性能の実測（本物のバックエンド）"
N=${RL78_PERF_ROWS:-20000}
awk -v n="$N" 'BEGIN { printf "mod,TEST_cycle,性能,0,1\n#COMMENT,TEST_cycle_cnt\n"; for (i = 1; i <= n / 10; i++) printf ",%d\n", i }' > "$WORK/perf_cycle.csv"
awk -v n="$N" 'BEGIN { printf "mod,f_neg,性能,0,1\n#COMMENT,f_neg@@\n"; for (i = 1; i <= n; i++) printf ",-3\n" }' > "$WORK/perf_neg.csv"
awk -v n="$N" 'BEGIN { printf "mod,get_fclk_freq_hz,性能,2,1\n#COMMENT,0xFFFA4,0xF00A8,get_fclk_freq_hz@@\n"; for (i = 1; i <= n; i++) printf ",0,%d,%d\n", i % 2, (i % 2) ? 16000000 : 32000000 }' > "$WORK/perf_g23.csv"
perf() {
    _label=$1; shift
    _t0=$(date +%s)
    "$RS" run "$@" -o "$WORK/r_perf" --quiet > "$WORK/perf.log" 2>&1
    _rc=$?
    _line=$(grep "^実行命令数" "$WORK/perf.log")
    _sec=$(grep "^実行時間" "$WORK/perf.log")
    if [ "$_rc" -eq 0 ] && [ -n "$_line" ]; then ok "性能: ${_label}（すべて OK）"; else ng "性能: $_label" "$(tail -3 "$WORK/perf.log")"; fi
    echo "        $_line"
    echo "        $_sec"
    PERF_SUMMARY="${PERF_SUMMARY:-}性能: $_label: $_line
"
}
perf "TEST_cycle（10us インナーループ、1 回 約 1.1 万命令）を $((N / 10)) 回" --abs "$TP" "$WORK/perf_cycle.csv"
perf "f_neg（2 命令）を $N 回（1 呼び出しあたりのオーバーヘッド）" --abs "$TP" "$WORK/perf_neg.csv"
OVH=$(awk '/^実行時間/ { gsub(/[^0-9.]/, " "); split($0, a, " "); print a[2] }' "$WORK/perf.log")
[ -n "$OVH" ] && echo "        1 呼び出しあたり: $(awk -v s="$OVH" -v n="$N" 'BEGIN { printf "%.2f us", s / n * 1e6 }')（対象関数の呼び出しにかかった時間 / 回数。CSV の処理は含まない）"
perf "G23 の get_fclk_freq_hz を $N 回" --abs "$G23" "$WORK/perf_g23.csv"

# Both execution paths must reach main with checksummed breakpoint bytes.
check "mkabs checksum" "$WORK/mkabs" checksum "$WORK/checksum.abs"
check "mklmf checksum" "$WORK/mklmf" checksum "$WORK/checksum.lmf"
for image in ccrl ca; do
    if [ "$image" = ccrl ]; then rom_image="$WORK/checksum.abs"; else rom_image="$WORK/checksum.lmf"; fi
    for mode in plain trace; do
        trace_opt=""
        if [ "$mode" = trace ]; then trace_opt="--trace-startup"; fi
        "$RS" info --abs "$rom_image" --cfg "$R/cfg/rom_check_$image.cfg" $trace_opt > "$WORK/rom_$image.$mode.log" 2>&1
        RC=$?
        check "ROM checksum $image $mode reaches main" test "$RC" -eq 0
        check "ROM checksum $image $mode init_end" contains "$WORK/rom_$image.$mode.log" "初期設定終了: main"
        grep "初期設定終了:" "$WORK/rom_$image.$mode.log" > "$WORK/count_$image.$mode"
        cat "$R/cfg/rom_check_$image.cfg" > "$WORK/corrupt.cfg"
        if [ "$image" = ccrl ]; then echo "poke = 0x13D:0"; else echo "poke = 0x10E:0"; fi >> "$WORK/corrupt.cfg"
        "$RS" info --abs "$rom_image" --cfg "$WORK/corrupt.cfg" $trace_opt > "$WORK/reset_$image.$mode.log" 2>&1
        RC=$?
        check "ROM checksum $image $mode rejects corruption" test "$RC" -eq 2
        check "ROM checksum $image $mode FFH diagnostic" contains "$WORK/reset_$image.$mode.log" "命令コード FFH を実行しました"
        check "ROM checksum $image $mode original bytes" contains "$WORK/reset_$image.$mode.log" "original bytes=FF"
        check "ROM checksum $image $mode genuine FF has no breakpoint" contains "$WORK/reset_$image.$mode.log" "breakpoint=none"
        if [ "$mode" = trace ]; then
            check "ROM checksum $image trace branch history" contains "$WORK/reset_$image.$mode.log" "FFH 経路:"
            check "ROM checksum $image trace memory observations" contains "$WORK/reset_$image.$mode.log" "last read:"
        fi

    done
    check "ROM checksum $image equal instruction and clock counts" cmp -s "$WORK/count_$image.plain" "$WORK/count_$image.trace"
done

# PSW slow-path immediately before init_end must not resume past the trap.
check "mklmf diagnostics" "$WORK/mklmf" diagnostics "$WORK/diag.lmf"
"$RS" info --abs "$WORK/diag.lmf" > "$WORK/diag.log" 2>&1
RC=$?
check "LMF diagnostic symbols load" test "$RC" -eq 0
check "LMF summary groups repeated combinations" contains "$WORK/diag.log" "0x55 0xDEAD 0x0000 normal 2"
check "LMF summary distinguishes absolute section" contains "$WORK/diag.log" "0x55 0xDEAD 0x0000 absolute 1"
check "LMF summary hides names" sh -c '! grep -F -q diag_unknown_a "$1"' sh "$WORK/diag.log"
"$RS" info --abs "$WORK/diag.lmf" -v -v > "$WORK/diag_names.log" 2>&1
check "LMF double verbose reveals examples" contains "$WORK/diag_names.log" "diag_unknown_a"
printf 'mod,cycle,t,3,3\n#COMMENT,abs_reg:1,diag_unknown_a:2,asm_data:2,abs_reg:1,diag_unknown_a:2,asm_data:2\n,0xAB,0x1234,0x5678,0xAB,0x1234,0x5678\n' > "$WORK/diag.csv"
rl78_run "LMF absolute and unknown names remain writable" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/diag.lmf" "$WORK/diag.csv"

check "mkabs boundary" "$WORK/mkabs" boundary "$WORK/boundary.abs"
printf 'init_end = boundary_end\nstub = boundary_end:boundary_short\nskip = boundary_short\n' > "$WORK/boundary.cfg"
for mode in plain trace; do
    trace_opt=""
    [ "$mode" = trace ] && trace_opt="--trace-startup"
    "$RS" info --abs "$WORK/boundary.abs" --cfg "$WORK/boundary.cfg" $trace_opt > "$WORK/boundary.$mode.log" 2>&1
    RC=$?
    check "boundary $mode stops at init_end FFH" test "$RC" -eq 0
    if [ "$mode" = trace ]; then
        check "trace compares both startup paths" contains "$WORK/boundary.$mode.log" "startup compare: MATCH"
        check "trace identifies normal result" contains "$WORK/boundary.$mode.log" "normal: OK"
        check "trace identifies diagnostic result" contains "$WORK/boundary.$mode.log" "diagnostic: OK"
    fi
    printf 'mod,boundary_end,overlap,0,0\n#COMMENT\nstep 1\nstep 2\nmod,boundary_end,reset,0,0\n#COMMENT\nstep 1\n' > "$WORK/boundary.csv"
    rl78_run "boundary $mode stub skip overlap and snapshot reset" 0 "総数 3, OK 0, NG 0, NO Check 3" -- --abs "$WORK/boundary.abs" --cfg "$WORK/boundary.cfg" $trace_opt "$WORK/boundary.csv"
done

rl78_run "header names and modi initializers real" 0 "総数 2, OK 2, NG 0" -- --abs "$WORK/test.lmf" --cfg "$T/defines/real.cfg" "$T/defines/real.csv"
"$RS" info --abs "$WORK/test.lmf" --cfg "$T/defines/real.cfg" > "$WORK/header_info.log" 2>&1
RC=$?
check "header definitions info" test "$RC" -eq 0
check "recursive header file count" contains "$WORK/header_info.log" "defines files=4"
"$RS" list --abs "$WORK/test.lmf" --cfg "$T/defines/real.cfg" > "$WORK/header_list.log" 2>&1
check "header names in list real" contains "$WORK/header_list.log" "df_unit"
check "macros in C files" contains "$WORK/header_list.log" "MORE"

rl78_run "var and varfile real" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/test.lmf" --cfg "$T/defines/var_real.cfg" "$T/defines/var_real.csv"

rl78_run "header arrays nested union and mixed bit storage real" 0 "総数 1, OK 1, NG 0" -- --abs "$WORK/test.lmf" --cfg "$T/defines/real.cfg" "$T/defines/types_real.csv"

echo "== defines: コンパイルスイッチ（本物）"
cond_suite "cond real" "$RS" FF9 "$T/defines/cond_real.csv" "--abs $WORK/test.lmf"
# コンパイラが暗黙に定義するマクロ: lmf なら CA78K0R、ELF なら CC-RL。cundef で外せる
cond_cfg
cond_list -v
check "cond real: LMF defines the CA78K0R implicit macros" sh -c 'grep -q -E "^var  0xFF97E COND_K0R +u8 " "$1" && grep -q -F "implicit: __K0R__=1, __CA78K0R__=1" "$2"' sh "$WORK/cond_list.log" "$WORK/cond_info.log"
check "cond real: implicit macro is not reported as undefined" sh -c '! grep -q "^  __K0R__ (" "$1"' sh "$WORK/cond_info.log"
cond_cfg 'cundef = __K0R__' 'char_unsigned = on'
cond_list -v
check "cond real: cundef removes an implicit macro" cond_at 7F COND_K0R u8
check "cond real: char_unsigned = on adds __CHAR_UNSIGNED__" contains "$WORK/cond_info.log" "implicit: __CA78K0R__=1, __CHAR_UNSIGNED__=1"
COND_ARGS="--abs $TP"
cond_cfg
cond_list -v
check "cond real: ELF defines the CC-RL implicit macros" sh -c 'grep -q -E "^var  0xFF97F COND_K0R +u8 " "$1" && grep -q -F "implicit: __CCRL__=1, __CCRL=1, __RL78__=1, __RENESAS__=1" "$2"' sh "$WORK/cond_list.log" "$WORK/cond_info.log"
# probe の lmf と、その CS+ プロジェクトファイル（定義マクロは空）の組み合わせ
"$RS" info --abs "$PROBE" --mtpj "$T/ca78k0r_probe/ca78k0r_probe.mtpj" --defines "$COND" --cdef PRODUCT=2 > "$WORK/cond_probe.log" 2>&1
check "cond real: probe lmf with its own project file" sh -c '[ "$1" -eq 0 ] && grep -q -F "mtpj macro definitions (0): なし" "$2" && grep -q "cdef implicit=2 mtpj=0 cfg=0 cli=1 cundef=0" "$2"' sh "$?" "$WORK/cond_probe.log"

# Socket-dependent integration. Keep explicit failure rather than reporting
# zero checks as success when this sandbox cannot listen.
PORT=31329
U="http://127.0.0.1:$PORT"
printf 'defines = %s/defines/types\ndefines = %s/defines/device/regs.h\nvar = NAMED_P1 : u8 @ 0xFFF01\n' "$T" "$T" >> "$WORK/boundary.cfg"
"$RS" gui --abs "$WORK/boundary.abs" --cfg "$WORK/boundary.cfg" --func boundary_end --no-browser --port "$PORT" > "$WORK/boundary_gui.log" 2>&1 &
GUI_PID=$!
i=0
while [ $i -lt 50 ]; do
    curl -s "$U/api/info" > "$WORK/boundary_info.json" 2>/dev/null && break
    kill -0 "$GUI_PID" 2>/dev/null || break
    i=$((i+1)); sleep 0.1
done
if curl -s "$U/api/info" > "$WORK/boundary_info.json" 2>/dev/null; then
    ok "boundary GUI listens"
    for body in '{"cmd":"step"}' '{"cmd":"reset"}' '{"func":"boundary_short","cmd":"step"}' '{"func":"boundary_end","cmd":"step"}'; do
        CODE=$(curl -s -o "$WORK/boundary_ctl.json" -w '%{http_code}' -X POST -d "$body" "$U/api/control")
        check "GUI breakpoint roles after reset or function change" test "$CODE" = 200
        check "GUI breakpoint role call has no error" contains "$WORK/boundary_ctl.json" '"error":""'
    done
    curl -s "$U/api/symbols?q=df_unit" > "$WORK/boundary_search.json"
    check "GUI searches header definition" contains "$WORK/boundary_search.json" "df_unit"
    CODE=$(curl -s -o "$WORK/boundary_watch.json" -w '%{http_code}' -X POST -d '{"names":["df_unit","NAMED_P1"]}' "$U/api/watch")
    check "GUI watches header and named address" test "$CODE" = 200
    CODE=$(curl -s -o "$WORK/boundary_write.json" -w '%{http_code}' -X POST -d '{"name":"NAMED_P1","value":"0xAB"}' "$U/api/write")
    check "GUI writes named address" test "$CODE" = 200
    printf 'modi,boundary_end,t,1,1\n#InitWheneverCall,df_unit\n0,5\n#COMMENT,Fs_ckc_css,df_unit\n,1,5\n,0,5\n' > "$WORK/boundary_play.csv"
    CODE=$(curl -s -o "$WORK/boundary_load.json" -w '%{http_code}' -X POST --data-binary "@$WORK/boundary_play.csv" "$U/api/load-csv")
    check "GUI modi playback load" test "$CODE" = 200
    sleep 0.1
    curl -s "$U/api/export.csv" > "$WORK/boundary_export.csv"
    rl78_run "GUI modi recording reruns with initialization" 0 "NG 0" -- --abs "$WORK/boundary.abs" --cfg "$WORK/boundary.cfg" "$WORK/boundary_export.csv"
else
    ng "GUI boundary and header integration unavailable" "$(head -2 "$WORK/boundary_gui.log")"
fi
kill "$GUI_PID" 2>/dev/null; wait "$GUI_PID" 2>/dev/null
GUI_PID=""

echo "== C への変換方式（exec = xlate / verify）"
# 変換したコードは clang で作る。なければこの節は失敗（既定の解釈実行のテストは上で済んでいる）
XLCC=${SIM_XLATE_CC:-clang}
if ! command -v "$XLCC" >/dev/null 2>&1; then
    ng "変換方式: clang がある" "$XLCC が見つかりません（SIM_XLATE_CC で指定できます）"
else
    XW="$WORK/xl"
    mkdir -p "$XW"
    cp "$TP" "$XW/tp.abs"
    XCSVS="synth_call synth_core synth_fault synth_halt synth_heap_odd synth_nomdu synth_nostub synth_stub synth_stub_line synth_types"
    # 解釈実行の結果（終了コードと結果 CSV）を控える
    for n in $XCSVS; do
        "$RS" run --abs "$XW/tp.abs" "$R/csv/$n.csv" -o "$XW/out_i" > "$XW/i_$n.log" 2>&1
        echo $? > "$XW/i_$n.rc"
    done
    # xl_same 名前 方式: すべての CSV で、終了コードと結果 CSV が解釈実行と同じ
    xl_same() {
        _bad=""
        for n in $XCSVS; do
            "$RS" run --exec "$2" --abs "$XW/tp.abs" "$R/csv/$n.csv" -o "$XW/out_x" > "$XW/x_$n.log" 2>&1
            _rc=$?
            [ "$_rc" = "$(cat "$XW/i_$n.rc")" ] || _bad="$_bad $n(終了コード $_rc)"
            if [ -f "$XW/out_i/$n.csv" ]; then cmp -s "$XW/out_i/$n.csv" "$XW/out_x/$n.csv" || _bad="$_bad $n(結果 CSV)"; fi
            if grep -q "照合で不一致" "$XW/x_$n.log"; then _bad="$_bad $n(照合で不一致)"; fi
        done
        if [ -z "$_bad" ]; then ok "$1"; else ng "$1" "$_bad"; fi
    }
    "$RS" run --exec xlate --abs "$XW/tp.abs" "$R/csv/synth_call.csv" -o "$XW/out_x" > "$XW/first.log" 2>&1
    check "変換方式: 初回は解釈実行で動かす" contains "$XW/first.log" "今回は解釈実行で動かし、終わってから変換します"
    check "変換方式: 初回の終わりに変換してコンパイルする" contains "$XW/first.log" "変換: できました"
    check "変換方式: 初回の結果 CSV は解釈実行と同じ" cmp -s "$XW/out_i/synth_call.csv" "$XW/out_x/synth_call.csv"
    "$RS" run --exec xlate --abs "$XW/tp.abs" "$R/csv/synth_call.csv" -o "$XW/out_x" > "$XW/second.log" 2>&1
    check "変換方式: 2 回目は変換済みの関数を使う" grep -q "実行方式: 変換（変換済みの関数 [0-9]* 個を使います）" "$XW/second.log"
    check "変換方式: 2 回目は変換したコードで実行する" grep -q "変換したコードで実行: [1-9][0-9]* 命令" "$XW/second.log"
    check "変換方式: 2 回目は作り直さない" sh -c "! grep -q '変換: 新しい関数' '$XW/second.log'"
    check "変換方式: 2 回目の結果 CSV は解釈実行と同じ" cmp -s "$XW/out_i/synth_call.csv" "$XW/out_x/synth_call.csv"
    xl_same "変換方式: どの CSV も終了コードと結果 CSV が解釈実行と同じ（通った関数だけ変換）" xlate
    xl_same "変換方式: 2 巡目（1 巡目で通った関数を変換済み）も同じ" xlate
    xl_same "照合（exec = verify）: 変換したコードと解釈実行が一致する" verify
    "$RS" xlate --abs "$XW/tp.abs" > "$XW/all.log" 2>&1
    check "sim xlate: すべての関数を変換する（終了コード 0）" test $? -eq 0
    check "sim xlate: 変換できた" contains "$XW/all.log" "変換: できました"
    "$RS" info --exec xlate --abs "$XW/tp.abs" > "$XW/info.log" 2>&1
    check "sim info: 実行方式と変換済みの関数の数を表示する" grep -q "変換済みの関数: [1-9][0-9]* 個、フォルダ: " "$XW/info.log"
    xl_same "sim xlate の後: すべての関数を変換した状態でも結果は同じ" xlate
    xl_same "sim xlate の後: 照合でも一致する" verify
    "$RS" run --exec xlate --abs "$XW/tp.abs" "$R/csv/synth_stub_line.csv" -o "$XW/out_x" > "$XW/stub.log" 2>&1
    check "変換方式: CSV の % 行の置換（ブレークポイント）でも変換したコードを使う" grep -q "変換したコードで実行: [1-9][0-9]* 命令" "$XW/stub.log"
    "$RS" xlate --abs "$XW/tp.abs" > "$XW/again.log" 2>&1
    check "sim xlate: 変換済みなら何もしない" sh -c "! grep -q '変換: 新しい関数' '$XW/again.log'"
    # sim.cfg の exec = と、起動できないコンパイラ
    mkdir -p "$XW/nocc"
    cp "$TP" "$XW/nocc/tp.abs"
    printf 'exec = xlate\nxlate_cc = %s/no-such-clang\n' "$XW" > "$XW/nocc/sim.cfg"
    "$RS" run --abs "$XW/nocc/tp.abs" "$R/csv/synth_call.csv" -o "$XW/out_n" > "$XW/nocc.log" 2>&1
    check "変換方式: clang を起動できなくても実行は成功する（終了コード 0）" test $? -eq 0
    check "変換方式: clang を起動できないことを知らせる" contains "$XW/nocc.log" "clang を起動できません"
    check "変換方式: clang を起動できないときの結果 CSV は解釈実行と同じ" cmp -s "$XW/out_i/synth_call.csv" "$XW/out_n/synth_call.csv"
    "$RS" run --exec fast --abs "$XW/tp.abs" "$R/csv/synth_call.csv" -o "$XW/out_x" > "$XW/badexec.log" 2>&1
    check "--exec の値が不正ならエラー（終了コード 2）" test $? -eq 2
    check "--exec の値が不正なら使える値を示す" contains "$XW/badexec.log" "--exec = fast は使えません"
    printf 'exec = xlate\nxlate_jobs = 0\n' > "$XW/nocc/sim.cfg"
    "$RS" info --abs "$XW/nocc/tp.abs" > "$XW/badjobs.log" 2>&1
    check "xlate_jobs の値が不正ならエラー" contains "$XW/badjobs.log" "xlate_jobs = 0 を読めません"
    # デバッグ情報だけが違うロードモジュール（コードは同じ）なら、変換結果を使い回す
    "$WORK/mkabs" dwarf3 "$XW/tp.abs" > /dev/null 2>&1
    "$RS" run --exec xlate --abs "$XW/tp.abs" "$R/csv/synth_call.csv" -o "$XW/out_x" > "$XW/samecode.log" 2>&1
    check "変換方式: コードが同じなら変換結果を使い回す" grep -q "実行方式: 変換（変換済みの関数 [0-9]* 個を使います）" "$XW/samecode.log"
    # 変換の前提（対象のメモリの範囲と中身、CPU の設定）が変われば別の変換結果になる。古いフォルダは消す
    printf 'rom_size = 0x10000\n' > "$XW/sim.cfg"
    "$RS" run --exec xlate --abs "$XW/tp.abs" "$R/csv/synth_types.csv" -o "$XW/out_x" > "$XW/changed.log" 2>&1
    check "変換方式: 変換の前提（rom_size）が変わったら変換し直す" contains "$XW/changed.log" "今回は解釈実行で動かし、終わってから変換します"
    check "変換方式: 古い変換結果のフォルダを消す" test "$(ls "$XW/.simxl" | wc -l | tr -d ' ')" = 1
fi
