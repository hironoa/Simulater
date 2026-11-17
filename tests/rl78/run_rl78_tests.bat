@echo off
rem tests\rl78\run_rl78_tests.bat - checks of the real RL78 backend on Windows
rem Called from tests\run_tests.bat (shares ROOT, T, WORK, PASS, FAIL, HAS). ASCII only.
rem Text in the output of sim is searched with "%HAS%" (tests\tools\has_text.c), not findstr: see run_tests.bat.
rem Japanese messages are checked through their ASCII parts (counts, addresses, names).

set "R=%T%\rl78"
set "FX=%T%\fixtures\renesas"
set "G22=%FX%\g22_adc\r01an6640-rl78g22-adc.abs"
set "G23=%FX%\g23_spi\r01an5890_rl78g23_serial_slave.abs"
if not defined CC set "CC=clang"
set "GCC_FLAGS="
for %%C in ("%CC%") do if /i "%%~nC"=="gcc" set "GCC_FLAGS=-D__USE_MINGW_ANSI_STDIO=1"

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
"%CC%" %GCC_FLAGS% -std=c11 -O2 -Wall -Wextra -o "%WORK%\mkabs.exe" "%T%\tools\mkabs.c" > "%WORK%\mkabs.log" 2>&1
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
for %%K in (dwarf2 dwarf3 dwarf4) do for %%N in (g_const_mixed.ro g_const_elements[0] g_const_record.w) do call :dwarf_const "%%K" "%%N"
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
"%HAS%" /b /l /c:",1,0,0,0xE162,OK" "%WORK%\r_out\synth_heap_odd.csv" >nul 2>nul
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
call :run "stub lines in the CSV, no sim.cfg" 0 "OK 3, NG 0" --abs "%TP%" "%R%\csv\synth_stub_line.csv"
call :run "CSV stubs do not leak into the next CSV" 0 "OK 4, NG 0" --abs "%TP%" "%R%\csv\synth_stub_line.csv" "%R%\csv\synth_nostub.csv"
> "%WORK%\stub_file.csv" echo mod,use_io,x,2,2
>> "%WORK%\stub_file.csv" echo %%,"winams/AMSTB_SrcFile.c/AMSTB_read_io","ENGINE.C/read_io"
>> "%WORK%\stub_file.csv" echo #COMMENT,0xFFF1E:2,g_stub_val,use_io@@,0xFFF1E:2
>> "%WORK%\stub_file.csv" echo ,0x0100,0x0222,0x0223,0x0100
call :run "stub line with file/function names" 0 "OK 1, NG 0" --abs "%TP%" "%WORK%\stub_file.csv"
mkdir "%WORK%\autocfg"
copy /y "%TP%" "%WORK%\autocfg\prog.abs" >nul
copy /y "%R%\cfg\stub.cfg" "%WORK%\autocfg\sim.cfg" >nul
call :run "sim.cfg next to the .abs is read" 0 "OK 3, NG 0" --abs "%WORK%\autocfg\prog.abs" "%R%\csv\synth_stub.csv"
mkdir "%WORK%\cfgcwd\module"
copy /y "%TP%" "%WORK%\cfgcwd\module\prog.abs" >nul
copy /y "%R%\cfg\stub.cfg" "%WORK%\cfgcwd\sim.cfg" >nul
pushd "%WORK%\cfgcwd"
call :run "sim.cfg in the current folder is read as fallback" 0 "OK 3, NG 0" --abs "module\prog.abs" "%R%\csv\synth_stub.csv"
copy /y "%R%\cfg\stub.cfg" "module\sim.cfg" >nul
> "sim.cfg" echo init_end = no_such_func
call :run "sim.cfg next to the .abs takes priority over the current folder" 0 "OK 3, NG 0" --abs "module\prog.abs" "%R%\csv\synth_stub.csv"
popd

echo == LMF CA78K0R
"%CC%" %GCC_FLAGS% -std=c11 -O2 -Wall -Wextra -o "%WORK%\mklmf.exe" "%T%\tools\mklmf.c" > "%WORK%\mklmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "build mklmf") else (call :ng "build mklmf")
"%WORK%\mklmf.exe" normal "%WORK%\test.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf normal") else (call :ng "mklmf normal")
"%RS%" info --abs "%WORK%\test.lmf" -v > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF info") else (call :ng "LMF info")
call :has "%WORK%\lmf.log" "ca78k0r" "LMF ABI auto"
call :has "%WORK%\lmf.log" "0x0008" "LMF unknown type warning"
call :has "%WORK%\lmf.log" "0x55" "LMF unknown storage warning"
"%RS%" list --abs "%WORK%\test.lmf" > "%WORK%\lmf.list" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF list") else (call :ng "LMF list")
call :has "%WORK%\lmf.list" "arr" "LMF array symbol"
call :run "LMF ABI startup driver unknown sizes 16 rows" 0 "OK 16" --abs "%WORK%\test.lmf" "%R%\csv\lmf_call.csv"
call :run "LMF first long accepted" 0 "NO Check 1" --abs "%WORK%\test.lmf" "%R%\csv\lmf_first32.csv"
> "%WORK%\abi.cfg" echo abi = ccrl
"%RS%" info --abs "%WORK%\test.lmf" --cfg "%WORK%\abi.cfg" > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF ABI override loads") else (call :ng "LMF ABI override loads")
call :has "%WORK%\lmf.log" "ccrl" "LMF ABI override"
> "%WORK%\abi.cfg" echo abi = bogus
"%RS%" info --abs "%WORK%\test.lmf" --cfg "%WORK%\abi.cfg" > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "LMF bad ABI rejected") else (call :ng "LMF bad ABI rejected")
copy /y "%WORK%\test.lmf" "%WORK%\lmf.abs" >nul
"%RS%" info --abs "%WORK%\lmf.abs" > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF identified by bytes") else (call :ng "LMF identified by bytes")
for %%M in (truncated section symbols aux string unterminated stringcut address lines magic) do call :lmf_bad %%M
call :has "%WORK%\lmf.log" "78 56 34 12" "Unknown format magic bytes"
> "%WORK%\test.sym" echo #05
>> "%WORK%\test.sym" echo ;FF PUBLIC
>> "%WORK%\test.sym" echo 02FE001_init
>> "%WORK%\test.sym" echo =
"%RS%" info --abs "%WORK%\test.lmf" -v > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "SYM mismatch still loads") else (call :ng "SYM mismatch still loads")
call :has "%WORK%\lmf.log" "1 mismatched" "SYM mismatch warning"
del "%WORK%\test.sym"
"%WORK%\mklmf.exe" nodbg "%WORK%\nodbg.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf no debug") else (call :ng "mklmf no debug")
> "%WORK%\nodbg.sym" echo #05
>> "%WORK%\nodbg.sym" echo ;FF PUBLIC
>> "%WORK%\nodbg.sym" echo 010010E_main
>> "%WORK%\nodbg.sym" echo 0100141_cycle
>> "%WORK%\nodbg.sym" echo 02FE000_init
>> "%WORK%\nodbg.sym" echo 02FE002_ticks
>> "%WORK%\nodbg.sym" echo =
> "%WORK%\sym.csv" echo mod,cycle,SYM fallback,1,2
>> "%WORK%\sym.csv" echo #COMMENT,ticks:2,init:2,ticks:2
>> "%WORK%\sym.csv" echo ,4,0x1234,5
call :run "LMF no debug SYM addresses" 0 "OK 1" --abs "%WORK%\nodbg.lmf" "%WORK%\sym.csv"

