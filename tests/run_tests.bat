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
findstr /b /l /c:",800,120,1,1,1,10,800" "%WORK%\export.csv" >nul
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

:err_case
"%SIM%" run "%T%\csv\err\%~1" -o "%WORK%\oerr" --quiet > "%WORK%\err.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%~2" "%WORK%\err.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~2") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0
