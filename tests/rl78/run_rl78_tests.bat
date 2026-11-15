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

echo == LMF CA78K0R
"%CC%" -std=c11 -O2 -Wall -Wextra -o "%WORK%\mklmf.exe" "%T%\tools\mklmf.c" > "%WORK%\mklmf.log" 2>&1
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
"%CC%" -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I"%B%" -I"%ROOT%\runtime\include" -I"%ROOT%\runtime\src" "%T%\tools\check_lmf_types.c" "%B%\rl78_lmf.c" "%B%\rl78_elf.c" "%B%\rl78_dwarf.c" "%B%\rl78_map.c" "%ROOT%\runtime\src\sim_util.c" -o "%WORK%\lmf_types.exe" > "%WORK%\types_build.log" 2>&1
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
powershell -NoProfile -Command "$s=(Get-Content -Raw -LiteralPath '%WORK%\nofunc_after.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.step -ne 0) {exit 1}"
if errorlevel 1 (call :ng "GUI unset function stays at step zero") else (call :ok "GUI unset function stays at step zero")
call :has "%WORK%\nofunc.log" "func" "GUI unset function startup explains func"
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nofunc_error.json" -w "%%{http_code}" "%U%/api/export.csv"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "GUI unset function export HTTP 400") else (call :ng "GUI unset function export HTTP 400")
call :has "%WORK%\nofunc_error.json" "func" "GUI unset function export explains func"
> "%WORK%\nofunc_set.json" echo {"func":"R_Config_ADC_Set_ADChannel","cmd":"step"}
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\nofunc_result.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\nofunc_set.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="200" (call :ok "GUI func and step together HTTP 200") else (call :ng "GUI func and step together HTTP 200")
curl -s "%U%/api/export.csv" > "%WORK%\nofunc.csv"
findstr /l /c:"step 1" "%WORK%\nofunc.csv" >nul
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
findstr /l /c:"unknown" "%WORK%\diag.log" >nul
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
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\fault.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 0 -or $s.step -ne 0 -or $s.running -ne $false -or $s.stepsLeft -ne 0 -or $s.needReset -ne $true -or !$s.error) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 failed step stops without advancing") else (call :ok "GUI %~1 failed step stops without advancing")
call :has "%WORK%\fault.json" "PC" "GUI %~1 error has PC"
call :has "%WORK%\fault.json" "%~2" "GUI %~1 error has instruction details"
curl -s "%U%/api/export.csv" > "%WORK%\fault.csv"
powershell -NoProfile -Command "if (@(Get-Content -LiteralPath '%WORK%\fault.csv').Count -ne 2) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 export has no data rows") else (call :ok "GUI %~1 export has no data rows")
> "%WORK%\fault_step.json" echo {"cmd":"step","steps":3}
set "CODE="
for /f "usebackq delims=" %%C in (`curl -s -o "%WORK%\fault_rejected.json" -w "%%{http_code}" -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control"`) do set "CODE=%%C"
if "%CODE%"=="400" (call :ok "GUI %~1 step rejected until reset") else (call :ng "GUI %~1 step rejected until reset")
curl -s "%U%/api/info" > "%WORK%\fault_info.json"
powershell -NoProfile -Command "$s=(Get-Content -Raw '%WORK%\fault_info.json' | ConvertFrom-Json).state; if ($null -eq $s -or $s.step -ne 0 -or $s.needReset -ne $true) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 rejected step leaves state unchanged") else (call :ok "GUI %~1 rejected step leaves state unchanged")
> "%WORK%\fault_reset.json" echo {"cmd":"reset","func":"TEST_cycle"}
curl -s -X POST --data-binary "@%WORK%\fault_reset.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.error -ne '' -or $s.needReset -ne $false) {exit 1}"
if errorlevel 1 (call :ng "GUI %~1 reset clears error") else (call :ok "GUI %~1 reset clears error")
> "%WORK%\fault_watch.json" echo {"names":["TEST_cycle_cnt"]}
curl -s -X POST --data-binary "@%WORK%\fault_watch.json" "%U%/api/watch" >nul
curl -s -X POST --data-binary "@%WORK%\fault_step.json" "%U%/api/control" > "%WORK%\recovered.json"
powershell -NoProfile -Command "$s=Get-Content -Raw -LiteralPath '%WORK%\recovered.json' | ConvertFrom-Json; if ($null -eq $s -or $s.stepsDone -ne 3 -or $s.error -ne '' -or $s.needReset -ne $false) {exit 1}"
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
findstr /l /c:"Hz" "%WORK%\rom_%~1.%~4.log" > "%WORK%\count_%~1.%~4"
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