echo == CA78K0R probe types and calls
set "PROBE=%T%\ca78k0r_probe\DefaultBuild\ca78k0r_probe.lmf"
set "B=%ROOT%\runtime\backends\rl78"
"%CC%" %GCC_FLAGS% -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"%B%" -I"%ROOT%\runtime\include" -I"%ROOT%\runtime\src" "%T%\tools\check_lmf_types.c" "%B%\rl78_lmf.c" "%B%\rl78_elf.c" "%B%\rl78_dwarf.c" "%B%\rl78_map.c" "%ROOT%\runtime\src\sim_util.c" -o "%WORK%\lmf_types.exe" > "%WORK%\types_build.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF type checker build") else (call :ng "LMF type checker build" & type "%WORK%\types_build.log")
"%WORK%\lmf_types.exe" "%WORK%\test.lmf" constargs > "%WORK%\const_types.txt" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF argument top const and variable return const") else (call :ng "LMF argument top const and variable return const")
"%WORK%\lmf_types.exe" "%PROBE%" probe > "%WORK%\probe_types.txt" 2> "%WORK%\probe_warnings.txt"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "probe 57 variables 17 functions 0 warnings") else (call :ng "probe type counts and warnings")
fc /b "%WORK%\probe_types.txt" "%R%\expected\probe_types.txt" >nul
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "probe types sizes addresses signatures match reference") else (call :ng "probe types match reference")
"%RS%" list --abs "%PROBE%" > "%WORK%\probe_list_raw.txt" 2> "%WORK%\probe_stderr.txt"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "probe sim list") else (call :ng "probe sim list")
"%WORK%\lmf_types.exe" "%WORK%\probe_list_raw.txt" list > "%WORK%\probe_list.txt"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "probe list has 57 typed variables") else (call :ng "probe list has 57 typed variables")
fc /b "%WORK%\probe_list.txt" "%R%\expected\probe_list.txt" >nul
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "probe sim list matches lmfdump") else (call :ng "probe sim list matches lmfdump")
call :run "probe initial values 50 rows" 0 "OK 50" --abs "%PROBE%" "%R%\csv\probe_init.csv"
call :run "probe calls 17 rows" 0 "OK 17" --abs "%PROBE%" "%R%\csv\probe_call.csv"
call :run "probe writes 2 rows" 0 "OK 2" --abs "%PROBE%" "%R%\csv\probe_write.csv"
call :run "probe aggregate return rejected" 2 "f_ret_struct" --abs "%PROBE%" "%R%\csv\probe_ret_struct.csv"
call :run "probe float argument rejected" 2 "f_float" --abs "%PROBE%" "%R%\csv\probe_float.csv"
call :run "probe char unsigned" 0 "OK 1" --abs "%PROBE%" --cfg "%R%\cfg\probe_char.cfg" "%R%\csv\probe_char.csv"
call :run "probe 32 bit skip" 0 "OK 1" --abs "%PROBE%" --cfg "%R%\cfg\probe_skip.cfg" "%R%\csv\probe_skip.csv"
call :run "generated struct bitfields far pointer" 0 "OK 1" --abs "%WORK%\test.lmf" "%R%\csv\lmf_aggregate.csv"
for %%M in (tagref tagaux tagmismatch tagend tagcycle tagwidth) do call :lmf_badtag %%M
rem an argument whose type cannot be resolved must not be called with the integer convention; variables keep the raw integer fallback
"%WORK%\mklmf.exe" badarg "%WORK%\badarg.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf badarg") else (call :ng "mklmf badarg")
call :run "unresolved LMF argument type rejected" 2 "OK 0, NG 0" --abs "%WORK%\badarg.lmf" "%R%\csv\lmf_badarg.csv"
call :run "unresolved LMF variable falls back to raw integer" 0 "OK 1, NG 0" --abs "%WORK%\badarg.lmf" "%R%\csv\lmf_badvar.csv"

echo == real backend GUI call failures
where curl >nul 2>nul
if errorlevel 1 goto after_fault_gui
set /a PORT=22000 + %RANDOM% %% 15000
set "U=http://127.0.0.1:%PORT%"
copy /y "%RS%" "%WORK%\sim_rgui_test.exe" >nul
start "" /b "%WORK%\sim_rgui_test.exe" gui --abs "%G22%" --no-browser --port %PORT% --html-dir "%WORK%\nohtml" > "%WORK%\nofunc.log" 2>&1
for /l %%I in (1,1,50) do (
    curl -s "%U%/api/info" > "%WORK%\nofunc_before.json" 2>nul
    if not errorlevel 1 goto nofunc_ready
    ping -n 2 127.0.0.1 >nul
)
:nofunc_ready
> "%WORK%\nofunc_step.json" echo {"cmd":"step","speed":2,"periodMs":10}
> "%WORK%\nofunc_run.json" echo {"cmd":"run","speed":2,"periodMs":10}
call :nofunc_reject step
call :nofunc_reject run
powershell -NoProfile -Command "$s=(Get-Content -Encoding UTF8 -Raw -LiteralPath '%WORK%\nofunc_after.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.step -ne 0) {exit 1}"
if errorlevel 1 (call :ng "GUI unset function stays at step zero") else (call :ok "GUI unset function stays at step zero")
call :has "%WORK%\nofunc.log" "func" "GUI unset function startup explains func"
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nofunc_error.json" -w "%%{http_code}" "%U%/api/export.csv"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "GUI unset function export HTTP 400") else (call :ng "GUI unset function export HTTP 400")
call :has "%WORK%\nofunc_error.json" "func" "GUI unset function export explains func"
> "%WORK%\nofunc_set.json" echo {"func":"R_Config_ADC_Set_ADChannel","cmd":"step"}
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nofunc_result.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\nofunc_set.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "GUI func and step together HTTP 200") else (call :ng "GUI func and step together HTTP 200")
curl -s "%U%/api/export.csv" > "%WORK%\nofunc.csv"
"%HAS%" /l /c:"step 1" "%WORK%\nofunc.csv" >nul
if errorlevel 1 (call :ng "GUI func and step records one call") else (call :ok "GUI func and step records one call")
taskkill /f /im sim_rgui_test.exe >nul 2>nul
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
rem PERF_MIN: optional lower bound in million instructions per second (guards against a slow build of the hot loop)
if defined PERF_MIN (
    powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('%WORK%\r.log',[Text.Encoding]::UTF8); if ($t -match '([0-9.]+) \u767e\u4e07' -and [double]$Matches[1] -ge [double]$env:PERF_MIN) {exit 0} else {exit 1}"
    if errorlevel 1 (call :ng "speed is at least %PERF_MIN% M insns/s") else (call :ok "speed is at least %PERF_MIN% M insns/s")
)

