@echo off
rem tests\rl78\run_rl78_tests.bat - checks of the real RL78 backend on Windows
rem Called from tests\run_tests.bat (shares ROOT, T, WORK, PASS, FAIL). ASCII only.
rem Japanese messages are checked through their ASCII parts (counts, addresses, names).

set "R=%T%\rl78"
set "FX=%T%\fixtures\renesas"
set "G22=%FX%\g22_adc\r01an6640-rl78g22-adc.abs"
set "G23=%FX%\g23_spi\r01an5890_rl78g23_serial_slave.abs"
if not defined CC set "CC=clang"

echo == real backend: build
if not defined SIM_RL78 (
    call "%ROOT%\build.bat" "%WORK%\real" > "%WORK%\build_real.log" 2>&1
    if errorlevel 1 (
        call :ng "build.bat (real backend)"
        type "%WORK%\build_real.log"
    ) else (
        call :ok "build.bat (real backend)"
    )
    findstr /i /c:"warning" "%WORK%\build_real.log" >nul
    if errorlevel 1 (call :ok "real backend builds without warnings") else (call :ng "real backend builds without warnings")
    set "SIM_RL78=%WORK%\real\sim.exe"
)
if not exist "%SIM_RL78%" (
    call :ng "real sim.exe not found: %SIM_RL78%"
    exit /b 0
)
set "RS=%SIM_RL78%"
"%CC%" -std=c11 -O2 -Wall -Wextra -o "%WORK%\mkabs.exe" "%T%\tools\mkabs.c" > "%WORK%\mkabs.log" 2>&1
if errorlevel 1 (call :ng "build tests\tools\mkabs.c" & type "%WORK%\mkabs.log") else (call :ok "build tests\tools\mkabs.c")
for %%K in (dwarf2 dwarf3 dwarf4 nodwarf) do (
    "%WORK%\mkabs.exe" %%K "%WORK%\tp_%%K.abs"
    if errorlevel 1 (call :ng "mkabs %%K") else (call :ok "mkabs %%K")
)
set "TP=%WORK%\tp_dwarf2.abs"

echo == real .abs (Renesas samples built with CC-RL)
"%RS%" info --abs "%G23%" > "%WORK%\g23_info.txt" 2>&1
call :has "%WORK%\g23_info.txt" "e_machine 197" "G23: ELF e_machine 197"
call :has "%WORK%\g23_info.txt" "main" "G23: startup reaches main"
call :has "%WORK%\g23_info.txt" "17 " "G23: map sections match"
"%RS%" list --abs "%G23%" > "%WORK%\g23_list.txt" 2>nul
call :has "%WORK%\g23_list.txt" "0x003E6 R_Config_CSI00_Send_Receive(uint8_t * tx_buf, uint16_t tx_num, uint8_t * rx_buf) -> MD_STATUS" "G23: function with parameters from DWARF"
call :has "%WORK%\g23_list.txt" "0xFBF04 g_rx_data_stored" "G23: global array variable"
call :run "G23: test CSV (16 rows OK)" 0 "OK 16, NG 0" --abs "%G23%" "%R%\csv\g23_spi.csv" --fixed-time
fc /b "%WORK%\r_out\g23_spi.csv" "%R%\expected\g23_spi.csv" >nul
if errorlevel 1 (call :ng "G23: result CSV matches expected bytes") else (call :ok "G23: result CSV matches expected bytes")
call :run "G22: test CSV (18 rows OK)" 0 "OK 18, NG 0" --abs "%G22%" "%R%\csv\g22_adc.csv" --fixed-time
fc /b "%WORK%\r_out\g22_adc.csv" "%R%\expected\g22_adc.csv" >nul
if errorlevel 1 (call :ng "G22: result CSV matches expected bytes") else (call :ok "G22: result CSV matches expected bytes")
call :run "G22: .data copied after RAM filled with 0xA5" 0 "OK 1, NG 0" --cfg "%R%\csv\g22_fill.cfg" "%R%\csv\g22_startup.csv"
call :run "G23: bss cleared after RAM filled with 0xA5" 0 "OK 1, NG 0" --cfg "%R%\csv\g23_fill.cfg" "%R%\csv\g23_startup.csv"

echo == generated .abs: calling convention and types
for %%K in (dwarf2 dwarf3 dwarf4) do call :run "%%K: calling convention and types (43 rows)" 0 "OK 43, NG 0" --abs "%WORK%\tp_%%K.abs" "%R%\csv\synth_call.csv" "%R%\csv\synth_types.csv"
call :run "result CSVs for byte compare" 0 "OK 43, NG 0" --abs "%TP%" "%R%\csv\synth_call.csv" "%R%\csv\synth_types.csv" --fixed-time
fc /b "%WORK%\r_out\synth_call.csv" "%R%\expected\synth_call.csv" >nul
if errorlevel 1 (call :ng "synth_call result matches expected bytes") else (call :ok "synth_call result matches expected bytes")
fc /b "%WORK%\r_out\synth_types.csv" "%R%\expected\synth_types.csv" >nul
if errorlevel 1 (call :ng "synth_types result matches expected bytes") else (call :ok "synth_types result matches expected bytes")
call :run "no DWARF: name:bytes and .map symbols" 0 "OK 1, NG 0" --abs "%WORK%\tp_nodwarf.abs" "%R%\csv\synth_nodwarf.csv"
"%RS%" info --abs "%TP%" -v > "%WORK%\tp_info.txt" 2>&1
call :has "%WORK%\tp_info.txt" "0x2001" "vendor attribute 0x2001 skipped with a warning"
call :has "%WORK%\tp_info.txt" "0xFE160" "heap chosen from unused RAM"

