@echo off
rem tools\build_dist.bat - build sim.exe on Windows and replace dist\sim-windows-x64 with it
rem
rem   tools\build_dist.bat
rem
rem Needs MinGW-w64 gcc on PATH (the distributed sim.exe is built with gcc, static runtime).
rem The folder has the same contents as the CI artifact sim-windows-x64.
rem Messages are ASCII only so that cmd.exe parses this file the same on any code page.
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "NAME=sim-windows-x64"
set "OUT=%ROOT%\build\dist-stage\%NAME%"
set "DIST=%ROOT%\dist\%NAME%"

gcc --version >nul 2>nul
if errorlevel 1 (
    echo build_dist.bat: gcc not found. Put MinGW-w64 gcc on PATH. 1>&2
    exit /b 2
)
if exist "%ROOT%\build\dist-stage" rmdir /s /q "%ROOT%\build\dist-stage"
set "CC=gcc"
call "%ROOT%\build.bat" "%OUT%"
if errorlevel 1 exit /b 1
if not exist "%OUT%\sim_gui.html" (
    echo build_dist.bat: gui\dist\sim_gui.html is missing 1>&2
    exit /b 1
)
copy /y "%ROOT%\example\sim.cfg" "%OUT%\sim.cfg.example" >nul
copy /y "%ROOT%\example\native_ram_clear.c" "%OUT%\native_ram_clear.c" >nul
copy /y "%ROOT%\README.md" "%OUT%\" >nul

rem replace the whole folder so that nothing from the previous build is left (only after the build succeeded)
if exist "%DIST%" rmdir /s /q "%DIST%"
xcopy /e /i /q /y "%OUT%" "%DIST%" >nul
if errorlevel 1 (
    echo build_dist.bat: cannot copy to %DIST% 1>&2
    exit /b 1
)
echo build_dist.bat: done: %DIST%
gcc --version | findstr /n "^" | findstr /b "1:"
exit /b 0
