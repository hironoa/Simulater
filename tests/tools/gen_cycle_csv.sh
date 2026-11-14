#!/bin/sh
# tests/csv/cycle_2000.csv を作り直す（通常は使わない。コミット済みのファイルを使う）
# 入力は値が変わったときだけ書き、変わらない行は空セル（前の値を保つ）にする。
# g_ms_tick、TEST_time_us、TEST_cycle_cnt、P1_bit.no7 はドライバの作りから決まる値を期待値に入れる。
awk 'BEGIN {
  N = 2100
  printf "mod,TEST_cycle,周期実行 %d 行（1 行 = 10ms）,3,8\r\n", N
  printf "#COMMENT,AMSTB_adc_read@AMIN_adc[0],AMSTB_adc_read@AMIN_adc[1],P2_bit.no0,g_eng.state,g_eng.gear,g_eng.flag.fan_req,P1_bit.no3,g_ms_tick,TEST_time_us,TEST_cycle_cnt,P1_bit.no7\r\n"
  printf ";,イグニッション ON、クランキング、アイドル、加速、減速、巡航、OFF、再始動\r\n"
  prpm = -1; ptemp = -1; pign = -1
  for (r = 1; r <= N; r++) {
    ign = (r < 5 || (r > 2000 && r <= 2050)) ? 0 : 1
    if (r < 5) rpm = 0
    else if (r <= 20) rpm = 300
    else if (r <= 100) rpm = 900
    else if (r <= 1100) rpm = 900 + (r - 100) * 5
    else if (r <= 1600) rpm = 5900 - (r - 1100) * 9
    else if (r <= 2000) rpm = 2000
    else if (r <= 2050) rpm = 0
    else if (r <= 2060) rpm = 300
    else rpm = 900
    if (r <= 1900) temp = int(20 + r * 0.05)
    else temp = int(115 - (r - 1900) * 0.1)
    raw = temp + 40
    c1 = (rpm != prpm) ? rpm : ""
    c2 = (raw != ptemp) ? raw : ""
    c3 = (ign != pign) ? ign : ""
    prpm = rpm; ptemp = raw; pign = ign
    if (r % 500 == 0) printf ";,%d 行目\r\n", r
    printf ",%s,%s,%s,,,,,%d,%d,%d,%d\r\n", c1, c2, c3, r * 10, r * 10000, r % 65536, r % 2
  }
}' > "$(dirname "$0")/../csv/cycle_2000.csv"