echo == generated .abs: faults, core type, stubs
call :run "runaway and undefined instruction make the row NG" 1 "OK 1, NG 2" --abs "%TP%" --cfg "%R%\cfg\limit.cfg" "%R%\csv\synth_fault.csv"
call :has "%WORK%\r.log" "PC=0x002A5" "runaway message has PC"
call :has "%WORK%\r.log" "61 19" "undefined instruction message has bytes"
call :run "halt = stop" 1 "OK 0, NG 1" --abs "%TP%" --cfg "%R%\cfg\halt_stop.cfg" "%R%\csv\synth_halt.csv"
rem a 1-byte heap with an odd exclusive end must not hand out 0xFE162 on the second allocation
call :run "1-byte heap (odd end): second $ allocation is an error (exit code 2)" 2 "0xFE160" --abs "%TP%" --cfg "%R%\cfg\heap_odd.cfg" "%R%\csv\synth_heap_odd.csv"
findstr /b /l /c:",1,0,0,0xE162,OK" "%WORK%\r_out\synth_heap_odd.csv" >nul 2>nul
if errorlevel 1 (call :ok "allocation outside the heap is not reported as OK") else (call :ng "allocation outside the heap is not reported as OK")
rem unreadable config values (out of range, with a unit, negative) are errors (exit code 2), not defaults
call :cfg_err "heap beyond the address space is an error" heap_over.cfg "heap = 0xFFFF0:0x100"
call :cfg_err "cpu_hz = 32MHz is not read as 32 Hz" bad_cpu_hz.cfg "cpu_hz = 32MHz"
call :cfg_err "max_insns = -1 is not read as a huge count" bad_max_insns.cfg "max_insns = -1"
call :cfg_err "sentinel = 0x1EFFF0 is not truncated" bad_sentinel.cfg "sentinel = 0x1EFFF0"
call :run "core = S2" 0 "OK 2, NG 0" --abs "%TP%" --cfg "%R%\cfg\core_s2.cfg" "%R%\csv\synth_core.csv"
call :run "core = S3, mdu = off" 0 "OK 2, NG 0" --abs "%TP%" --cfg "%R%\cfg\core_s3_nomdu.cfg" "%R%\csv\synth_nomdu.csv"
call :run "stub = read_io:AMSTB_read_io" 0 "OK 3, NG 0" --abs "%TP%" --cfg "%R%\cfg\stub.cfg" "%R%\csv\synth_stub.csv"
call :run "stub_auto = on" 0 "OK 3, NG 0" --abs "%TP%" --cfg "%R%\cfg\stub_auto.cfg" "%R%\csv\synth_stub.csv"
mkdir "%WORK%\autocfg"
copy /y "%TP%" "%WORK%\autocfg\prog.abs" >nul
copy /y "%R%\cfg\stub.cfg" "%WORK%\autocfg\sim.cfg" >nul
call :run "sim.cfg next to the .abs is read" 0 "OK 3, NG 0" --abs "%WORK%\autocfg\prog.abs" "%R%\csv\synth_stub.csv"

echo == real backend GUI call failures
where curl >nul 2>nul
if errorlevel 1 goto after_fault_gui
set /a PORT=22000 + %RANDOM% %% 15000
set "U=http://127.0.0.1:%PORT%"
copy /y "%RS%" "%WORK%\sim_rgui_test.exe" >nul
start "" /b "%WORK%\sim_rgui_test.exe" gui --abs "%TP%" --cfg "%R%\cfg\limit.cfg" --func TEST_cycle --no-browser --port %PORT% --html-dir "%WORK%\nohtml" > "%WORK%\rgui.log" 2>&1
for /l %%I in (1,1,50) do (
    curl -s "%U%/api/info" > "%WORK%\rinfo.json" 2>nul
    if not errorlevel 1 goto fault_gui_ready
    ping -n 2 127.0.0.1 >nul
)
:fault_gui_ready
call :gui_fault f_undef "61 19"
call :gui_fault f_loop "BR"
taskkill /f /im sim_rgui_test.exe >nul 2>nul
:after_fault_gui