echo == translation to C (exec = xlate / verify)
rem the translated code is built with clang (the sim itself may be built with gcc)
if not defined SIM_XLATE_CC (
    clang --version >nul 2>nul
    if errorlevel 1 if exist "C:\Program Files\LLVM\bin\clang.exe" set "SIM_XLATE_CC=C:\Program Files\LLVM\bin\clang.exe"
)
set "XW=%WORK%\xl"
if not exist "%XW%" mkdir "%XW%"
copy /y "%TP%" "%XW%\tp.abs" >nul
"%RS%" run --abs "%XW%\tp.abs" "%R%\csv\synth_call.csv" -o "%XW%\out_i" > "%XW%\i.log" 2>&1
"%RS%" run --exec xlate --abs "%XW%\tp.abs" "%R%\csv\synth_call.csv" -o "%XW%\out_x" > "%XW%\first.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "xlate: first run exits 0") else (call :ng "xlate: first run exits 0" & type "%XW%\first.log")
dir /s /b "%XW%\.simxl\current.txt" >nul 2>nul
if errorlevel 1 (call :ng "xlate: first run builds the DLL" & type "%XW%\first.log" & for /r "%XW%\.simxl" %%F in (cc.log) do type "%%F") else (call :ok "xlate: first run builds the DLL")
fc /b "%XW%\out_i\synth_call.csv" "%XW%\out_x\synth_call.csv" >nul
if errorlevel 1 (call :ng "xlate: first run result equals the interpreter") else (call :ok "xlate: first run result equals the interpreter")
"%RS%" run --exec xlate --abs "%XW%\tp.abs" "%R%\csv\synth_call.csv" -o "%XW%\out_x" > "%XW%\second.log" 2>&1
rem "translated code ran N instructions" with N > 0 (the message is Japanese: match it by code points)
powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('%XW%\second.log',[Text.Encoding]::UTF8); if ($t -match '\u5909\u63db\u3057\u305f\u30b3\u30fc\u30c9\u3067\u5b9f\u884c: [1-9]') {exit 0} else {exit 1}"
if errorlevel 1 (call :ng "xlate: second run uses the translated code" & type "%XW%\second.log") else (call :ok "xlate: second run uses the translated code")
fc /b "%XW%\out_i\synth_call.csv" "%XW%\out_x\synth_call.csv" >nul
if errorlevel 1 (call :ng "xlate: second run result equals the interpreter") else (call :ok "xlate: second run result equals the interpreter")
"%RS%" run --exec verify --abs "%XW%\tp.abs" "%R%\csv\synth_call.csv" -o "%XW%\out_v" > "%XW%\verify.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "verify: translated code and the interpreter agree") else (call :ng "verify: translated code and the interpreter agree" & type "%XW%\verify.log")
fc /b "%XW%\out_i\synth_call.csv" "%XW%\out_v\synth_call.csv" >nul
if errorlevel 1 (call :ng "verify: result equals the interpreter") else (call :ok "verify: result equals the interpreter")
"%RS%" xlate --abs "%XW%\tp.abs" > "%XW%\all.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "sim xlate translates every function") else (call :ng "sim xlate translates every function" & type "%XW%\all.log")
"%RS%" run --exec verify --abs "%XW%\tp.abs" "%R%\csv\synth_types.csv" -o "%XW%\out_v" > "%XW%\verify2.log" 2>&1
set RC=%ERRORLEVEL%
"%RS%" run --abs "%XW%\tp.abs" "%R%\csv\synth_types.csv" -o "%XW%\out_i" > "%XW%\i2.log" 2>&1
if "%RC%"=="%ERRORLEVEL%" (call :ok "verify after sim xlate: same exit code") else (call :ng "verify after sim xlate: same exit code" & type "%XW%\verify2.log")
fc /b "%XW%\out_i\synth_types.csv" "%XW%\out_v\synth_types.csv" >nul
if errorlevel 1 (call :ng "verify after sim xlate: result equals the interpreter") else (call :ok "verify after sim xlate: result equals the interpreter")
"%RS%" run --exec fast --abs "%XW%\tp.abs" "%R%\csv\synth_call.csv" -o "%XW%\out_x" > "%XW%\bad.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "bad --exec value exits 2") else (call :ng "bad --exec value exits 2")

