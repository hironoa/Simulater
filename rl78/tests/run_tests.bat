@echo off
rem Build and run all RL78 core tests and the benchmark (Windows, clang / MinGW-w64 gcc).
rem   CC            compiler (default: clang)
rem   RL78_GNU_BIN  optional: directory with the GNU rl78-elf-* tools -> difftest
rem   BENCH_INSNS   benchmark instructions (default 300000000)
rem   BENCH_MIN     optional: fail if the benchmark is below this many M insns/s
rem Exit code is non-zero if any check fails.
setlocal
set HERE=%~dp0
set ROOT=%HERE%..
set BUILD=%ROOT%\build
if "%CC%"=="" set CC=clang
if "%BENCH_INSNS%"=="" set BENCH_INSNS=300000000
set CFLAGS=-std=c11 -O2 -fno-unroll-loops -Wall -Wextra -Wpedantic -Wshadow -Werror -D_CRT_SECURE_NO_WARNINGS
for %%C in ("%CC%") do if /i "%%~nC"=="gcc" set "CFLAGS=%CFLAGS% -D__USE_MINGW_ANSI_STDIO=1"
if not exist "%BUILD%" mkdir "%BUILD%"
set STATUS=0

echo == build (%CC% %CFLAGS%)
"%CC%" %CFLAGS% -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%test_main.c" "%HERE%t_decode.c" "%HERE%t_manual.c" "%HERE%t_alu.c" "%HERE%t_insn.c" "%HERE%t_prog.c" "%HERE%t_api.c" "%HERE%t_regress.c" -o "%BUILD%\rl78_tests.exe"
if errorlevel 1 exit /b 1
"%CC%" %CFLAGS% -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%bench.c" -o "%BUILD%\rl78_bench.exe"
if errorlevel 1 exit /b 1
"%CC%" %CFLAGS% -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%difftest\rl78dt.c" -o "%BUILD%\rl78dt.exe"
if errorlevel 1 exit /b 1

echo == tests
"%BUILD%\rl78_tests.exe"
if errorlevel 1 set STATUS=1

echo == benchmark
"%BUILD%\rl78_bench.exe" %BENCH_INSNS% %BENCH_MIN%
if errorlevel 1 set STATUS=1

echo == translation to C ^(needs clang^)
set "XLCC=%XLCC%"
if "%XLCC%"=="" set "XLCC=clang"
"%XLCC%" --version >nul 2>nul
if errorlevel 1 if defined LLVM_DIR set "XLCC=%LLVM_DIR%\bin\clang.exe"
"%XLCC%" --version >nul 2>nul
if errorlevel 1 set "XLCC=C:\Program Files\LLVM\bin\clang.exe"
"%XLCC%" --version >nul 2>nul
if errorlevel 1 (
  echo translation: FAIL ^(clang not found; set XLCC^)
  set STATUS=1
  goto after_xl
)
if not exist "%BUILD%\xl" mkdir "%BUILD%\xl"
"%CC%" %CFLAGS% -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%xlgen.c" -o "%BUILD%\xlgen.exe"
if errorlevel 1 exit /b 1
"%CC%" %CFLAGS% -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%xlrun.c" -o "%BUILD%\xlrun.exe"
if errorlevel 1 exit /b 1
"%BUILD%\xlgen.exe" "%BUILD%\xl"
if errorlevel 1 set STATUS=1
rem freestanding DLL: no C runtime and no Windows SDK (only clang and lld-link from the LLVM package)
"%XLCC%" -O2 -fno-unroll-loops -ffreestanding -nostdlib -shared -fuse-ld=lld -Wl,/noentry -mno-stack-arg-probe -DRL78_XL_DLL -I"%ROOT%\include" -I"%ROOT%\src" "%BUILD%\xl\xl_unit.c" "%BUILD%\xl\xl_unit2.c" "%BUILD%\xl\xl_main.c" -o "%BUILD%\xl\xl.dll"
if errorlevel 1 (
  echo translation: FAIL ^(cannot build the DLL^)
  set STATUS=1
  goto after_xl
)
"%BUILD%\xlrun.exe" "%BUILD%\xl" "%BUILD%\xl\xl.dll"
if errorlevel 1 set STATUS=1
rem the same again with every function cut into parts of a few instructions (XL_PART_INSNS in rl78_xlate.c)
echo == translation to C: functions cut into parts
if not exist "%BUILD%\xlp" mkdir "%BUILD%\xlp"
"%CC%" %CFLAGS% -DXL_PART_INSNS=4u -I"%ROOT%\include" -I"%HERE%." "%ROOT%\src\rl78_exec.c" "%ROOT%\src\rl78_sys.c" "%ROOT%\src\rl78_disasm.c" "%ROOT%\src\rl78_xlate.c" "%HERE%tasm.c" "%HERE%xlgen.c" -o "%BUILD%\xlgen_parts.exe"
if errorlevel 1 exit /b 1
"%BUILD%\xlgen_parts.exe" "%BUILD%\xlp"
if errorlevel 1 set STATUS=1
"%XLCC%" -O2 -fno-unroll-loops -ffreestanding -nostdlib -shared -fuse-ld=lld -Wl,/noentry -mno-stack-arg-probe -DRL78_XL_DLL -I"%ROOT%\include" -I"%ROOT%\src" "%BUILD%\xlp\xl_unit.c" "%BUILD%\xlp\xl_unit2.c" "%BUILD%\xlp\xl_main.c" -o "%BUILD%\xlp\xl.dll"
if errorlevel 1 (
  echo translation in parts: FAIL ^(cannot build the DLL^)
  set STATUS=1
  goto after_xl
)
"%BUILD%\xlrun.exe" "%BUILD%\xlp" "%BUILD%\xlp\xl.dll" parts
if errorlevel 1 set STATUS=1
:after_xl

if "%RL78_GNU_BIN%"=="" (
  echo == difftest: SKIP ^(RL78_GNU_BIN not set^)
) else (
  echo == difftest vs GNU rl78-elf-run
  set RL78DT=%BUILD%\rl78dt.exe
  python "%HERE%difftest\difftest.py" %DIFFTEST_ARGS%
  if errorlevel 1 set STATUS=1
)

if "%STATUS%"=="0" (echo RESULT: PASS) else (echo RESULT: FAIL)
exit /b %STATUS%