echo == performance (real backend)
> "%WORK%\perf_cycle.csv" echo mod,TEST_cycle,perf,0,1
>> "%WORK%\perf_cycle.csv" echo #COMMENT,TEST_cycle_cnt
for /l %%I in (1,1,2000) do >> "%WORK%\perf_cycle.csv" echo ,%%I
call :run "TEST_cycle 2000 times" 0 "OK 2000, NG 0" --abs "%TP%" "%WORK%\perf_cycle.csv"
type "%WORK%\r.log" | findstr /v /c:"mod " /c:"->"
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

rem :has file text name
:has
findstr /l /c:"%~2" "%~1" >nul
if errorlevel 1 (call :ng "%~3") else (call :ok "%~3")
exit /b 0

rem :cfg_err name cfg-file expected_text  (sim info exits 2 with the text)
:cfg_err
"%RS%" info --abs "%TP%" --cfg "%R%\cfg\%~2" > "%WORK%\r.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%~3" "%WORK%\r.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~3") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0

rem :run name expected_exit_code expected_text sim-run-args...
:run
set "RN=%~1"
set "RC_EXP=%~2"
set "RTXT=%~3"
shift
shift
shift
"%RS%" run %1 %2 %3 %4 %5 %6 %7 %8 %9 -o "%WORK%\r_out" --quiet > "%WORK%\r.log" 2>&1
set RC=%ERRORLEVEL%
findstr /l /c:"%RTXT%" "%WORK%\r.log" >nul
if errorlevel 1 (call :ng "%RN%" & type "%WORK%\r.log") else if "%RC%"=="%RC_EXP%" (call :ok "%RN%") else (call :ng "%RN%: exit code %RC_EXP%, got %RC%")
exit /b 0

rem JSON fields are checked as JSON; findstr patterns below contain only letters, digits and spaces.
:gui_fault
> "%WORK%\fault_watch.json" echo {"names":["g_out"]}
> "%WORK%\fault_reset.json" echo {"cmd":"reset","func":"%~1"}
> "%WORK%\fault_step.json" echo {"cmd":"step","steps":1}
curl -s -X POST --data-binary "@%WORK%\fault_watch.json" "%U%/api/watch" >nul
curl -s -X POST --data-binary "@%WORK%\fault_reset.json" "%U%/api/control" >nul
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\fault.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "GUI %~1 step HTTP 200") else (call :ng "GUI %~1 step HTTP 200")
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\fault.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 0 -or $s.step -ne 0 -or $s.running -ne $false -or $s.stepsLeft -ne 0 -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 failed step stops without advancing") else (call :ok "GUI %~1 failed step stops without advancing")
call :has "%WORK%\fault.json" "PC" "GUI %~1 error has PC"
call :has "%WORK%\fault.json" "%~2" "GUI %~1 error has instruction details"
curl -s "%U%/api/export.csv" > "%WORK%\fault.csv"
powershell -NoProfile -Command "if (@(Get-Content -LiteralPath '%WORK%\fault.csv').Count -ne 2) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 export has no data rows") else (call :ok "GUI %~1 export has no data rows")
> "%WORK%\fault_step.json" echo {"cmd":"step","steps":3}
curl -s -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control" > "%WORK%\fault.json"
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\fault.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 0 -or $s.stepsLeft -ne 0 -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 step N stops at first failure") else (call :ok "GUI %~1 step N stops at first failure")
> "%WORK%\fault_reset.json" echo {"cmd":"reset","func":"TEST_cycle"}
curl -s -X POST --data-binary "@%WORK%\fault_reset.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.error -ne '') {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 reset clears error") else (call :ok "GUI %~1 reset clears error")
> "%WORK%\fault_watch.json" echo {"names":["TEST_cycle_cnt"]}
curl -s -X POST --data-binary "@%WORK%\fault_watch.json" "%U%/api/watch" >nul
curl -s -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 3 -or $s.error -ne '') {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 reset allows three steps") else (call :ok "GUI %~1 reset allows three steps")
curl -s "%U%/api/export.csv" > "%WORK%\recovered.csv"
call :run "GUI %~1 recovered CSV replays three rows OK" 0 "OK 3, NG 0" --abs "%TP%" --cfg "%R%\cfg\limit.cfg" "%WORK%\recovered.csv"
> "%WORK%\fault_play.csv" echo mod,%~1,fault,0,1
>> "%WORK%\fault_play.csv" echo #COMMENT,g_out
>> "%WORK%\fault_play.csv" echo ,0
>> "%WORK%\fault_play.csv" echo ,0
curl -s -X POST --data-binary "@%WORK%\fault_play.csv" "%U%/api/load-csv" >nul
ping -n 2 127.0.0.1 >nul
curl -s "%U%/api/info" > "%WORK%\fault_info.json"
powershell -NoProfile -Command "$s=(Get-Content -Raw -LiteralPath '%WORK%\fault_info.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.playback -ne $false -or $s.running -ne $false -or $s.step -ne 0 -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 failed playback stops without advancing") else (call :ok "GUI %~1 failed playback stops without advancing")
exit /b 0