echo == native: product C sources compiled to a PC DLL (exec = native)
rem The project is copied so that the cache folder (.simnative) and the edits below stay out of the repository.
set "NW=%WORK%\native"
xcopy /e /i /q /y "%T%\native" "%NW%" >nul
set "NC=%NW%\driver\sim.cfg"
"%RS%" run --cfg "%NC%" "%NW%\csv\native_basic.csv" -o "%NW%\out" > "%NW%\first.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "native: first run exits 0") else (call :ng "native: first run exits 0" & type "%NW%\first.log" & for /r "%NW%\driver\.simnative" %%F in (cc.log) do type "%%F")
call :has "%NW%\first.log" "OK 18, NG 0, NO Check 0" "native: all 18 rows are OK"
call :has "%NW%\out\native_basic.csv" ",0xA5,0xA5,OK" "native: native_src header register is read back"
"%HAS%" /b /l /c:",30000,30000,-5536,OK" "%NW%\out\native_basic.csv" >nul
if errorlevel 1 (call :ng "native: int is 16 bits (30000 + 30000 = -5536)") else (call :ok "native: int is 16 bits (30000 + 30000 = -5536)")
"%HAS%" /b /l /c:",0x55,0x55,0x82,OK" "%NW%\out\native_basic.csv" >nul
if errorlevel 1 (call :ng "native: registers written by address are read back") else (call :ok "native: registers written by address are read back")
"%HAS%" /b /l /c:",0x5A,0x5A,OK" "%NW%\out\native_basic.csv" >nul
if errorlevel 1 (call :ng "native: a header name uses the same #if branch as the DLL") else (call :ok "native: a header name uses the same #if branch as the DLL")
"%HAS%" /b /l /c:",65535,65535,OK" "%NW%\out\native_basic.csv" >nul
if errorlevel 1 (call :ng "native: RAM written by address is read by the function") else (call :ok "native: RAM written by address is read by the function")
"%RS%" info --cfg "%NC%" > "%NW%\info.log" 2>&1
call :has "%NW%\info.log" "native sources=6 asm=1 files=7 off=1 compiled=0 reused=7 linked=0" "native: second open compiles nothing"
call :has "%NW%\info.log" "funcs=35 vars=22 opt=0" "native: variables and functions come from the debug information"
call :has "%NW%\info.log" "defines auto=1" "native: header definitions are automatic"
copy /y "%NC%" "%NW%\driver\autoff.cfg" >nul
>> "%NW%\driver\autoff.cfg" echo defines_auto = off
call :native_err "native: disabling auto leaves P3 undefined" "P3" autoff.cfg
copy /y "%NC%" "%NW%\driver\autobad.cfg" >nul
>> "%NW%\driver\autobad.cfg" echo defines_auto = maybe
call :native_err "native: invalid defines_auto is an error" "defines_auto" autobad.cfg
call :has "%NW%\info.log" "native rewrite int=24 int_dropped=7 short_added=6 asm=4 asm_files=1" "native: legacy assembly is counted on cache reuse"
"%RS%" run --cfg "%NC%" "%NW%\csv\native_legacy.csv" -o "%NW%\out_legacy" > "%NW%\legacy.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "native: legacy run exits 0") else (call :ng "native: legacy run exits 0" & type "%NW%\legacy.log")
call :has "%NW%\legacy.log" "OK 3, NG 0, NO Check 0" "native: all legacy rows are OK"
call :has "%NW%\info.log" "startup.asm" "native: assembler sources are listed"
call :has "%NW%\info.log" "files=7 off=1" "native: a file that is not a build target is left out"
call :has "%NW%\info.log" "native undef funcs=6 data=5 skipped=0" "native: undefined assembly names are substituted"
"%RS%" run --cfg "%NC%" "%NW%\csv\native_asmcall.csv" -o "%NW%\out_asmcall" > "%NW%\asmcall.log" 2>&1
call :has "%NW%\asmcall.log" "OK 6, NG 0, NO Check 0" "native: assembly substitutes return zero"
rem a header changes: only the sources that include it are compiled again (reg.h is included by io.c only)
>> "%NW%\product\inc\reg.h" echo /* changed */
"%RS%" info --cfg "%NC%" > "%NW%\info2.log" 2>&1
call :has "%NW%\info2.log" "compiled=1 reused=6 linked=1" "native: a changed header recompiles only its users"
>> "%NW%\product\inc\types.h" echo /* changed */
"%RS%" info --cfg "%NC%" > "%NW%\info3.log" 2>&1
call :has "%NW%\info3.log" "compiled=6 reused=1 linked=1" "native: a header used by every source recompiles them all"
"%RS%" run --cfg "%NC%" "%NW%\csv\native_basic.csv" -o "%NW%\out2" > "%NW%\again.log" 2>&1
call :has "%NW%\again.log" "OK 18, NG 0, NO Check 0" "native: all rows are OK after the rebuild"

rem a header of the same name appears earlier in the search order (next to the source that includes it)
copy /y "%NW%\product\inc\reg.h" "%NW%\product\src\reg.h" >nul
"%RS%" info --cfg "%NC%" > "%NW%\info4.log" 2>&1
call :has "%NW%\info4.log" "compiled=1 reused=6 linked=1" "native: a header that shadows another recompiles its users"
del "%NW%\product\src\reg.h"
"%RS%" info --cfg "%NC%" > "%NW%\info5.log" 2>&1
call :has "%NW%\info5.log" "compiled=1 reused=6 linked=1" "native: removing the shadowing header recompiles again"
copy /y "%NW%\extra\asm_impl.c" "%NW%\driver\asm_impl.c" >nul
"%RS%" run --cfg "%NC%" "%NW%\csv\native_asmcall2.csv" -o "%NW%\out_asmcall2" > "%NW%\asmcall2.log" 2>&1
call :has "%NW%\asmcall2.log" "OK 6, NG 0, NO Check 0" "native: native_src definition overrides the substitute"
"%RS%" info --cfg "%NC%" > "%NW%\info_asmcall2.log" 2>&1
call :has "%NW%\info_asmcall2.log" "native undef funcs=5 data=5 skipped=0" "native: defined function leaves the undefined list"
del "%NW%\driver\asm_impl.c"
"%RS%" info --cfg "%NC%" > "%NW%\info_asmcall3.log" 2>&1
call :has "%NW%\info_asmcall3.log" "native undef funcs=6 data=5 skipped=0" "native: removing the definition brings the substitute back"
rem a macro that the mtpj undefines and cdef defines again: the DLL sees the cdef value (same order as the defines loader)
copy /y "%NC%" "%NW%\driver\feature.cfg" >nul
>> "%NW%\driver\feature.cfg" echo cdef = FEATURE_X=7
"%RS%" run --cfg "%NW%\driver\feature.cfg" "%NW%\csv\native_feature.csv" -o "%NW%\out_feature" > "%NW%\feature.log" 2>&1
call :has "%NW%\feature.log" "OK 1, NG 0, NO Check 0" "native: cdef overrides a macro that the mtpj undefines"
rem variables and functions come from the debug information of the DLL
"%RS%" run --cfg "%NC%" "%NW%\csv\native_vars.csv" -o "%NW%\out_vars" > "%NW%\vars.log" 2>&1
call :has "%NW%\vars.log" "OK 11, NG 0, NO Check 0" "native: all variable rows are OK"
rem the cfg is given without a folder: clang drops the leading .\ from the unit name, and vars.c/name must still resolve
pushd "%NW%\driver"
"%RS%" run --cfg sim.cfg "%NW%\csv\native_vars.csv" -o "%NW%\out_vars_rel" > "%NW%\vars_rel.log" 2>&1
popd
call :has "%NW%\vars_rel.log" "OK 11, NG 0, NO Check 0" "native: file names resolve with the cfg in the current folder"
copy /y "%NC%" "%NW%\driver\opt2.cfg" >nul
>> "%NW%\driver\opt2.cfg" echo native_opt = 2
"%RS%" run --cfg "%NW%\driver\opt2.cfg" "%NW%\csv\native_basic.csv" -o "%NW%\out_opt2" > "%NW%\opt2.log" 2>&1
call :has "%NW%\opt2.log" "OK 18, NG 0, NO Check 0" "native: optimization 2 keeps basic results"
rem with optimization 2 clang copies the 16-byte arrays with movaps, so the substitutes must be 16-byte aligned
"%RS%" run --cfg "%NW%\driver\opt2.cfg" "%NW%\csv\native_asmcall.csv" -o "%NW%\out_opt2_asmcall" > "%NW%\opt2_asmcall.log" 2>&1
call :has "%NW%\opt2_asmcall.log" "OK 6, NG 0, NO Check 0" "native: optimization 2 keeps substitute variables aligned"
copy /y "%NC%" "%NW%\driver\opt3.cfg" >nul
>> "%NW%\driver\opt3.cfg" echo native_opt = 3
call :native_err "native: invalid optimization is an error" "native_opt" opt3.cfg
copy /y "%NC%" "%NW%\driver\oldfn.cfg" >nul
>> "%NW%\driver\oldfn.cfg" echo native_func = u8 f(void)
"%RS%" run --cfg "%NW%\driver\oldfn.cfg" "%NW%\csv\native_basic.csv" -o "%NW%\out_oldfn" > "%NW%\oldfn.log" 2>&1
call :has "%NW%\oldfn.log" "OK 18, NG 0, NO Check 0" "native: obsolete function setting is ignored"
call :has "%NW%\oldfn.log" "native_func" "native: obsolete function setting warns"

