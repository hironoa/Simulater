@echo off
rem tests\run_tests.bat - automated tests on Windows
rem   first half: mock backend (common part), second half: real RL78 backend (tests\rl78\run_rl78_tests.bat)
rem
rem   tests\run_tests.bat              build with build.bat mock / build.bat, then test
rem   set SIM=path & tests\run_tests.bat        use an existing mock sim.exe
rem   set SIM_RL78=path & tests\run_tests.bat   use an existing real sim.exe
rem
rem Prints the number of checks run and failed at the end; exits 1 when any check failed.
rem Messages are ASCII only so that cmd.exe parses this file the same on any code page.
rem Japanese error messages are checked through their ASCII parts.
setlocal EnableExtensions

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "T=%ROOT%\tests"
set "WORK=%TEMP%\simtest_%RANDOM%%RANDOM%"
mkdir "%WORK%"
set PASS=0
set FAIL=0

echo == build
if not defined SIM (
    call "%ROOT%\build.bat" mock "%WORK%\build" > "%WORK%\build.log" 2>&1
    if errorlevel 1 (
        call :ng "build.bat mock"
        type "%WORK%\build.log"
    ) else (
        call :ok "build.bat mock"
    )
    findstr /i /c:"warning" "%WORK%\build.log" >nul
    if errorlevel 1 (call :ok "build without warnings") else (call :ng "build without warnings")
    set "SIM=%WORK%\build\sim.exe"
)
if not exist "%SIM%" (
    echo sim.exe not found: %SIM%
    set /a FAIL+=1
    goto summary
)

echo == run test CSVs and compare results byte by byte
call :run_cmp unit_all.csv 0
call :run_cmp ng_case.csv 1
call :run_cmp sjis_comment.csv 0
call :run_cmp cycle_2000.csv 0

echo == 2000-row periodic run
"%SIM%" run "%T%\csv\cycle_2000.csv" -o "%WORK%\o_cycle" --quiet > "%WORK%\cycle.log" 2>&1
findstr /l /c:"OK 2100, NG 0" "%WORK%\cycle.log" >nul
if errorlevel 1 (call :ng "cycle_2000: all 2100 rows OK") else (call :ok "cycle_2000: all 2100 rows OK")
findstr /c:"ms" "%WORK%\o_cycle\cycle_2000.csv" >nul
if errorlevel 1 (call :ng "time column present") else (call :ok "time column present")

echo == read result CSVs back
"%SIM%" run "%WORK%\out_unit_all\unit_all.csv" -o "%WORK%\rb1" --fixed-time --quiet > "%WORK%\rb.log" 2>&1
fc /b "%WORK%\rb1\unit_all.csv" "%T%\expected\unit_all.csv" >nul
if errorlevel 1 (call :ng "unit_all result read back gives the same result") else (call :ok "unit_all result read back gives the same result")
"%SIM%" run "%WORK%\out_ng_case\ng_case.csv" -o "%WORK%\rb2" --fixed-time --quiet > "%WORK%\rb.log" 2>&1
if errorlevel 2 (call :ng "ng_case read back exits 1") else if errorlevel 1 (call :ok "ng_case read back exits 1") else (call :ng "ng_case read back exits 1")
"%SIM%" run "%WORK%\out_ng_case\ng_case.csv" -o "%WORK%\rb3" --fixed-time --expect-actual --quiet > "%WORK%\rb.log" 2>&1
if errorlevel 1 (call :ng "--expect-actual exits 0") else (call :ok "--expect-actual exits 0")

echo == Shift_JIS and CRLF are kept as bytes
findstr /b /c:";" "%T%\csv\sjis_comment.csv" > "%WORK%\sj_in.txt"
findstr /b /c:";" "%WORK%\out_sjis_comment\sjis_comment.csv" > "%WORK%\sj_out.txt"
fc /b "%WORK%\sj_in.txt" "%WORK%\sj_out.txt" >nul
if errorlevel 1 (call :ng "Shift_JIS comment lines unchanged") else (call :ok "Shift_JIS comment lines unchanged")

