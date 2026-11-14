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
mkdir -p "$WORK/autocfg"
cp "$TP" "$WORK/autocfg/prog.abs"
cp "$R/cfg/stub.cfg" "$WORK/autocfg/sim.cfg"
rl78_run ".abs と同じフォルダの sim.cfg を自動で読む" 0 "合計: 総数 3, OK 3, NG 0" -- --abs "$WORK/autocfg/prog.abs" "$R/csv/synth_stub.csv"
printf 'abs = prog.abs\n' > "$WORK/autocfg/only_abs.cfg"
rl78_run "sim.cfg の abs = で .abs を指定（--abs なし）" 0 "合計: 総数 1, OK 1, NG 0" -- --cfg "$WORK/autocfg/only_abs.cfg" "$R/csv/synth_nostub.csv"

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