rem An unmapped address fails only its row; the next row still runs.
"%RS%" run --cfg "%NC%" "%NW%\csv\native_low.csv" -o "%NW%\out3" > "%NW%\low.log" 2>&1
set RC=%ERRORLEVEL%
"%HAS%" /l /c:"0x100 " "%NW%\low.log" >nul
if errorlevel 1 (call :ng "native: reading an unmapped address names it" & type "%NW%\low.log") else if "%RC%"=="1" (call :ok "native: reading an unmapped address names it") else (call :ng "native: reading an unmapped address: exit code 1, got %RC%")
call :has "%NW%\low.log" "OK 1, NG 1, NO Check 0" "native: access fault fails only its row"
"%HAS%" /b /l /c:",1,2,3,OK" "%NW%\out3\native_low.csv" >nul
if errorlevel 1 (call :ng "native: normal row after access fault is OK") else (call :ok "native: normal row after access fault is OK")
copy /y "%NC%" "%NW%\driver\timeout.cfg" >nul
>> "%NW%\driver\timeout.cfg" echo native_timeout_ms = 300
"%RS%" run --cfg "%NW%\driver\timeout.cfg" "%NW%\csv\native_fault.csv" -o "%NW%\out_fault" > "%NW%\fault.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="1" (call :ok "native: fault rows exit 1") else (call :ng "native: fault rows exit 1" & type "%NW%\fault.log")
"%HAS%" /l /c:"OK 5, NG 3, NO Check 0" "%NW%\fault.log" >nul
if errorlevel 1 (call :ng "native: five OK rows and three NG rows") else (call :ok "native: five OK rows and three NG rows")
"%HAS%" /b /l /c:",1,2,3,OK" "%NW%\out_fault\native_fault.csv" >nul
if errorlevel 1 (call :ng "native: continues after access fault") else (call :ok "native: continues after access fault")
"%HAS%" /b /l /c:",10,5,2,OK" "%NW%\out_fault\native_fault.csv" >nul
if errorlevel 1 (call :ng "native: continues after divide by zero") else (call :ok "native: continues after divide by zero")
"%HAS%" /b /l /c:",0x80,1,OK" "%NW%\out_fault\native_fault.csv" > "%NW%\wait_ok.log"
set "WAIT_OK=0"
for /f "usebackq delims=" %%L in ("%NW%\wait_ok.log") do set /a WAIT_OK+=1 >nul
if "%WAIT_OK%"=="2" (call :ok "native: both ready rows including after timeout are OK") else (call :ng "native: both ready rows including after timeout are OK")
call :has "%NW%\fault.log" "native_timeout_ms" "native: timeout names its setting"
rem The stopped source line is shown as path:line. The line numbers are those of io.c (marked there with a comment).
call :has "%NW%\fault.log" "\src\io.c:24" "native: access fault shows io.c line 24"
call :has "%NW%\fault.log" "\src\io.c:29" "native: divide by zero shows io.c line 29"
call :has "%NW%\fault.log" "\src\io.c:34" "native: timeout shows io.c line 34"
rem With optimization the line can move a little, so only the file name is checked (3 rows, each on stderr and stdout).
copy /y "%NW%\driver\timeout.cfg" "%NW%\driver\faultopt2.cfg" >nul
>> "%NW%\driver\faultopt2.cfg" echo native_opt = 2
"%RS%" run --cfg "%NW%\driver\faultopt2.cfg" "%NW%\csv\native_fault.csv" -o "%NW%\out_faultopt2" > "%NW%\faultopt2.log" 2>&1
"%HAS%" /l /c:"\src\io.c:" "%NW%\faultopt2.log" > "%NW%\faultopt2_loc.log"
set "LOC_N=0"
for /f "usebackq delims=" %%L in ("%NW%\faultopt2_loc.log") do set /a LOC_N+=1 >nul
if "%LOC_N%"=="6" (call :ok "native: optimization 2 shows io.c for all three fault rows") else (call :ng "native: optimization 2 shows io.c for all three fault rows: got %LOC_N% lines" & type "%NW%\faultopt2.log")
rem Without llvm-symbolizer the rows are still NG and the message names the tool and the setting.
copy /y "%NW%\driver\timeout.cfg" "%NW%\driver\nosym.cfg" >nul
>> "%NW%\driver\nosym.cfg" echo native_symbolizer = no_such_symbolizer_for_test
"%RS%" run --cfg "%NW%\driver\nosym.cfg" "%NW%\csv\native_fault.csv" -o "%NW%\out_nosym" > "%NW%\nosym.log" 2>&1
call :has "%NW%\nosym.log" "OK 5, NG 3, NO Check 0" "native: without llvm-symbolizer the fault rows are NG"
call :has "%NW%\nosym.log" "llvm-symbolizer" "native: without llvm-symbolizer the message names it"
call :has "%NW%\nosym.log" "native_symbolizer" "native: without llvm-symbolizer the message names the setting"
"%RS%" info --cfg "%NW%\driver\timeout.cfg" > "%NW%\timeout_info.log" 2>&1
call :has "%NW%\timeout_info.log" "native timeout_ms=300" "native: info shows timeout"
call :has "%NW%\timeout_info.log" "llvm-symbolizer" "native: info shows the symbolizer"
copy /y "%NC%" "%NW%\driver\badtimeout.cfg" >nul
>> "%NW%\driver\badtimeout.cfg" echo native_timeout_ms = abc
call :native_err "native: invalid timeout is an error" "native_timeout_ms" badtimeout.cfg
rem no fallback: a missing compiler and a compile error are errors (exit code 2)
copy /y "%NC%" "%NW%\driver\nocc.cfg" >nul
>> "%NW%\driver\nocc.cfg" echo native_cc = no_such_clang_for_test
call :native_err "native: a missing compiler is an error" "no_such_clang_for_test" nocc.cfg
> "%NW%\driver\zz_bad.c" echo #error "compile error for the test"
call :native_err "native: a compile error is an error and names the file" "zz_bad.c" sim.cfg
del "%NW%\driver\zz_bad.c"
rem the same sim.cfg with --exec interp goes to the RL78 backend, which needs a load module
"%RS%" info --cfg "%NC%" --exec interp > "%NW%\interp.log" 2>&1
set RC=%ERRORLEVEL%
"%HAS%" /l /c:"--abs" "%NW%\interp.log" >nul
if errorlevel 1 (call :ng "native: --exec interp overrides exec = native") else if "%RC%"=="2" (call :ok "native: --exec interp overrides exec = native") else (call :ng "native: --exec interp: exit code 2, got %RC%")