echo == error cases (exit code 2 and a clear message)
call :err_case unknown_var.csv "g_engx"
call :err_case unknown_func.csv "no_such_func"
call :err_case unknown_arg.csv "@rpm, @temp, @cur_gear"
call :err_case float_arg.csv "set_duty"
call :err_case bad_value.csv "12abc"
call :err_case struct_no_member.csv "g_eng."
call :err_case ambiguous_static.csv "engine.c/s_cnt sensor.c/s_cnt"
call :err_case write_const.csv "g_lut[0]"
call :err_case index_range.csv "g_gear_up_rpm[5]"
call :err_case overflow.csv "300"
call :err_case null_deref.csv "NULL"
call :err_case bad_mod.csv "'x'"

echo == never overwrite an input, whatever the path spelling
mkdir "%WORK%\same"
mkdir "%WORK%\other"
call :same_case "input given with .\ in the middle" "%WORK%\same\.\unit_all.csv" -o "%WORK%\same"
call :same_case "relative input, absolute output" unit_all.csv -o "%WORK%\same"
call :same_case "output given as ." "%WORK%\same\unit_all.csv" -o .
call :same_case "output given with sub\.." "%WORK%\same\unit_all.csv" -o "%WORK%\same\sub\.."
call :same_case "output given with other letter case and slashes" "%WORK%\same\unit_all.csv" -o "%WORK%/SAME"
call :same_case "a later input is not overwritten by an earlier result" "%T%\csv\unit_all.csv" "%WORK%\same\unit_all.csv" -o "%WORK%\same"
"%SIM%" run "%WORK%\same\.\unit_all.csv" -o "%WORK%\fresh\." --fixed-time --quiet > "%WORK%\fresh.log" 2>&1
if errorlevel 1 (call :ng "output file not there yet is written") else if exist "%WORK%\fresh\unit_all.csv" (call :ok "output file not there yet is written") else (call :ng "output file not there yet is written")
copy /y "%T%\csv\unit_all.csv" "%WORK%\other\unit_all.csv" >nul
"%SIM%" run "%T%\csv\unit_all.csv" -o "%WORK%\other" --fixed-time --quiet > "%WORK%\other.log" 2>&1
if errorlevel 1 (call :ng "another file with the same content is overwritten") else (
    fc /b "%WORK%\other\unit_all.csv" "%T%\expected\unit_all.csv" >nul
    if errorlevel 1 (call :ng "another file with the same content is overwritten") else (call :ok "another file with the same content is overwritten")
)

echo == arrays and pointers with more than 4096 elements
"%SIM%" run "%T%\csv\long_array.csv" -o "%WORK%\olong" --quiet > "%WORK%\long.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="1" (call :ok "mismatch at element 4097 is NG (exit code 1)") else (call :ng "mismatch at element 4097 is NG (exit code 1), got %RC%")
findstr /l /c:"OK 2, NG 1, NO Check 0" "%WORK%\long.log" >nul
if errorlevel 1 (call :ng "all elements beyond 4096 are compared (OK 2, NG 1)") else (call :ok "all elements beyond 4096 are compared (OK 2, NG 1)")

echo == expected values are read with the input rules; values that do not fit are NG
call :run_cmp expect_range.csv 1
"%SIM%" run "%T%\csv\expect_range.csv" -o "%WORK%\orange" --quiet > "%WORK%\range.log" 2>&1
findstr /l /c:"OK 15" "%WORK%\range.log" >nul
if errorlevel 1 (call :ng "out-of-range expected values are NG, not errors (OK 15)") else (call :ok "out-of-range expected values are NG, not errors (OK 15)")
findstr /l /c:"NG 16" "%WORK%\range.log" >nul
if errorlevel 1 (call :ng "out-of-range expected values are NG, not errors (NG 16)") else (call :ok "out-of-range expected values are NG, not errors (NG 16)")

echo == empty string and empty cell
call :run_cmp empty_string.csv 1
"%SIM%" run "%T%\csv\empty_string.csv" -o "%WORK%\oempty" --quiet > "%WORK%\empty.log" 2>&1
findstr /l /c:"OK 6" "%WORK%\empty.log" >nul
if errorlevel 1 (call :ng "an empty string writes the terminator and is compared as a string (OK 6)") else (call :ok "an empty string writes the terminator and is compared as a string (OK 6)")

