@echo off
rem build.bat - Windows build script (clang only; no CMake / Python / make)
rem
rem   build.bat [outdir]         build sim.exe with the real RL78 backend (default outdir: build\sim)
rem   build.bat mock [outdir]    build sim.exe with the mock backend, for testing the common part (default: build\mock)
rem
rem Needs LLVM clang for Windows (e.g. 18.1.8) plus the MSVC C runtime and Windows SDK.
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
    if defined LLVM_DIR set "CC=%LLVM_DIR%\bin\clang.exe"
)
"%CC%" --version >nul 2>nul
if errorlevel 1 (
    echo build.bat: clang not found. Add LLVM\bin to PATH, or set LLVM_DIR or CC. 1>&2
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
set "COMMON="%S%\sim_main.c" "%S%\sim_util.c" "%S%\sim_json.c" "%S%\sim_platform.c" "%S%\sim_expr.c" "%S%\sim_csv.c" "%S%\sim_gui.c" "%S%\sim_cfg.c""
if "%MODE%"=="mock" (
    set "SRCS=%COMMON% "%ROOT%\runtime\backends\mock\mock_backend.c""
    set "INC=-I"%ROOT%\runtime\include" -I"%S%""
) else (
    set "SRCS=%COMMON% "%B%\rl78_backend.c" "%B%\rl78_elf.c" "%B%\rl78_dwarf.c" "%B%\rl78_map.c" "%C%\rl78_exec.c" "%C%\rl78_sys.c" "%C%\rl78_disasm.c""
    set "INC=-I"%ROOT%\runtime\include" -I"%S%" -I"%ROOT%\rl78\include" -I"%B%""
)

echo build.bat: building %OUT%\sim.exe with %CC% (backend: %MODE%)
"%CC%" -std=c11 -O2 -Wall -Wextra %INC% %SRCS% -o "%OUT%\sim.exe" -lws2_32
if errorlevel 1 (
    echo build.bat: compile failed. clang for Windows also needs the MSVC C runtime and the Windows SDK, e.g. Visual Studio Build Tools. 1>&2
    exit /b 1
)

if exist "%ROOT%\gui\dist\sim_gui.html" (
    copy /y "%ROOT%\gui\dist\sim_gui.html" "%OUT%\sim_gui.html" >nul
    echo build.bat: copied gui\dist\sim_gui.html
) else (
    if exist "%OUT%\sim_gui.html" del "%OUT%\sim_gui.html"
    echo build.bat: gui\dist\sim_gui.html not found, sim gui will show a GUI-not-built page
)
echo build.bat: done: %OUT%\sim.exe
exit /b 0