echo == startup diagnostics and skip poke
"%WORK%\mkabs.exe" startup "%WORK%\startup.abs"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mkabs startup") else (call :ng "mkabs startup")
"%RS%" info --abs "%WORK%\startup.abs" --cfg "%R%\cfg\startup_limit.cfg" --trace-startup --trace-head 1 --trace-tail 1 > "%WORK%\trace.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "SFR wait exits 2") else (call :ng "SFR wait exits 2")
call :has "%WORK%\trace.log" "wait" "trace stopped function"
call :has "%WORK%\trace.log" "boot" "trace call history and caller"
call :has "%WORK%\trace.log" "0xFFFA2" "trace SFR address"
call :has "%WORK%\trace.log" "0xF00A8" "trace extended SFR address"
call :has "%WORK%\trace.log" "SP" "trace registers"
"%RS%" info --abs "%WORK%\startup.abs" --cfg "%R%\cfg\startup_limit.cfg" > "%WORK%\plain.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "plain startup failure exits 2") else (call :ng "plain startup failure exits 2")
call :has "%WORK%\plain.log" "trace" "plain failure offers diagnostics"
call :startup_ok poke
call :startup_ok skip
call :run "CCRL skip byte word long nested and poke snapshot" 0 "OK 5, NG 0" --abs "%TP%" --cfg "%R%\cfg\skip_ccrl.cfg" "%R%\csv\skip_ccrl.csv"
call :run "CA78K0R skip byte word long" 0 "OK 3, NG 0" --abs "%WORK%\test.lmf" --cfg "%R%\cfg\skip_ca.cfg" "%R%\csv\skip_ca.csv"
call :run "LMF const arguments and returns" 0 "OK 5" --abs "%WORK%\test.lmf" "%R%\csv\const_lmf.csv"
for %%K in (dwarf2 dwarf3 dwarf4) do call :const_calls "%%K"
for %%N in (constant const_pointer cycle@constant) do call :const_reject "%WORK%\test.lmf" cycle "%%N"
> "%WORK%\const_ok.csv" echo mod,cycle,const pointer target,2,0
>> "%WORK%\const_ok.csv" echo #COMMENT,0xFE03E:2,const_pointer[0]
>> "%WORK%\const_ok.csv" echo ,0xE004,1
call :run "LMF allows const_pointer[0]" 0 "NG 0" --abs "%WORK%\test.lmf" "%WORK%\const_ok.csv"
> "%WORK%\const_skip.cfg" echo skip = constbyte:231
> "%WORK%\const_skip.csv" echo mod,constbyte,const skip,0,1
>> "%WORK%\const_skip.csv" echo #COMMENT,constbyte@@
>> "%WORK%\const_skip.csv" echo ,231
call :run "LMF const return skip" 0 "OK 1" --abs "%WORK%\test.lmf" --cfg "%WORK%\const_skip.cfg" "%WORK%\const_skip.csv"

call :startup_bad "skip = missing_function"
call :startup_bad "skip = read_io:garbage"
call :startup_bad "skip = read_io:0x10000"
call :startup_bad "skip = f_ll:1"
call :startup_bad "poke = bad:1"
call :startup_bad "poke = 0x100000:1"
call :startup_bad "poke = 0xFFFA2:bad"
call :startup_bad "poke = 0xFFFA2:256"
call :startup_bad "poke = 0xFFFFF:1:2"
call :startup_bad "poke = 0xFFFA2:1:0"
call :startup_bad "poke = 0xFFFA2:1:5"
call :startup_bad "poke = 0xFFFA2:1:1:2"
> "%WORK%\bad_startup.cfg" echo stub = read_io:AMSTB_read_io
>> "%WORK%\bad_startup.cfg" echo skip = read_io
"%RS%" info --abs "%TP%" --cfg "%WORK%\bad_startup.cfg" > "%WORK%\trace.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "stub skip conflict exits 2") else (call :ng "stub skip conflict exits 2")
call :run "trace startup retains existing ABI 43 rows" 0 "OK 43, NG 0" --abs "%TP%" --trace-startup "%R%\csv\synth_call.csv" "%R%\csv\synth_types.csv"

echo == ROM checksum and opcode FFH
"%WORK%\mkabs.exe" checksum "%WORK%\checksum.abs"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mkabs checksum") else (call :ng "mkabs checksum")
"%WORK%\mklmf.exe" checksum "%WORK%\checksum.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf checksum") else (call :ng "mklmf checksum")
call :rom_check ccrl checksum.abs 0x13D plain ""
call :rom_check ccrl checksum.abs 0x13D trace --trace-startup
call :rom_check ca checksum.lmf 0x10E plain ""
call :rom_check ca checksum.lmf 0x10E trace --trace-startup
fc /b "%WORK%\count_ccrl.plain" "%WORK%\count_ccrl.trace" >nul
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "CCRL plain trace equal counts") else (call :ng "CCRL plain trace equal counts")
fc /b "%WORK%\count_ca.plain" "%WORK%\count_ca.trace" >nul
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "CA plain trace equal counts") else (call :ng "CA plain trace equal counts")