echo == GUI server API
where curl >nul 2>nul
if errorlevel 1 (
    call :ng "curl.exe not found"
    goto after_gui
)
set /a PORT=20000 + %RANDOM% %% 20000
set "U=http://127.0.0.1:%PORT%"
mkdir "%WORK%\gui"
copy /y "%SIM%" "%WORK%\gui\sim_guitest.exe" >nul
start "" /b "%WORK%\gui\sim_guitest.exe" gui --no-browser --port %PORT% --period-ms 2 > "%WORK%\gui.log" 2>&1
set TRIES=0
:wait_up
curl -s "%U%/api/info" > "%WORK%\info.json" 2>nul
if not errorlevel 1 goto up
set /a TRIES+=1
if %TRIES% geq 20 goto up
ping -n 2 127.0.0.1 >nul
goto wait_up
:up
findstr /l /c:"\"func\":\"TEST_cycle\",\"functions\":[" "%WORK%\info.json" >nul
if errorlevel 1 (call :ng "/api/info") else (call :ok "/api/info")
curl -s "%U%/api/symbols?q=flag.fan&limit=10" > "%WORK%\sym.json"
findstr /l /c:"\"name\":\"g_eng.flag.fan_req\"" "%WORK%\sym.json" >nul
if errorlevel 1 (call :ng "/api/symbols") else (call :ok "/api/symbols")
curl -s -X POST --data-binary "@%T%\gui\watch.json" "%U%/api/watch" > "%WORK%\watch.out"
findstr /l /c:"\"name\":\"nope_var\"" "%WORK%\watch.out" >nul
if errorlevel 1 (call :ng "/api/watch errors") else (call :ok "/api/watch errors")
curl -s -X POST --data-binary "@%T%\gui\write_rpm.json" "%U%/api/write" > "%WORK%\w1.out"
findstr /l /c:"\"ok\":true" "%WORK%\w1.out" >nul
if errorlevel 1 (call :ng "/api/write") else (call :ok "/api/write")
curl -s -X POST --data-binary "@%T%\gui\write_temp.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step5.json" "%U%/api/control" > "%WORK%\step.out"
findstr /l /c:"\"step\":5" "%WORK%\step.out" >nul
if errorlevel 1 (call :ng "/api/control step") else (call :ok "/api/control step")
curl -s "%U%/api/export.csv" > "%WORK%\export.csv"
findstr /b /c:"mod,TEST_cycle," "%WORK%\export.csv" >nul
if errorlevel 1 (call :ng "/api/export.csv") else (call :ok "/api/export.csv")
findstr /b /l /c:",800,0x78,1,1,1,10,800" "%WORK%\export.csv" >nul
if errorlevel 1 (call :ng "/api/export.csv first row") else (call :ok "/api/export.csv first row")
start "" /b curl -s -N --max-time 3 "%U%/api/stream" > "%WORK%\sse.txt"
ping -n 2 127.0.0.1 >nul
curl -s -X POST --data-binary "@%T%\gui\run.json" "%U%/api/control" > nul
ping -n 3 127.0.0.1 >nul
curl -s -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > nul
ping -n 2 127.0.0.1 >nul
findstr /l /c:"event: hello" "%WORK%\sse.txt" >nul
if errorlevel 1 (call :ng "/api/stream hello") else (call :ok "/api/stream hello")
findstr /l /c:"event: samples" "%WORK%\sse.txt" >nul
if errorlevel 1 (call :ng "/api/stream samples") else (call :ok "/api/stream samples")
findstr /l /c:"event: state" "%WORK%\sse.txt" >nul
if errorlevel 1 (call :ng "/api/stream state") else (call :ok "/api/stream state")
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > "%WORK%\reset.out"
findstr /l /c:"\"step\":0" "%WORK%\reset.out" >nul
if errorlevel 1 (call :ng "/api/control reset") else (call :ok "/api/control reset")
curl -s -X POST --data-binary "@%T%\gui\play.csv" "%U%/api/load-csv" > "%WORK%\load.out"
findstr /l /c:"\"rows\":4" "%WORK%\load.out" >nul
if errorlevel 1 (call :ng "/api/load-csv") else (call :ok "/api/load-csv")
rem a failed function change keeps the previous function, arguments and settings
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\func_bad.json" "%U%/api/control" > "%WORK%\fb.out"
findstr /l /c:"\"error\":" "%WORK%\fb.out" >nul
if errorlevel 1 (call :ng "change to a function with unsupported arguments is rejected") else (call :ok "change to a function with unsupported arguments is rejected")
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\fb_export.csv"
rem the last row ends with eng_update@call_cnt, which must be 2 after two steps
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\fb_export.csv") do set "LAST=%%L"
if "%LAST:~-2%"==",2" (call :ok "after a rejected change the previous function is still called") else (call :ng "after a rejected change the previous function is still called")
curl -s -X POST --data-binary "@%T%\gui\func_bad_speed.json" "%U%/api/control" > nul
curl -s "%U%/api/info" > "%WORK%\fbs_info.json"
findstr /l /c:"\"speed\":7" /c:"\"periodMs\":9" "%WORK%\fbs_info.json" >nul
if errorlevel 1 (call :ok "a rejected change keeps speed and periodMs") else (call :ng "a rejected change keeps speed and periodMs")
curl -s -X POST --data-binary "@%T%\gui\long_value.json" "%U%/api/write" > "%WORK%\longval.out"
findstr /l /c:"\"error\":" "%WORK%\longval.out" >nul
if errorlevel 1 (call :ng "/api/write does not truncate long values") else (call :ok "/api/write does not truncate long values")
curl -s -X POST --data-binary "@%T%\gui\func_bad_cmd.json" "%U%/api/control" > nul
curl -s "%U%/api/info" > "%WORK%\fbc_info.json"
findstr /l /c:"\"func\":\"TEST_cycle\"" "%WORK%\fbc_info.json" >nul
if errorlevel 1 (call :ng "an unknown cmd does not change the function") else (call :ok "an unknown cmd does not change the function")
curl -s -X POST --data-binary "@%T%\gui\bad_play.csv" "%U%/api/load-csv" > nul
curl -s "%U%/api/info" > "%WORK%\bp_info.json"
findstr /l /c:"\"func\":\"TEST_cycle\"" "%WORK%\bp_info.json" >nul
if errorlevel 1 (call :ng "a rejected /api/load-csv does not change the function") else (call :ok "a rejected /api/load-csv does not change the function")
rem string and array inputs ('XYZ', 65|66|67) stay in the saved CSV and the CSV re-runs with all rows OK
curl -s -X POST --data-binary "@%T%\gui\watch_name0.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_arr.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\str_export.csv"
findstr /l /c:"XYZ" "%WORK%\str_export.csv" >nul
if errorlevel 1 (call :ng "a string input is saved as written") else (call :ok "a string input is saved as written")
findstr /l /c:"65|66|67" "%WORK%\str_export.csv" >nul
if errorlevel 1 (call :ng "an array input is saved as written") else (call :ok "an array input is saved as written")
"%SIM%" run "%WORK%\str_export.csv" -o "%WORK%\ostr" --quiet > "%WORK%\str_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 2, NG 0, NO Check 0" "%WORK%\str_rerun.log" >nul
if errorlevel 1 (call :ng "the saved string/array inputs re-run with all rows OK") else if "%RC%"=="0" (call :ok "the saved string/array inputs re-run with all rows OK") else (call :ng "the saved string/array inputs re-run: exit code 0, got %RC%")
rem changing the target function resets the target and starts a new record
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step_func_name_len.json" "%U%/api/control" > "%WORK%\sfn.out"
findstr /l /c:"\"step\":1," "%WORK%\sfn.out" >nul
if errorlevel 1 (call :ng "changing the function restarts from step 1") else (call :ok "changing the function restarts from step 1")
curl -s "%U%/api/export.csv" > "%WORK%\fn_export.csv"
findstr /x /l /c:",1" "%WORK%\fn_export.csv" >nul
if errorlevel 1 (call :ok "records of the previous function are not saved under the new one") else (call :ng "records of the previous function are not saved under the new one")
"%SIM%" run "%WORK%\fn_export.csv" -o "%WORK%\ofn" --quiet > "%WORK%\fn_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\fn_rerun.log" >nul
if errorlevel 1 (call :ng "the record after a function change re-runs with all rows OK") else if "%RC%"=="0" (call :ok "the record after a function change re-runs with all rows OK") else (call :ng "the record after a function change re-runs: exit code 0, got %RC%")
curl -s -X POST --data-binary "@%T%\gui\step_func_cycle.json" "%U%/api/control" > nul
rem other round-trip losses: hex into a signed variable, $ while stopped (no double allocation), a string with ','
curl -s -X POST --data-binary "@%T%\gui\watch_rt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset_func_name_len.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_alloc_bad.json" "%U%/api/write" > "%WORK%\wab.out"
findstr /l /c:"\"error\":" "%WORK%\wab.out" >nul
if errorlevel 1 (call :ng "/api/write rejects a negative $ count") else (call :ok "/api/write rejects a negative $ count")
curl -s -X POST --data-binary "@%T%\gui\write_temp_hex.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_alloc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_comma.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\rt_export.csv"
findstr /l /c:",0xFFFF," "%WORK%\rt_export.csv" >nul
if errorlevel 1 (call :ng "a hex input is saved as written") else (call :ok "a hex input is saved as written")
"%SIM%" run "%WORK%\rt_export.csv" -o "%WORK%\ort" --quiet > "%WORK%\rt_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\rt_rerun.log" >nul
if errorlevel 1 (call :ng "hex, $ and quoted string inputs re-run with all rows OK") else if "%RC%"=="0" (call :ok "hex, $ and quoted string inputs re-run with all rows OK") else (call :ng "hex, $ and quoted string inputs re-run: exit code 0, got %RC%")
rem changing the function drops holds and watches the new function cannot resolve (@rpm), so they never write the old argument slot (@a)
curl -s -X POST --data-binary "@%T%\gui\reset_func_gear.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_rpm.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_rpm_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\reset_func_mul16.json" "%U%/api/control" > "%WORK%\rfm.out"
findstr /l /c:"dropped" "%WORK%\rfm.out" >nul
if errorlevel 1 (call :ng "/api/control reports dropped names") else (call :ok "/api/control reports dropped names")
findstr /l /c:"watch" "%WORK%\rfm.out" >nul
if errorlevel 1 (call :ng "/api/control reports the dropped watch") else (call :ok "/api/control reports the dropped watch")
curl -s -X POST --data-binary "@%T%\gui\watch_mul16.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_a3.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_b2.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\dstep.out"
findstr /l /c:"rpm" "%WORK%\dstep.out" >nul
if errorlevel 1 (call :ok "the dropped hold is not left in holds") else (call :ng "the dropped hold is not left in holds")
curl -s "%U%/api/export.csv" > "%WORK%\drop_export.csv"
rem the last cell of the last row is mul16@@, which must be 6 (14 when the old hold overwrote @a with 7)
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\drop_export.csv") do set "LAST=%%L"
if "%LAST:~-2%"==",6" (call :ok "a dropped hold does not overwrite another argument") else (call :ng "a dropped hold does not overwrite another argument: %LAST%")
findstr /l /c:"rpm" "%WORK%\drop_export.csv" >nul
if errorlevel 1 (call :ok "the saved CSV has no column for the dropped hold") else (call :ng "the saved CSV has no column for the dropped hold")
"%SIM%" run "%WORK%\drop_export.csv" -o "%WORK%\odrop" --quiet > "%WORK%\drop_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\drop_rerun.log" >nul
if errorlevel 1 (call :ng "the record after dropping a hold re-runs with all rows OK") else if "%RC%"=="0" (call :ok "the record after dropping a hold re-runs with all rows OK") else (call :ng "the record after dropping a hold re-runs: exit code 0, got %RC%")
rem the same when /api/load-csv changes the function
curl -s -X POST --data-binary "@%T%\gui\reset_func_gear.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_rpm_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\play_mul16.csv" "%U%/api/load-csv" > nul
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\lm_info.json"
findstr /l /c:"dropped" "%WORK%\lm_info.json" >nul
if errorlevel 1 (call :ng "/api/load-csv reports dropped names in state") else (call :ok "/api/load-csv reports dropped names in state")
curl -s "%U%/api/export.csv" > "%WORK%\lm_export.csv"
findstr /l /c:"rpm" "%WORK%\lm_export.csv" >nul
if errorlevel 1 (call :ok "the playback record has no column for the dropped hold") else (call :ng "the playback record has no column for the dropped hold")
"%SIM%" run "%WORK%\lm_export.csv" -o "%WORK%\olm" --quiet > "%WORK%\lm_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 2, NG 0, NO Check 0" "%WORK%\lm_rerun.log" >nul
if errorlevel 1 (call :ng "the playback record re-runs with all rows OK") else if "%RC%"=="0" (call :ok "the playback record re-runs with all rows OK") else (call :ng "the playback record re-runs: exit code 0, got %RC%")
rem a hold (P1) and a one-shot write (P1_bit.no0) to the same memory are applied in the order of the input columns, as sim run does;
rem a hold set and released while stopped (g_mode = 5) is recorded as an input of the next step; '' writes the terminator
curl -s -X POST --data-binary "@%T%\gui\reset_func_mul16.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_p1.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1bit0.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\release_p1.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_mode_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_mode.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_empty.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_alloc_empty.json" "%U%/api/write" > "%WORK%\wae.out"
findstr /l /c:"error" "%WORK%\wae.out" >nul
if errorlevel 1 (call :ng "/api/write rejects an empty string for $") else (call :ok "/api/write rejects an empty string for $")
curl -s "%U%/api/export.csv" > "%WORK%\ov_export.csv"
rem the last row ends with the outputs g_eng_name[0] (0 after writing ''), P1, g_mode. Input columns before them depend on earlier holds
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\ov_export.csv") do set "LAST=%%L"
if "%LAST:~-6%"==",0,0,5" (call :ok "writing an empty string clears the previous string") else (call :ng "writing an empty string clears the previous string: %LAST%")
"%SIM%" run "%WORK%\ov_export.csv" -o "%WORK%\oov" --quiet > "%WORK%\ov_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 4" "%WORK%\ov_rerun.log" >nul
if errorlevel 1 (call :ng "overlapping hold and one-shot inputs re-run with all rows OK") else if "%RC%"=="0" (call :ok "overlapping hold and one-shot inputs re-run with all rows OK") else (call :ng "overlapping hold and one-shot inputs re-run: exit code 0, got %RC%")
curl -s "%U%/" > "%WORK%\index.html"
findstr /l /c:"<html" "%WORK%\index.html" >nul
if errorlevel 1 (call :ng "GET / returns a page") else (call :ok "GET / returns a page")
taskkill /f /im sim_guitest.exe >nul 2>nul

