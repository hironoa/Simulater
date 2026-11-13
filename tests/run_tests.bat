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
call :err_case big_decimal.csv "g_mode"

echo == encodings and line identification (never skip a block silently)
rem a UTF-8 BOM is skipped when identifying lines and kept as bytes in the result CSV
call :run_cmp bom_mul16.csv 1
"%SIM%" run "%WORK%\out_bom_mul16\bom_mul16.csv" -o "%WORK%\obom_rb" --fixed-time --quiet > "%WORK%\bom_rb.log" 2>&1
set RC=%ERRORLEVEL%
fc /b "%WORK%\obom_rb\bom_mul16.csv" "%T%\expected\bom_mul16.csv" >nul
if errorlevel 1 (call :ng "a result CSV with a BOM reads back the same") else if "%RC%"=="1" (call :ok "a result CSV with a BOM reads back the same") else (call :ng "a result CSV with a BOM reads back: exit code 1, got %RC%")
call :err_case utf16le_bom.csv "UTF"
call :err_case utf16be_bom.csv "UTF"
call :err_case nul_byte.csv "NUL"
call :err_case no_mod.csv "mod"
call :err_case no_comment.csv "COMMENT"
rem letter case, spaces and tabs, double quotes, a leading full-width space and extra trailing commas are accepted
call :run_cmp lenient_ident.csv 0
"%SIM%" run "%T%\csv\lenient_ident.csv" -o "%WORK%\olen" --quiet > "%WORK%\len.log" 2>&1
findstr /l /c:"OK 8, NG 0" "%WORK%\len.log" >nul
if errorlevel 1 (call :ng "all 8 spellings of mod and COMMENT run (OK 8)") else (call :ok "all 8 spellings of mod and COMMENT run (OK 8)")
rem CR-only line ends and a last line without a line end
call :run_cmp cr_only.csv 0
rem a block without data rows and lines before the first mod line are warnings, not errors
call :run_cmp only_comments.csv 0
"%SIM%" run "%T%\csv\only_comments.csv" -o "%WORK%\ooc" --quiet > nul 2> "%WORK%\oc_err.log"
findstr /l /c:"mul16" "%WORK%\oc_err.log" >nul
if errorlevel 1 (call :ng "a block without data rows is warned about") else (call :ok "a block without data rows is warned about")
call :run_cmp data_before_mod.csv 0
"%SIM%" run "%T%\csv\data_before_mod.csv" -o "%WORK%\odb" --quiet > nul 2> "%WORK%\db_err.log"
findstr /l /c:"mod" "%WORK%\db_err.log" >nul
if errorlevel 1 (call :ng "lines before the first mod line are warned about") else (call :ok "lines before the first mod line are warned about")
rem several files: the others still run and the final exit code reflects every file
"%SIM%" run "%T%\csv\err\no_mod.csv" "%T%\csv\unit_all.csv" -o "%WORK%\omulti" --quiet > "%WORK%\multi.log" 2>&1
set RC=%ERRORLEVEL%
if not exist "%WORK%\omulti\unit_all.csv" (call :ng "a file without mod lines does not stop the others") else if "%RC%"=="2" (call :ok "a file without mod lines does not stop the others (exit code 2)") else (call :ng "a file without mod lines with a normal file: exit code 2, got %RC%")
"%SIM%" run "%T%\csv\only_comments.csv" "%T%\csv\ng_case.csv" -o "%WORK%\omulti2" --quiet > "%WORK%\multi2.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="1" (call :ok "a warning does not change the exit code (1)") else (call :ng "a warning does not change the exit code: 1, got %RC%")

echo == numeric ranges and spellings (never silently another value)
"%SIM%" run "%T%\csv\err\num_range.csv" -o "%WORK%\onr" --quiet > "%WORK%\nr.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "out-of-range values, indexes and addresses exit 2") else (call :ng "out-of-range values, indexes and addresses exit 2, got %RC%")
findstr /l /c:"18446744073709551615" "%WORK%\nr.log" >nul
if errorlevel 1 (call :ng "a decimal above INT64_MAX does not fit 8 bits") else (call :ok "a decimal above INT64_MAX does not fit 8 bits")
findstr /l /c:"1e+39" "%WORK%\nr.log" >nul
if errorlevel 1 (call :ng "1e39 does not fit a float") else (call :ok "1e39 does not fit a float")
findstr /l /c:"'010'" "%WORK%\nr.log" >nul
if errorlevel 1 (call :ng "index 010 is not read as octal") else (call :ok "index 010 is not read as octal")
findstr /l /c:"'99999999999999999999'" "%WORK%\nr.log" >nul
if errorlevel 1 (call :ng "an index above 64 bits is not saturated") else (call :ok "an index above 64 bits is not saturated")
findstr /l /c:"'0100:1'" "%WORK%\nr.log" >nul
if errorlevel 1 (call :ng "address 0100 is not read as octal") else (call :ok "address 0100 is not read as octal")
call :run_cmp expect_num_range.csv 1

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