echo == Boundary startup comparison and LMF groups
"%WORK%\mkabs.exe" boundary "%WORK%\boundary.abs"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mkabs boundary") else (call :ng "mkabs boundary")
> "%WORK%\boundary.cfg" echo init_end = boundary_end
>> "%WORK%\boundary.cfg" echo stub = boundary_end:boundary_short
>> "%WORK%\boundary.cfg" echo skip = boundary_short
"%RS%" info --abs "%WORK%\boundary.abs" --cfg "%WORK%\boundary.cfg" --trace-startup > "%WORK%\boundary.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "boundary stops before FFH") else (call :ng "boundary stops before FFH")
call :has "%WORK%\boundary.log" "MATCH" "boundary both paths match"
"%WORK%\mklmf.exe" diagnostics "%WORK%\diag.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf diagnostics") else (call :ng "mklmf diagnostics")
"%RS%" info --abs "%WORK%\diag.lmf" > "%WORK%\diag.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "LMF diagnostics load") else (call :ng "LMF diagnostics load")
call :has "%WORK%\diag.log" "0x55 0xDEAD 0x0000 normal 2" "LMF grouped repeated combinations"
call :has "%WORK%\diag.log" "0x55 0xDEAD 0x0000 absolute 1" "LMF absolute combination"
"%HAS%" /l /c:"unknown" "%WORK%\diag.log" >nul
if errorlevel 1 (call :ok "LMF default hides names") else (call :ng "LMF default hides names")
"%RS%" info --abs "%WORK%\diag.lmf" -v -v > "%WORK%\diag_names.log" 2>&1
call :has "%WORK%\diag_names.log" "unknown" "LMF named examples explicit"
call :run "header names and modi real" 0 "OK 2, NG 0" --abs "%WORK%\test.lmf" --cfg "%T%\defines\real.cfg" "%T%\defines\real.csv"
"%RS%" info --abs "%WORK%\test.lmf" --cfg "%T%\defines\real.cfg" > "%WORK%\header.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "header info real") else (call :ng "header info real")
call :has "%WORK%\header.log" "defines files" "recursive header scan real"
call :run "var and varfile real" 0 "OK 1, NG 0" --abs "%WORK%\test.lmf" --cfg "%T%\defines\var_real.cfg" "%T%\defines\var_real.csv"

echo == defines: compile switches (real)
call :rcond_cfg
call :rcond_list "%WORK%\test.lmf"
call :has "%WORK%\cond_list.log" "0xFF964 COND_REG" "cond real: no switch takes the else branch"
call :has "%WORK%\cond_list.log" "0xFF97E COND_K0R" "cond real: LMF defines the CA78K0R implicit macros"
call :has "%WORK%\cond_info.log" "cdef implicit" "cond real: info shows the macro counts"
>> "%WORK%\cond.cfg" echo cdef = PRODUCT=2
>> "%WORK%\cond.cfg" echo cundef = __K0R__
call :rcond_list "%WORK%\test.lmf"
call :has "%WORK%\cond_list.log" "0xFF962 COND_REG" "cond real: cdef PRODUCT 2"
call :has "%WORK%\cond_list.log" "0xFF97F COND_K0R" "cond real: cundef removes an implicit macro"
call :run "cond real: 16-bit typedef and struct member" 0 "OK 1, NG 0" --abs "%WORK%\test.lmf" --cfg "%WORK%\cond.cfg" "%T%\defines\cond_real.csv"
call :rcond_cfg
>> "%WORK%\cond.cfg" echo mtpj = %T%\defines\cond\cond_test.mtpj
call :rcond_list "%WORK%\test.lmf"
call :has "%WORK%\cond_list.log" "0xFF960 COND_REG" "cond real: mtpj macro definitions"
call :has "%WORK%\cond_info.log" "MTPJ_ONLY" "cond real: info lists the mtpj macros"
call :rcond_cfg
call :rcond_list "%TP%"
call :has "%WORK%\cond_list.log" "0xFF97F COND_K0R" "cond real: ELF does not define the CA78K0R macros"
call :rcond_cfg
>> "%WORK%\cond.cfg" echo mtpj = %T%\ca78k0r_probe\ca78k0r_probe.mtpj
call :rcond_list "%PROBE%"
call :has "%WORK%\cond_info.log" "mtpj macro definitions" "cond real: probe project file has no macro definitions"

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
"%HAS%" /l /c:"%~2" "%~1" >nul
if errorlevel 1 (call :ng "%~3") else (call :ok "%~3")
exit /b 0

rem :cfg_err name cfg-file expected_text  (sim info exits 2 with the text)
:cfg_err
"%RS%" info --abs "%TP%" --cfg "%R%\cfg\%~2" > "%WORK%\r.log" 2>&1
set RC=%ERRORLEVEL%
"%HAS%" /l /c:"%~3" "%WORK%\r.log" >nul
if errorlevel 1 (call :ng "%~1: message contains %~3") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
exit /b 0

rem :native_err name expected_text cfg-file-in-driver-folder
:native_err
"%RS%" run --cfg "%NW%\driver\%~3" "%NW%\csv\native_basic.csv" -o "%NW%\out_err" > "%NW%\err.log" 2>&1
set RC=%ERRORLEVEL%
"%HAS%" /l /c:"%~2" "%NW%\err.log" >nul
if errorlevel 1 (call :ng "%~1" & type "%NW%\err.log") else if "%RC%"=="2" (call :ok "%~1") else (call :ng "%~1: exit code 2, got %RC%")
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
"%HAS%" /l /c:"%RTXT%" "%WORK%\r.log" >nul
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
powershell -NoProfile -Command "$s=Get-Content -Encoding UTF8 -Raw -LiteralPath '%WORK%\fault.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 0 -or $s.step -ne 0 -or $s.running -ne $false -or $s.stepsLeft -ne 0 -or $s.needReset -ne $true -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 failed step stops without advancing") else (call :ok "GUI %~1 failed step stops without advancing")
call :has "%WORK%\fault.json" "PC" "GUI %~1 error has PC"
call :has "%WORK%\fault.json" "%~2" "GUI %~1 error has instruction details"
curl -s "%U%/api/export.csv" > "%WORK%\fault.csv"
powershell -NoProfile -Command "if (@(Get-Content -Encoding UTF8 -LiteralPath '%WORK%\fault.csv').Count -ne 2) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 export has no data rows") else (call :ok "GUI %~1 export has no data rows")
> "%WORK%\fault_step.json" echo {"cmd":"step","steps":3}
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\fault_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "GUI %~1 step rejected until reset") else (call :ng "GUI %~1 step rejected until reset")
curl -s "%U%/api/info" > "%WORK%\fault_info.json"
powershell -NoProfile -Command "$s=(Get-Content -Encoding UTF8 -Raw '%WORK%\fault_info.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.step -ne 0 -or $s.needReset -ne $true) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 rejected step leaves state unchanged") else (call :ok "GUI %~1 rejected step leaves state unchanged")
> "%WORK%\fault_reset.json" echo {"cmd":"reset","func":"TEST_cycle"}
curl -s -X POST --data-binary "@%WORK%\fault_reset.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Encoding UTF8 -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.error -ne '' -or $s.needReset -ne $false) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 reset clears error") else (call :ok "GUI %~1 reset clears error")
> "%WORK%\fault_watch.json" echo {"names":["TEST_cycle_cnt"]}
curl -s -X POST --data-binary "@%WORK%\fault_watch.json" "%U%/api/watch" >nul
curl -s -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Encoding UTF8 -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 3 -or $s.error -ne '' -or $s.needReset -ne $false) {exit 1}"
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
powershell -NoProfile -Command "$s=(Get-Content -Encoding UTF8 -Raw -LiteralPath '%WORK%\fault_info.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.playback -ne $false -or $s.running -ne $false -or $s.step -ne 0 -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 failed playback stops without advancing") else (call :ok "GUI %~1 failed playback stops without advancing")
exit /b 0

