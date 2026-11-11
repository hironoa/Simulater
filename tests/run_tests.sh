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
    if [ "$_rc" -eq 2 ] && cmp -s "$WORK/same/unit_all.csv" "$T/csv/unit_all.csv" && contains "$WORK/same.log" "入力と同じです"; then ok "$_name"
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

echo "== 配列・ポインタの長い値（4096 要素を超える）"
"$SIM" run "$T/csv/long_array.csv" -o "$WORK/olong" --quiet > "$WORK/long.log" 2>&1
RC=$?
check "4097 要素目の不一致を NG にする（終了コード 1）" test "$RC" -eq 1
check "4096 要素を超える期待値と入力を全部比べる（OK 2, NG 1）" contains "$WORK/long.log" "合計: 総数 3, OK 2, NG 1, NO Check 0"

echo "== GUI サーバの API"
if ! command -v curl > /dev/null 2>&1; then
    ng "curl がありません"
else
    PORT=$((20000 + $$ % 20000))
    mkdir -p "$WORK/nohtml"
    "$SIM" gui --no-browser --port "$PORT" --period-ms 2 --html-dir "$WORK/nohtml" > "$WORK/gui.log" 2>&1 &
    GUI_PID=$!
    U="http://127.0.0.1:$PORT"
    i=0
    while [ $i -lt 50 ]; do
        curl -s "$U/api/info" > "$WORK/info.json" 2>/dev/null && break
        i=$((i + 1)); sleep 0.1
    done
    post() { curl -s -o "$WORK/$2" -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data-binary "@$T/gui/$1" "$U$3"; }

    check "/api/info が応答" contains "$WORK/info.json" '"version":"0.1.0"'
    check "/api/info に functions と既定の関数" contains "$WORK/info.json" '"func":"TEST_cycle","functions":["eng_judge_gear"'
    check "/api/info に state" contains "$WORK/info.json" '"state":{"running":false,"step":0'

    curl -s "$U/api/symbols?q=flag.fan&limit=10" > "$WORK/sym.json"
    check "/api/symbols でメンバまで検索" contains "$WORK/sym.json" '{"name":"g_eng.flag.fan_req","type":"uint8","size":1,"kind":"uint","writable":true,"members":[]}'
    curl -s "$U/api/symbols?q=g_eng&limit=1" > "$WORK/sym2.json"
    check "/api/symbols の構造体と members" contains "$WORK/sym2.json" '[{"name":"g_eng","type":"eng_status_t","size":8,"kind":"struct","writable":false,"members":["g_eng.rpm","g_eng.water_temp","g_eng.state","g_eng.gear","g_eng.flag"]}]'
    curl -s "$U/api/symbols?q=%40&limit=5" > "$WORK/sym3.json"
    check "/api/symbols に関数内 static（func@name）" contains "$WORK/sym3.json" '"name":"eng_update@call_cnt"'

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
    check "state に holds" contains "$WORK/step.out" '"holds":[{"name":"AMSTB_adc_read@AMIN_adc[0]","value":800},{"name":"P2_bit.no0","value":1}]'

    curl -s "$U/api/export.csv" > "$WORK/export.csv"
    check "/api/export.csv の mod 行" grep -q "^mod,TEST_cycle,GUI記録,3,4" "$WORK/export.csv"
    check "/api/export.csv の #COMMENT 行" grep -q "^#COMMENT,AMSTB_adc_read@AMIN_adc\[0\],AMSTB_adc_read@AMIN_adc\[1\],P2_bit.no0,g_eng.state,TEST_cycle_cnt,g_ms_tick,AMSTB_adc_read@AMIN_adc\[0\]" "$WORK/export.csv"
    # 1 ステップ目だけ 1 回書き込みの AMIN_adc[1] が入る。状態は STOP->CRANK->IDLE
    tr -d '\r' < "$WORK/export.csv" | sed -n '3,7p' > "$WORK/export_rows.txt"
    printf ',800,120,1,1,1,10,800\n,800,,1,2,2,20,800\n,800,,1,2,3,30,800\n,800,,1,2,4,40,800\n,800,,1,2,5,50,800\n' > "$WORK/export_expect.txt"
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
    PORT_USED_LOG="$WORK/gui2.log"
    "$SIM" gui --no-browser --port "$PORT" > "$PORT_USED_LOG" 2>&1
    RC=$?
    check "使用中のポートは分かるエラー（終了コード 2）" test "$RC" -eq 2
    check "使用中のポートのメッセージ" contains "$PORT_USED_LOG" "で待ち受けられません"
    kill "$GUI_PID" 2>/dev/null
    wait "$GUI_PID" 2>/dev/null
    GUI_PID=""
fi

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