:after_gui
set /a MOCK_TOTAL=PASS+FAIL
set /a MOCK_FAIL=FAIL
call "%T%\rl78\run_rl78_tests.bat"

:summary
set /a TOTAL=PASS+FAIL
echo.
if defined MOCK_TOTAL echo mock backend: %MOCK_TOTAL% checks (%MOCK_FAIL% failed)
echo checks run: %TOTAL%, passed: %PASS%, failed: %FAIL%
set "LAST="
if exist "%WORK%\cycle.log" for /f "usebackq delims=" %%L in ("%WORK%\cycle.log") do set "LAST=%%L"
if defined LAST echo 2000-row CSV (2100 rows) run time: %LAST%
rmdir /s /q "%WORK%" 2>nul
if %TOTAL%==0 exit /b 1
if %FAIL% gtr 0 exit /b 1
exit /b 0

rem ---------------------------------------------------------------- subroutines
:ok
set /a PASS+=1
echo   OK    %~1
exit /b 0

:ng
set /a FAIL+=1
echo   FAIL  %~1
exit /b 0

:run_cmp
"%SIM%" run "%T%\csv\%~1" -o "%WORK%\out_%~n1" --fixed-time --quiet > "%WORK%\run.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="%~2" (call :ok "%~1: exit code %~2") else (call :ng "%~1: exit code %~2, got %RC%")
fc /b "%WORK%\out_%~n1\%~1" "%T%\expected\%~1" >nul
if errorlevel 1 (call :ng "%~1: result CSV matches expected bytes") else (call :ok "%~1: result CSV matches expected bytes")
exit /b 0

:same_case
rem %1 = name, %2.. = arguments of sim run (run in %WORK%\same); expects exit code 2 and the input unchanged
copy /y "%T%\csv\unit_all.csv" "%WORK%\same\unit_all.csv" >nul
pushd "%WORK%\same"
"%SIM%" run %2 %3 %4 %5 --quiet > "%WORK%\same.log" 2>&1
set RC=%ERRORLEVEL%
popd
fc /b "%WORK%\same\unit_all.csv" "%T%\csv\unit_all.csv" >nul
if errorlevel 1 (call :ng "%~1: input overwritten") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0

:err_case
"%SIM%" run "%T%\csv\err\%~1" -o "%WORK%\oerr" --quiet > "%WORK%\err.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%~2" "%WORK%\err.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~2") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0