:nofunc_reject
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nofunc_error.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\nofunc_%1.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "GUI unset function %1 HTTP 400") else (call :ng "GUI unset function %1 HTTP 400")
call :has "%WORK%\nofunc_error.json" "func" "GUI unset function explains func"
curl -s "%U%/api/info" > "%WORK%\nofunc_after.json"
fc /b "%WORK%\nofunc_before.json" "%WORK%\nofunc_after.json" >nul
if errorlevel 1 (call :ng "GUI unset function %1 preserves state") else (call :ok "GUI unset function %1 preserves state")
exit /b 0

:lmf_bad
"%WORK%\mklmf.exe" %1 "%WORK%\bad.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf %1") else (call :ng "mklmf %1")
"%RS%" info --abs "%WORK%\bad.lmf" > "%WORK%\lmf.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "LMF malformed %1 rejected") else (call :ng "LMF malformed %1 rejected")
exit /b 0

:startup_ok
"%RS%" info --abs "%WORK%\startup.abs" --cfg "%R%\cfg\startup_%~1.cfg" --trace-startup > "%WORK%\trace.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "%~1 allows startup") else (call :ng "%~1 allows startup")
call :has "%WORK%\trace.log" "%~1" "%~1 settings shown"
exit /b 0

:startup_bad
> "%WORK%\bad_startup.cfg" echo %~1
"%RS%" info --abs "%TP%" --cfg "%WORK%\bad_startup.cfg" > "%WORK%\trace.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "bad startup setting rejected %~1") else (call :ng "bad startup setting rejected %~1")
exit /b 0

:lmf_badtag
"%WORK%\mklmf.exe" %1 "%WORK%\badtag.lmf"
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "mklmf %1") else (call :ng "mklmf %1")
"%WORK%\lmf_types.exe" "%WORK%\badtag.lmf" bad > "%WORK%\tag.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "malformed %1 becomes unknown") else (call :ng "malformed %1 becomes unknown")
exit /b 0

:rom_check
"%RS%" info --abs "%WORK%\%~2" --cfg "%R%\cfg\rom_check_%~1.cfg" %~5 > "%WORK%\rom_%~1.%~4.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="0" (call :ok "ROM checksum %~1 %~4 reaches main") else (call :ng "ROM checksum %~1 %~4 reaches main")
call :has "%WORK%\rom_%~1.%~4.log" "main" "ROM checksum init end"
"%HAS%" /l /c:"Hz" "%WORK%\rom_%~1.%~4.log" > "%WORK%\count_%~1.%~4"
copy /y "%R%\cfg\rom_check_%~1.cfg" "%WORK%\corrupt.cfg" >nul
>> "%WORK%\corrupt.cfg" echo poke = %~3:0
"%RS%" info --abs "%WORK%\%~2" --cfg "%WORK%\corrupt.cfg" %~5 > "%WORK%\reset_%~1.%~4.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "ROM checksum %~1 %~4 rejects corruption") else (call :ng "ROM checksum %~1 %~4 rejects corruption")
call :has "%WORK%\reset_%~1.%~4.log" "FFH" "ROM checksum FFH diagnostic"
call :has "%WORK%\reset_%~1.%~4.log" "original bytes" "ROM checksum original bytes"
call :has "%WORK%\reset_%~1.%~4.log" "breakpoint" "ROM checksum breakpoint context"
exit /b 0

:rcond_cfg
> "%WORK%\cond.cfg" echo defines = %T%\defines\cond
exit /b 0

rem :rcond_list load-module
:rcond_list
"%RS%" list --abs "%~1" --cfg "%WORK%\cond.cfg" COND_ > "%WORK%\cond_list.log" 2>&1
"%RS%" info --abs "%~1" --cfg "%WORK%\cond.cfg" > "%WORK%\cond_info.log" 2>&1
exit /b 0

:dwarf_const
> "%WORK%\dwarf_const.csv" echo mod,f_bits,const path,1,0
>> "%WORK%\dwarf_const.csv" echo #COMMENT,%~2
>> "%WORK%\dwarf_const.csv" echo ,1
"%SIM_RL78%" run --abs "%WORK%\tp_%~1.abs" "%WORK%\dwarf_const.csv" -o "%WORK%\dwarf_const" > "%WORK%\dwarf_const.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "%~1 const %~2 rejected") else (call :ng "%~1 const %~2 rejected")
exit /b

:const_calls
call :run "%~1 const arguments and returns" 0 "OK 10" --abs "%WORK%\tp_%~1.abs" "%R%\csv\const_dwarf.csv"
for %%N in (g_const_pointer g_const_target[0] f_bits@constant f_bits@local_const) do call :const_reject "%WORK%\tp_%~1.abs" f_bits "%%N"
> "%WORK%\const_ok.csv" echo mod,f_bits,const pointer target,3,0
>> "%WORK%\const_ok.csv" echo #COMMENT,0xFE190:2,0xFE192:2,g_const_pointer[0]
>> "%WORK%\const_ok.csv" echo ,0xE11A,0xE11A,1
call :run "%~1 allows g_const_pointer[0]" 0 "NG 0" --abs "%WORK%\tp_%~1.abs" "%WORK%\const_ok.csv"
> "%WORK%\const_skip.cfg" echo skip = constret:231
> "%WORK%\const_skip.csv" echo mod,constret,const skip,0,1
>> "%WORK%\const_skip.csv" echo #COMMENT,constret@@
>> "%WORK%\const_skip.csv" echo ,231
call :run "%~1 const return skip" 0 "OK 1" --abs "%WORK%\tp_%~1.abs" --cfg "%WORK%\const_skip.cfg" "%WORK%\const_skip.csv"
exit /b

:const_reject
if "%~2"=="cycle" goto const_reject_lmf
> "%WORK%\const_reject.csv" echo mod,%~2,const protection,3,0
>> "%WORK%\const_reject.csv" echo #COMMENT,0xFE190:2,0xFE192:2,%~3
>> "%WORK%\const_reject.csv" echo ,0xE11A,0xE11A,1
goto const_reject_run
:const_reject_lmf
> "%WORK%\const_reject.csv" echo mod,%~2,const protection,2,0
>> "%WORK%\const_reject.csv" echo #COMMENT,0xFE03E:2,%~3
>> "%WORK%\const_reject.csv" echo ,0xE004,1
:const_reject_run
"%SIM_RL78%" run --abs "%~1" "%WORK%\const_reject.csv" -o "%WORK%\const_out" > "%WORK%\const.log" 2>&1
set RC=%ERRORLEVEL%
if "%RC%"=="2" (call :ok "const %~3 rejected") else (call :ng "const %~3 rejected")
exit /b