echo == the whole mismatch cell (actual?(expected)) is quoted as CSV
call :run_cmp expect_quote.csv 1
"%SIM%" run "%WORK%\out_expect_quote\expect_quote.csv" -o "%WORK%\oquote_rb" --fixed-time --quiet > "%WORK%\quote_rb.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 3, NO Check 0" "%WORK%\quote_rb.log" >nul
if errorlevel 1 (call :ng "a quoted result CSV reads back with the expected side (OK 1, NG 3)") else if "%RC%"=="1" (call :ok "a quoted result CSV reads back with the expected side (OK 1, NG 3)") else (call :ng "a quoted result CSV reads back: exit code 1, got %RC%")
"%SIM%" run "%WORK%\out_expect_quote\expect_quote.csv" -o "%WORK%\oquote_rb2" --fixed-time --expect-actual --quiet > "%WORK%\quote_rb2.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 4, NG 0, NO Check 0" "%WORK%\quote_rb2.log" >nul
if errorlevel 1 (call :ng "a quoted result CSV with --expect-actual is all OK") else if "%RC%"=="0" (call :ok "a quoted result CSV with --expect-actual is all OK") else (call :ng "a quoted result CSV with --expect-actual: exit code 0, got %RC%")

echo == numeric options of sim gui (out of range or trailing garbage is an error)
call :gui_opt "--port 70000 is not truncated" "70000" --port 70000
call :gui_opt "--port 8765x is not read as 8765" "8765x" --port 8765x
call :gui_opt "--period-ms 1e400 (inf) is rejected" "1e400" --period-ms 1e400

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
rem writes reach memory only at the start of the next step, so a hold set and released while stopped (g_mode = 5)
rem and a one-shot write whose name is released (g_mode = 7) are never written; '' writes the terminator
curl -s -X POST --data-binary "@%T%\gui\reset_func_mul16.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_p1.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1bit0.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\release_p1.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_mode_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_mode.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_mode7.json" "%U%/api/write" > nul
curl -s "%U%/api/info" > "%WORK%\m7_info.json"
findstr /l /c:"pending" "%WORK%\m7_info.json" | findstr /l /c:"g_mode" >nul
if errorlevel 1 (call :ng "a one-shot write waiting for the next step is listed in pending") else (call :ok "a one-shot write waiting for the next step is listed in pending")
curl -s -X POST --data-binary "@%T%\gui\release_mode.json" "%U%/api/write" > nul
curl -s "%U%/api/info" > "%WORK%\m7r_info.json"
findstr /l /c:"g_mode" "%WORK%\m7r_info.json" >nul
if errorlevel 1 (call :ok "release also drops the pending write of that name") else (call :ng "release also drops the pending write of that name")
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
if "%LAST:~-6%"==",0,0,0" (call :ok "writing an empty string clears the previous string") else (call :ng "writing an empty string clears the previous string: %LAST%")
"%SIM%" run "%WORK%\ov_export.csv" -o "%WORK%\oov" --quiet > "%WORK%\ov_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 4" "%WORK%\ov_rerun.log" >nul
if errorlevel 1 (call :ng "overlapping hold and one-shot inputs re-run with all rows OK") else if "%RC%"=="0" (call :ok "overlapping hold and one-shot inputs re-run with all rows OK") else (call :ng "overlapping hold and one-shot inputs re-run: exit code 0, got %RC%")
rem writes while stopped are not applied at once: the next step writes only the last one per column ('XYZ' then 'A' gives 'A',
rem so g_eng_name[2] keeps the initial 'G' = 71), and the saved CSV re-runs with all rows OK
curl -s -X POST --data-binary "@%T%\gui\reset_func_name_len.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_name2.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_a.json" "%U%/api/write" > nul
curl -s "%U%/api/info" > "%WORK%\pa_info.json"
findstr /l /c:"pending" "%WORK%\pa_info.json" >nul
if errorlevel 1 (call :ng "state has pending") else (call :ok "state has pending")
findstr /l /c:"XYZ" "%WORK%\pa_info.json" >nul
if errorlevel 1 (call :ok "a rewrite of the same name replaces the pending write") else (call :ng "a rewrite of the same name replaces the pending write")
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\pa_export.csv"
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\pa_export.csv") do set "LAST=%%L"
if "%LAST:~-3%"==",71" (call :ok "only the last write while stopped is applied (g_eng_name[2] = 71)") else (call :ng "only the last write while stopped is applied (g_eng_name[2] = 71): %LAST%")
"%SIM%" run "%WORK%\pa_export.csv" -o "%WORK%\opa" --quiet > "%WORK%\pa_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\pa_rerun.log" >nul
if errorlevel 1 (call :ng "writes rewritten while stopped re-run with all rows OK") else if "%RC%"=="0" (call :ok "writes rewritten while stopped re-run with all rows OK") else (call :ng "writes rewritten while stopped re-run: exit code 0, got %RC%")
rem CSV playback writes in the order of the loaded CSV input columns (P1_bit.no0, P1), even after P1 and P1_bit.no0 were written in that order
curl -s -X POST --data-binary "@%T%\gui\reset_func_mul16.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_p1only.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1_one.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1bit0.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\play_order.csv" "%U%/api/load-csv" > nul
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/export.csv" > "%WORK%\po_export.csv"
rem the outputs at the end of the row are P1 and P1_bit.no0 (both 1 when P1 = 1 is written last)
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\po_export.csv") do set "LAST=%%L"
if "%LAST:~-4%"==",1,1" (call :ok "CSV playback keeps the input column order of the CSV") else (call :ng "CSV playback keeps the input column order of the CSV: %LAST%")
rem input columns with the same name: sim run writes them from left to right (P1=0, P1_bit.no0=1, P1=2 gives P1 = 2);
rem /api/load-csv rejects the CSV with 400 and changes nothing
"%SIM%" run "%T%\gui\play_dup.csv" -o "%WORK%\odup" --quiet > "%WORK%\dup_run.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\dup_run.log" >nul
if errorlevel 1 (call :ng "sim run writes input columns with the same name from left to right") else if "%RC%"=="0" (call :ok "sim run writes input columns with the same name from left to right") else (call :ng "sim run writes input columns with the same name: exit code 0, got %RC%")
curl -s -X POST --data-binary "@%T%\gui\watch_p1only.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_p1_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/info" > "%WORK%\dup_info0.json"
curl -s "%U%/api/export.csv" > "%WORK%\dup_export0.csv"
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\dup.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_dup.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
findstr /l /c:"sim run" "%WORK%\dup.out" >nul
if errorlevel 1 (call :ng "/api/load-csv rejects input columns with the same name") else if "%CODE%"=="400" (call :ok "/api/load-csv rejects input columns with the same name") else (call :ng "/api/load-csv rejects input columns with the same name: 400 expected, got %CODE%")
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\dup_info1.json"
curl -s "%U%/api/export.csv" > "%WORK%\dup_export1.csv"
fc /b "%WORK%\dup_info0.json" "%WORK%\dup_info1.json" >nul
if errorlevel 1 (call :ng "a rejected CSV with same-name inputs changes nothing (info)") else (call :ok "a rejected CSV with same-name inputs changes nothing (info)")
fc /b "%WORK%\dup_export0.csv" "%WORK%\dup_export1.csv" >nul
if errorlevel 1 (call :ng "a rejected CSV with same-name inputs changes nothing (record)") else (call :ok "a rejected CSV with same-name inputs changes nothing (record)")
curl -s -X POST --data-binary "@%T%\gui\release_p1.json" "%U%/api/write" > nul
rem input columns spelled differently for the same memory (P1 and P1_bit.no0, a[0] and a[ 0 ], g.x and g .x) are separate
rem columns written in column order; same-name outputs and an input with the name of an output are accepted
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\al.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_alias.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "/api/load-csv accepts inputs spelled differently for the same memory") else (call :ng "/api/load-csv accepts inputs spelled differently for the same memory: 200 expected, got %CODE%")
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/export.csv" > "%WORK%\al_export.csv"
"%SIM%" run "%WORK%\al_export.csv" -o "%WORK%\oal" --quiet > "%WORK%\al_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\al_rerun.log" >nul
if errorlevel 1 (call :ng "the record of differently spelled inputs re-runs with all rows OK") else if "%RC%"=="0" (call :ok "the record of differently spelled inputs re-runs with all rows OK") else (call :ng "the record of differently spelled inputs re-runs: exit code 0, got %RC%")
rem a CSV with two mod blocks plays only the first one and says so in the response
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\mb.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_multi.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
findstr /l /c:"\"blocks\":2,\"warning\":" "%WORK%\mb.out" >nul
if errorlevel 1 (call :ng "/api/load-csv reports the mod blocks it does not play") else if "%CODE%"=="200" (call :ok "/api/load-csv reports the mod blocks it does not play") else (call :ng "/api/load-csv reports the mod blocks it does not play: 200 expected, got %CODE%")
ping -n 2 127.0.0.1 >nul
rem round trip: GUI operations, /api/export.csv, then sim run without --expect-actual gives all rows OK
rem (overlapping inputs, strings and arrays, $ptr, holds with one-shot writes, release, a reset in the middle, CSV playback)
curl -s -X POST --data-binary "@%T%\gui\reset_func_mul16.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_watch.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_p1_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1bit0.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_name_q.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_a_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_b2.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_p1bit0.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_alloc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\release_p1.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_b5.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_arr.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\rt1_export.csv"
"%SIM%" run "%WORK%\rt1_export.csv" -o "%WORK%\ort1" --quiet > "%WORK%\rt1_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 2, NG 0, NO Check 0" "%WORK%\rt1_rerun.log" >nul
if errorlevel 1 (call :ng "round trip: the record after a reset re-runs with all rows OK") else if "%RC%"=="0" (call :ok "round trip: the record after a reset re-runs with all rows OK") else (call :ng "round trip: the record after a reset re-runs: exit code 0, got %RC%")
curl -s -X POST --data-binary "@%T%\gui\rt_play.csv" "%U%/api/load-csv" > nul
ping -n 2 127.0.0.1 >nul
curl -s -X POST --data-binary "@%T%\gui\rt_p1_16.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_name_a.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_alloc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\rt_release_a.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_b2.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\rt2_export.csv"
"%SIM%" run "%WORK%\rt2_export.csv" -o "%WORK%\ort2" --quiet > "%WORK%\rt2_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 5, NG 0, NO Check 0" "%WORK%\rt2_rerun.log" >nul
if errorlevel 1 (call :ng "round trip: playback followed by operations re-runs with all rows OK") else if "%RC%"=="0" (call :ok "round trip: playback followed by operations re-runs with all rows OK") else (call :ng "round trip: playback followed by operations re-runs: exit code 0, got %RC%")
rem a step whose input and output cells are all empty is saved with "step N" in the comment column,
rem so sim run does not skip it as an empty row and calls the function as many times as the GUI did
rem release the holds left by earlier checks (a reset keeps holds), so that empty steps are really empty
curl -s -X POST --data-binary "@%T%\gui\release_adc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_none.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset_func_cycle.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\en_export.csv"
findstr /l /c:"step 1" "%WORK%\en_export.csv" >nul
if errorlevel 1 (call :ng "an empty step before watching is saved with step 1 in the comment column") else (call :ok "an empty step before watching is saved with step 1 in the comment column")
"%SIM%" run "%WORK%\en_export.csv" -o "%WORK%\oen" --quiet > "%WORK%\en_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 1" "%WORK%\en_rerun.log" >nul
if errorlevel 1 (call :ng "a record with an empty step re-runs with the same number of calls") else if "%RC%"=="0" (call :ok "a record with an empty step re-runs with the same number of calls") else (call :ng "a record with an empty step re-runs: exit code 0, got %RC%")
rem a block with 0 inputs and 0 outputs (saved with nothing watched or written)
rem release the holds left by earlier checks (a reset keeps holds), so that empty steps are really empty
curl -s -X POST --data-binary "@%T%\gui\release_adc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_none.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset_func_cycle.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step3.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\e0_export.csv"
findstr /l /c:"step 2" "%WORK%\e0_export.csv" >nul
if errorlevel 1 (call :ng "a record with nothing watched or written is a block of 0 inputs and 0 outputs") else (call :ok "a record with nothing watched or written is a block of 0 inputs and 0 outputs")
findstr /l /c:"step 3" "%WORK%\e0_export.csv" >nul
if errorlevel 1 (call :ng "every row of a 0 input 0 output record has step N and a comma") else (call :ok "every row of a 0 input 0 output record has step N and a comma")
"%SIM%" run "%WORK%\e0_export.csv" -o "%WORK%\oe0" --quiet > "%WORK%\e0_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 0, NG 0, NO Check 3" "%WORK%\e0_rerun.log" >nul
if errorlevel 1 (call :ng "a 0 input 0 output record re-runs with 3 calls") else if "%RC%"=="0" (call :ok "a 0 input 0 output record re-runs with 3 calls") else (call :ng "a 0 input 0 output record re-runs: exit code 0, got %RC%")
rem inputs and watches added and removed during the record (earlier rows have the later columns empty);
rem the last row watches eng_update@call_cnt to check the number of calls
rem release the holds left by earlier checks (a reset keeps holds), so that empty steps are really empty
curl -s -X POST --data-binary "@%T%\gui\release_adc.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_none.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\reset_func_cycle.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\write_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\release_ign.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_none.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > nul
curl -s "%U%/api/export.csv" > "%WORK%\em_export.csv"
findstr /l /c:"step 6" "%WORK%\em_export.csv" >nul
if errorlevel 1 (call :ng "an empty step after removing the watch is saved with step 6") else (call :ok "an empty step after removing the watch is saved with step 6")
set "LAST="
for /f "usebackq delims=" %%L in ("%WORK%\em_export.csv") do set "LAST=%%L"
if "%LAST:~-3%"==",,7" (call :ok "the last row has eng_update@call_cnt = 7") else (call :ng "the last row has eng_update@call_cnt = 7: %LAST%")
"%SIM%" run "%WORK%\em_export.csv" -o "%WORK%\oem" --quiet > "%WORK%\em_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 2, NG 0, NO Check 5" "%WORK%\em_rerun.log" >nul
if errorlevel 1 (call :ng "a record with columns added and removed re-runs with the same number of calls") else if "%RC%"=="0" (call :ok "a record with columns added and removed re-runs with the same number of calls") else (call :ng "a record with columns added and removed re-runs: exit code 0, got %RC%")
rem sim run skips only rows whose cells are all empty, comment column included; a row with only a comment is run
"%SIM%" run "%T%\csv\comment_col_only.csv" -o "%WORK%\occ" --quiet > "%WORK%\cc_rerun.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 1" "%WORK%\cc_rerun.log" >nul
if errorlevel 1 (call :ng "a row with only the comment column is run") else if "%RC%"=="0" (call :ok "a row with only the comment column is run") else (call :ng "a row with only the comment column is run: exit code 0, got %RC%")
findstr /b /l /c:"note,1,NO Check," "%WORK%\occ\comment_col_only.csv" >nul
if errorlevel 1 (call :ng "the comment-only row is written with its actual value") else (call :ok "the comment-only row is written with its actual value")
rem numbers out of range are rejected with 400 instead of being clamped or rounded
curl -s -X POST --data-binary "@%T%\gui\control_bad_speed.json" "%U%/api/control" > "%WORK%\nv1.out"
findstr /l /c:"\"error\":" "%WORK%\nv1.out" >nul
if errorlevel 1 (call :ng "speed 1000 is rejected, not clamped to 100") else (call :ok "speed 1000 is rejected, not clamped to 100")
curl -s -X POST --data-binary "@%T%\gui\control_bad_steps.json" "%U%/api/control" > "%WORK%\nv2.out"
findstr /l /c:"\"error\":" "%WORK%\nv2.out" >nul
if errorlevel 1 (call :ng "steps 0 is rejected, not turned into 1") else (call :ok "steps 0 is rejected, not turned into 1")
curl -s -X POST --data-binary "@%T%\gui\write_u64_exact.json" "%U%/api/write" > nul
curl -s "%U%/api/info" > "%WORK%\nv6.json"
findstr /l /c:"9007199254740993" "%WORK%\nv6.json" >nul
if errorlevel 1 (call :ng "a large integer is written as typed, not rounded") else (call :ok "a large integer is written as typed, not rounded")
curl -s -X POST --data-binary "@%T%\gui\release_u64.json" "%U%/api/write" > nul
rem hold values are returned as the written cell text (a large integer is not rounded, hex is not turned into decimal)
curl -s -X POST --data-binary "@%T%\gui\write_u64_hold.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\write_hex_hold.json" "%U%/api/write" > nul
curl -s "%U%/api/info" > "%WORK%\hh_info.json"
findstr /l /c:"{\"name\":\"0x100:8\",\"value\":\"9007199254740993\"}" "%WORK%\hh_info.json" >nul
if errorlevel 1 (call :ng "holds keep a large integer as written") else (call :ok "holds keep a large integer as written")
curl -s -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > "%WORK%\hh_ctl.out"
findstr /l /c:"\"value\":\"0x78\"}" "%WORK%\hh_ctl.out" >nul
if errorlevel 1 (call :ng "holds keep hex as written") else (call :ok "holds keep hex as written")
curl -s -X POST --data-binary "@%T%\gui\release_u64.json" "%U%/api/write" > nul
curl -s -X POST --data-binary "@%T%\gui\release_hex.json" "%U%/api/write" > nul
curl -s "%U%/" > "%WORK%\index.html"
findstr /l /c:"<html" "%WORK%\index.html" >nul
if errorlevel 1 (call :ng "GET / returns a page") else (call :ok "GET / returns a page")
rem a CSV with a UTF-8 BOM loads; UTF-16 is rejected with a clear message
curl -s -X POST --data-binary "@%T%\gui\play_bom.csv" "%U%/api/load-csv" > "%WORK%\lbom.out"
findstr /l /c:"rows" "%WORK%\lbom.out" >nul
if errorlevel 1 (call :ng "/api/load-csv accepts a UTF-8 BOM") else (call :ok "/api/load-csv accepts a UTF-8 BOM")
ping -n 2 127.0.0.1 >nul
curl -s -X POST --data-binary "@%T%\gui\play_utf16.csv" "%U%/api/load-csv" > "%WORK%\lu16.out"
findstr /l /c:"UTF" "%WORK%\lu16.out" >nul
if errorlevel 1 (call :ng "/api/load-csv rejects UTF-16 with a clear message") else (call :ok "/api/load-csv rejects UTF-16 with a clear message")
rem Rejected input cells and empty CSV files must leave all state untouched.
curl -s -X POST --data-binary "@%T%\gui\release_adc.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\release_ign.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\reset_func_cycle.json" "%U%/api/control" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\er.out"
curl -s "%U%/api/info" > "%WORK%\reject_info0"
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv0"
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\reject.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_out_of_range.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "play_out_of_range returns 400") else (call :ng "play_out_of_range returns 400: got %CODE%")
findstr /l /c:"999999" "%WORK%\reject.out" >nul
if errorlevel 1 (call :ng "play_out_of_range reports the rejected value or error") else (call :ok "play_out_of_range reports the rejected value or error")
curl -s "%U%/api/info" > "%WORK%\reject_info1"
fc /b "%WORK%\reject_info0" "%WORK%\reject_info1" >nul
if errorlevel 1 (call :ng "play_out_of_range leaves info unchanged") else (call :ok "play_out_of_range leaves info unchanged")
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv1"
fc /b "%WORK%\reject_csv0" "%WORK%\reject_csv1" >nul
if errorlevel 1 (call :ng "play_out_of_range leaves export.csv unchanged") else (call :ok "play_out_of_range leaves export.csv unchanged")
curl -s "%U%/api/info" > "%WORK%\reject_info0"
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv0"
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\reject.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_bad_value.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "play_bad_value returns 400") else (call :ng "play_bad_value returns 400: got %CODE%")
findstr /l /c:"abc" "%WORK%\reject.out" >nul
if errorlevel 1 (call :ng "play_bad_value reports the rejected value or error") else (call :ok "play_bad_value reports the rejected value or error")
curl -s "%U%/api/info" > "%WORK%\reject_info1"
fc /b "%WORK%\reject_info0" "%WORK%\reject_info1" >nul
if errorlevel 1 (call :ng "play_bad_value leaves info unchanged") else (call :ok "play_bad_value leaves info unchanged")
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv1"
fc /b "%WORK%\reject_csv0" "%WORK%\reject_csv1" >nul
if errorlevel 1 (call :ng "play_bad_value leaves export.csv unchanged") else (call :ok "play_bad_value leaves export.csv unchanged")
curl -s "%U%/api/info" > "%WORK%\reject_info0"
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv0"
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\reject.out" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\play_empty.csv" "%U%/api/load-csv"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "play_empty returns 400") else (call :ng "play_empty returns 400: got %CODE%")
findstr /l /c:"error" "%WORK%\reject.out" >nul
if errorlevel 1 (call :ng "play_empty reports the rejected value or error") else (call :ok "play_empty reports the rejected value or error")
curl -s "%U%/api/info" > "%WORK%\reject_info1"
fc /b "%WORK%\reject_info0" "%WORK%\reject_info1" >nul
if errorlevel 1 (call :ng "play_empty leaves info unchanged") else (call :ok "play_empty leaves info unchanged")
curl -s "%U%/api/export.csv" > "%WORK%\reject_csv1"
fc /b "%WORK%\reject_csv0" "%WORK%\reject_csv1" >nul
if errorlevel 1 (call :ng "play_empty leaves export.csv unchanged") else (call :ok "play_empty leaves export.csv unchanged")
curl -s -X POST --data-binary "@%T%\gui\period_slow.json" "%U%/api/control" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\play_null.csv" "%U%/api/load-csv" > "%WORK%\null_load.out"
findstr /l /c:"rows" "%WORK%\null_load.out" >nul
if errorlevel 1 (call :ng "memory dependent input loads successfully") else (call :ok "memory dependent input loads successfully")
ping -n 2 127.0.0.1 >nul
curl -s -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step5.json" "%U%/api/control" > "%WORK%\null_step.out"
findstr /l /c:"NULL" "%WORK%\null_step.out" >nul
if errorlevel 1 (call :ng "failed CSV step reports NULL") else (call :ok "failed CSV step reports NULL")
findstr /l /c:"7" "%WORK%\null_step.out" >nul
if errorlevel 1 (call :ng "failed CSV step reports value") else (call :ok "failed CSV step reports value")
findstr /l /c:"stepsDone" "%WORK%\null_step.out" >nul
if errorlevel 1 (call :ng "failed CSV step reports stepsDone") else (call :ok "failed CSV step reports stepsDone")
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\null_step.out' | ConvertFrom-Json; if ($s.stepsDone -eq 1 -and $s.step -eq 2 -and !$s.running -and !$s.playback -and !$s.timedOut -and $s.error.Contains('CSV 3')) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "step N stops at the failing data row") else (call :ok "step N stops at the failing data row")
curl -s "%U%/api/export.csv" > "%WORK%\null_export.csv"
"%SIM%" run "%WORK%\null_export.csv" -o "%WORK%\onull_export" --quiet > "%WORK%\null_export.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 2, NG 0, NO Check 0" "%WORK%\null_export.log" >nul
if errorlevel 1 (call :ng "failed cycle adds no call or record and two rows re-run OK") else if "%RC%"=="0" (call :ok "failed cycle adds no call or record and two rows re-run OK") else (call :ng "failed cycle adds no call or record and two rows re-run OK: exit %RC%")
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" >nul
curl -s "%U%/api/export.csv" > "%WORK%\null_export.csv"
curl -s -X POST --data-binary "@%T%\gui\write_null.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step5.json" "%U%/api/control" > "%WORK%\null_write.out"
findstr /l /c:"NULL" "%WORK%\null_write.out" >nul
if errorlevel 1 (call :ng "write_null reports NULL") else (call :ok "write_null reports NULL")
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\null_write.out' | ConvertFrom-Json; if ($s.stepsDone -eq 0 -and $s.step -eq 0 -and !$s.running -and $s.needReset -eq $true) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "write_null stops without advancing") else (call :ok "write_null stops without advancing")
curl -s "%U%/api/export.csv" > "%WORK%\null_after.csv"
powershell -NoProfile -Command "if (@(Get-Content -LiteralPath '%WORK%\null_after.csv').Count -eq 2) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "write_null adds no record") else (call :ok "write_null adds no record")
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" >nul
curl -s -X POST --data-binary "@%T%\gui\release_null.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" >nul
curl -s "%U%/api/export.csv" > "%WORK%\null_export.csv"
curl -s -X POST --data-binary "@%T%\gui\hold_null.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step5.json" "%U%/api/control" > "%WORK%\null_write.out"
findstr /l /c:"NULL" "%WORK%\null_write.out" >nul
if errorlevel 1 (call :ng "hold_null reports NULL") else (call :ok "hold_null reports NULL")
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\null_write.out' | ConvertFrom-Json; if ($s.stepsDone -eq 0 -and $s.step -eq 0 -and !$s.running -and $s.needReset -eq $true) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "hold_null stops without advancing") else (call :ok "hold_null stops without advancing")
curl -s "%U%/api/export.csv" > "%WORK%\null_after.csv"
powershell -NoProfile -Command "if (@(Get-Content -LiteralPath '%WORK%\null_after.csv').Count -eq 2) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "hold_null adds no record") else (call :ok "hold_null adds no record")
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" >nul
curl -s -X POST --data-binary "@%T%\gui\release_null.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\watch_call_cnt.json" "%U%/api/watch" >nul
curl -s -N --max-time 0.3 "%U%/api/stream" > "%WORK%\null_values" 2>nul
powershell -NoProfile -Command "$v=Get-Content '%WORK%\null_values' | Where-Object {$_ -like 'data: *values*'} | Select-Object -First 1; $s=$v.Substring(6) | ConvertFrom-Json; if ($s.values.Count -eq 1 -and $s.values[0] -eq 0) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "failed writes after reset never call the function") else (call :ok "failed writes after reset never call the function")
rem Partial writes require reset before any further execution or input reservation.
curl -s -X POST --data-binary "@%T%\gui\reset_func_name_len.json" "%U%/api/control" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\watch_name2.json" "%U%/api/watch" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\write_null_one.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\nr_failed.json"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\nr_failed.json' | ConvertFrom-Json; if ($null -ne $s -and ($s.needReset -eq $true -and $s.step -eq 0 -and $s.error.Contains('NULL'))) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "partial write failure requires reset at step zero") else (call :ok "partial write failure requires reset at step zero")
curl -s "%U%/api/info" > "%WORK%\nr_before.json"
curl -s "%U%/api/export.csv" > "%WORK%\nr_before.csv"
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\write_name_a.json" "%U%/api/write"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "write_name_a rejected with HTTP 400 until reset") else (call :ng "write_name_a rejected with HTTP 400 until reset")
findstr /l /c:"NULL" "%WORK%\nr_rejected.json" >nul
if errorlevel 1 (call :ng "rejection preserves failure reason") else (call :ok "rejection preserves failure reason")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\hold_null.json" "%U%/api/write"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "hold_null rejected with HTTP 400 until reset") else (call :ng "hold_null rejected with HTTP 400 until reset")
findstr /l /c:"NULL" "%WORK%\nr_rejected.json" >nul
if errorlevel 1 (call :ng "rejection preserves failure reason") else (call :ok "rejection preserves failure reason")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\release_null.json" "%U%/api/write"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "release_null rejected with HTTP 400 until reset") else (call :ng "release_null rejected with HTTP 400 until reset")
findstr /l /c:"NULL" "%WORK%\nr_rejected.json" >nul
if errorlevel 1 (call :ng "rejection preserves failure reason") else (call :ok "rejection preserves failure reason")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\run.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "run rejected with HTTP 400 until reset") else (call :ng "run rejected with HTTP 400 until reset")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "step1 rejected with HTTP 400 until reset") else (call :ng "step1 rejected with HTTP 400 until reset")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nr_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%T%\gui\run_blocked.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "run_blocked rejected with HTTP 400 until reset") else (call :ng "run_blocked rejected with HTTP 400 until reset")
curl -s "%U%/api/info" > "%WORK%\nr_after.json"
curl -s "%U%/api/export.csv" > "%WORK%\nr_after.csv"
fc /b "%WORK%\nr_before.json" "%WORK%\nr_after.json" >nul
if errorlevel 1 (call :ng "rejections leave state and settings unchanged") else (call :ok "rejections leave state and settings unchanged")
powershell -NoProfile -Command "if (@(Get-Content -LiteralPath '%WORK%\nr_after.csv').Count -eq 2) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "rejections leave records unchanged") else (call :ok "rejections leave records unchanged")
curl -s -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > "%WORK%\nr_pause.json"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\nr_pause.json' | ConvertFrom-Json; if ($null -ne $s -and ($s.needReset -eq $true -and $s.error.Contains('NULL'))) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "pause preserves needReset and error") else (call :ok "pause preserves needReset and error")
curl -s -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > "%WORK%\nr_reset.json"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\nr_reset.json' | ConvertFrom-Json; if ($null -ne $s -and ($s.needReset -eq $false -and $s.error -eq '' -and $s.step -eq 0)) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "reset clears needReset and error") else (call :ok "reset clears needReset and error")
curl -s -X POST --data-binary "@%T%\gui\write_name_a.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\er.out"
curl -s "%U%/api/export.csv" > "%WORK%\nr_reset.csv"
"%SIM%" run "%WORK%\nr_reset.csv" -o "%WORK%\onr_reset" --quiet > "%WORK%\nr_reset.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\nr_reset.log" >nul
if errorlevel 1 (call :ng "reset then A replays one row OK") else if "%RC%"=="0" (call :ok "reset then A replays one row OK") else (call :ng "reset then A replays one row OK: exit %RC%")
curl -s -X POST --data-binary "@%T%\gui\write_name_str.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\write_null_one.json" "%U%/api/write" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\nr_prefix.json"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\nr_prefix.json' | ConvertFrom-Json; if ($null -ne $s -and ($s.needReset -eq $true -and $s.step -eq 1)) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "failure preserves previous successful step") else (call :ok "failure preserves previous successful step")
curl -s "%U%/api/export.csv" > "%WORK%\nr_prefix.csv"
"%SIM%" run "%WORK%\nr_prefix.csv" -o "%WORK%\onr_prefix" --quiet > "%WORK%\nr_prefix.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 1, NG 0, NO Check 0" "%WORK%\nr_prefix.log" >nul
if errorlevel 1 (call :ng "normal record before partial failure replays one row OK") else if "%RC%"=="0" (call :ok "normal record before partial failure replays one row OK") else (call :ng "normal record before partial failure replays one row OK: exit %RC%")
curl -s -X POST --data-binary "@%T%\gui\play_name_a.csv" "%U%/api/load-csv" > "%WORK%\nr_load.json"
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\nr_load_info.json"
powershell -NoProfile -Command "$s=(Get-Content -Raw '%WORK%\nr_load_info.json' | ConvertFrom-Json).state; if ($null -ne $s -and ($s.needReset -eq $false -and $s.error -eq '' -and $s.step -eq 1)) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "successful CSV load clears needReset and error") else (call :ok "successful CSV load clears needReset and error")
curl -s -X POST --data-binary "@%T%\gui\write_null_one.json" "%U%/api/write" >nul
curl -s -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" >nul
curl -s -X POST --data-binary "@%T%\gui\func_cycle.json" "%U%/api/control" > "%WORK%\nr_func.json"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\nr_func.json' | ConvertFrom-Json; if ($null -ne $s -and ($s.needReset -eq $false -and $s.error -eq '')) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "function change clears needReset and error") else (call :ok "function change clears needReset and error")
curl -s -X POST --data-binary "@%T%\gui\period_normal.json" "%U%/api/control" > "%WORK%\er.out"
curl -s -X POST --data-binary "@%T%\gui\play_null.csv" "%U%/api/load-csv" >nul
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\null_auto_info"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\null_auto_info' | ConvertFrom-Json; if ($s.state.step -eq 2 -and !$s.state.running -and !$s.state.playback -and $s.state.error.Contains('NULL')) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "running CSV stops on input failure") else (call :ok "running CSV stops on input failure")
curl -s -X POST --data-binary "@%T%\gui\play_three.csv" "%U%/api/load-csv" > "%WORK%\er.out"
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\three_info"
powershell -NoProfile -Command "$s=Get-Content -Raw '%WORK%\three_info' | ConvertFrom-Json; if ($s.state.step -eq 3 -and !$s.state.running -and !$s.state.playback) {exit 0} else {exit 1}" >nul
if errorlevel 1 (call :ng "three CSV rows finish paused at step three") else (call :ok "three CSV rows finish paused at step three")
curl -s "%U%/api/export.csv" > "%WORK%\three_export.csv"
"%SIM%" run "%WORK%\three_export.csv" -o "%WORK%\othree_export" --quiet > "%WORK%\three_export.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"OK 3, NG 0, NO Check 0" "%WORK%\three_export.log" >nul
if errorlevel 1 (call :ng "three CSV rows produce exactly three OK records") else if "%RC%"=="0" (call :ok "three CSV rows produce exactly three OK records") else (call :ng "three CSV rows produce exactly three OK records: exit %RC%")
rem a long period wait wakes on /api/control: with periodMs 5000, run (the first step runs at once), pause, then step
rem answers without waiting for the end of the period and advances to step 2
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\run_long.json" "%U%/api/control" > nul
ping -n 2 127.0.0.1 >nul
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\lp2.out" -w "%%{http_code}" --max-time 1.5 -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "long period: pause while running answers at once") else (call :ng "long period: pause while running answers at once: 200 expected, got %CODE%")
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\lp3.out" -w "%%{http_code}" --max-time 1.5 -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "long period: step after pause answers within 1.5 s") else (call :ng "long period: step after pause answers within 1.5 s: 200 expected, got %CODE%")
findstr /l /c:"\"running\":false,\"step\":2," "%WORK%\lp3.out" >nul
if errorlevel 1 (call :ng "long period: step after pause advances to step 2") else (call :ok "long period: step after pause advances to step 2")
rem shortening the period while running takes effect without waiting for the end of the old period
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\run_long.json" "%U%/api/control" > nul
ping -n 2 127.0.0.1 >nul
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\period_short.json" "%U%/api/control" > nul
ping -n 2 127.0.0.1 >nul
curl -s --max-time 2 "%U%/api/info" > "%WORK%\lp7.json"
set "LPOK=0"
findstr /l /c:"\"running\":true" "%WORK%\lp7.json" >nul && set "LPOK=1"
findstr /l /c:"\"step\":1," "%WORK%\lp7.json" >nul && set "LPOK=0"
if "%LPOK%"=="1" (call :ok "long period: changing the period to 2 ms while running takes effect at once") else (call :ng "long period: changing the period to 2 ms while running takes effect at once")
rem switching to the fastest speed while waiting for a long period runs at once
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\period_long.json" "%U%/api/control" > nul
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\speed_fast.json" "%U%/api/control" > nul
ping -n 2 127.0.0.1 >nul
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > "%WORK%\lp12.out"
findstr /r /c:"\"step\":[0-9][0-9][0-9][0-9]" "%WORK%\lp12.out" >nul
if errorlevel 1 (call :ng "long period: switching to the fastest speed while waiting runs at once") else (call :ok "long period: switching to the fastest speed while waiting runs at once")
rem a step that does not finish in 5 s answers with the steps done, the steps left and that it is still running; pause stops it
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\speed_restore.json" "%U%/api/control" > nul
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\lp14.out" -w "%%{http_code}" --max-time 8 -X POST --data-binary "@%T%\gui\step_huge.json" "%U%/api/control"`) do set "CODE=%%C"
findstr /l /c:"\"timedOut\":true" "%WORK%\lp14.out" >nul
if errorlevel 1 (call :ng "a step that does not finish in 5 s reports timedOut") else if "%CODE%"=="200" (call :ok "a step that does not finish in 5 s reports timedOut") else (call :ng "a step that does not finish in 5 s: 200 expected, got %CODE%")
findstr /l /c:"\"running\":true" "%WORK%\lp14.out" >nul
if errorlevel 1 (call :ng "a step that does not finish in 5 s reports running") else (call :ok "a step that does not finish in 5 s reports running")
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\pause.json" "%U%/api/control" > "%WORK%\lp15.out"
findstr /l /c:"\"stepsLeft\":0" "%WORK%\lp15.out" >nul
if errorlevel 1 (call :ng "pause stops a step that did not finish") else (call :ok "pause stops a step that did not finish")
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\step1.json" "%U%/api/control" > "%WORK%\lp16.out"
findstr /l /c:"\"stepsLeft\":0,\"stepsDone\":1,\"timedOut\":false" "%WORK%\lp16.out" >nul
if errorlevel 1 (call :ng "a finished step reports timedOut false") else (call :ok "a finished step reports timedOut false")
curl -s --max-time 2 -X POST --data-binary "@%T%\gui\reset.json" "%U%/api/control" > nul
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

:gui_opt
rem %1 = name, %2 = text in the message, %3 %4 = option and value; expects exit code 2
rem --stop-here is not an option: if the value were accepted, sim exits at it instead of starting the server
"%SIM%" gui --no-browser %3 %4 --stop-here > "%WORK%\gopt.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%~2" "%WORK%\gopt.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~2") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0

:err_case
"%SIM%" run "%T%\csv\err\%~1" -o "%WORK%\oerr" --quiet > "%WORK%\err.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%~2" "%WORK%\err.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~2") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0
