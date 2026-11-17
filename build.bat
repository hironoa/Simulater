@echo off
rem build.bat - Windows build script (clang or MinGW-w64 gcc; no CMake / Python / make)
rem
rem   build.bat [outdir]         build sim.exe with the real RL78 backend (default outdir: build\sim)
rem   build.bat mock [outdir]    build sim.exe with the mock backend, for testing the common part (default: build\mock)
rem
rem Default: LLVM clang plus the MSVC C runtime and Windows SDK.
rem Set CC=gcc to use MinGW-w64 gcc (static runtime, no extra runtime DLLs).
rem Messages in this file are ASCII only so that cmd.exe parses it the same on any code page.
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "MODE=real"
if /i "%~1"=="mock" (
    set "MODE=mock"
    shift
)
set "OUT=%~1"
if "%OUT%"=="" if "%MODE%"=="mock" set "OUT=%ROOT%\build\mock"
if "%OUT%"=="" set "OUT=%ROOT%\build\sim"

if not defined CC set "CC=clang"
"%CC%" --version >nul 2>nul
if errorlevel 1 (
    for %%C in ("%CC%") do if /i "%%~nC"=="clang" if defined LLVM_DIR set "CC=%LLVM_DIR%\bin\clang.exe"
)
set "GCC_FLAGS="
set "LIBS=-lws2_32"
for %%C in ("%CC%") do if /i "%%~nC"=="gcc" (
    set "GCC_FLAGS=-static -D__USE_MINGW_ANSI_STDIO=1"
    set "LIBS=-lws2_32 -lshell32"
)
"%CC%" --version >nul 2>nul
if errorlevel 1 (
    echo build.bat: compiler %CC% not found. Add its bin directory to PATH, or set LLVM_DIR or CC. 1>&2
    exit /b 2
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%" (
    echo build.bat: cannot create %OUT% 1>&2
    exit /b 2
)

set "S=%ROOT%\runtime\src"
set "B=%ROOT%\runtime\backends\rl78"
set "C=%ROOT%\rl78\src"
set "COMMON="%S%\sim_main.c" "%S%\sim_util.c" "%S%\sim_json.c" "%S%\sim_platform.c" "%S%\sim_expr.c" "%S%\sim_csv.c" "%S%\sim_gui.c" "%S%\sim_cfg.c" "%S%\sim_defines.c" "%S%\sim_ppexpr.c" "%S%\sim_mtpj.c" "%S%\sim_cc.c""
if "%MODE%"=="mock" (
    set "SRCS=%COMMON% "%ROOT%\runtime\backends\mock\mock_backend.c""
    set "INC=-I"%ROOT%\runtime\include" -I"%S%""
) else (
    set "SRCS=%COMMON% "%B%\rl78_backend.c" "%B%\rl78_xl.c" "%B%\rl78_abi.c" "%B%\rl78_lmf.c" "%B%\rl78_sym.c" "%B%\rl78_elf.c" "%B%\rl78_dwarf.c" "%B%\rl78_map.c" "%C%\rl78_exec.c" "%C%\rl78_sys.c" "%C%\rl78_disasm.c" "%C%\rl78_xlate.c" "%ROOT%\runtime\backends\native\native_backend.c" "%ROOT%\runtime\backends\native\native_rewrite.c" "%ROOT%\runtime\backends\native\native_near.c" "%ROOT%\runtime\backends\native\native_coff.c" "%ROOT%\runtime\backends\native\native_layout.c""
    set "INC=-I"%ROOT%\runtime\include" -I"%S%" -I"%ROOT%\rl78\include" -I"%B%" -I"%ROOT%\runtime\backends\native""
)

echo build.bat: building %OUT%\sim.exe with %CC% (backend: %MODE%)
rem -fno-unroll-loops: clang 20 at plain -O2 makes the instruction loop about 10x slower on x86-64
"%CC%" %GCC_FLAGS% -std=c11 -O2 -fno-unroll-loops -Wall -Wextra %INC% %SRCS% -o "%OUT%\sim.exe" %LIBS%
if errorlevel 1 (
    echo build.bat: compile failed. Use MinGW-w64 gcc, or clang with the MSVC C runtime and Windows SDK. 1>&2
    exit /b 1
)

if exist "%ROOT%\gui\dist\sim_gui.html" (
    copy /y "%ROOT%\gui\dist\sim_gui.html" "%OUT%\sim_gui.html" >nul
    echo build.bat: copied gui\dist\sim_gui.html
) else (
    if exist "%OUT%\sim_gui.html" del "%OUT%\sim_gui.html"
    echo build.bat: gui\dist\sim_gui.html not found, sim gui will show a GUI-not-built page
)
if not "%MODE%"=="mock" (
    rem sources that exec = xlate hands to clang
    if not exist "%OUT%\xlate" mkdir "%OUT%\xlate"
    copy /y "%ROOT%\rl78\include\rl78.h" "%OUT%\xlate\" >nul
    copy /y "%ROOT%\rl78\src\rl78_internal.h" "%OUT%\xlate\" >nul
    copy /y "%ROOT%\rl78\src\rl78_exec_defs.h" "%OUT%\xlate\" >nul
    copy /y "%ROOT%\rl78\src\rl78_exec_ops.inc" "%OUT%\xlate\" >nul
    copy /y "%ROOT%\rl78\src\rl78_xlate_rt.h" "%OUT%\xlate\" >nul
    rem files that exec = native hands to clang
    if not exist "%OUT%\native" mkdir "%OUT%\native"
    copy /y "%ROOT%\runtime\backends\native\rt\sim_native_pre.h" "%OUT%\native\" >nul
    copy /y "%ROOT%\runtime\backends\native\rt\sim_native_rt.c" "%OUT%\native\" >nul
)
echo build.bat: done: %OUT%\sim.exe
exit /b 0
