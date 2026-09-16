@echo off
REM ===========================================================================
REM  Builds and runs the search index on its own and prints what it found.
REM  Creates no window and launches nothing, so it is safe to run at any time.
REM
REM    searchprobe                 the shipped defaults
REM    searchprobe --no-drives     Program Files only, for comparison
REM    searchprobe --deep steam    helper exes too, and show what "steam" finds
REM ===========================================================================
setlocal enabledelayedexpansion
set ROOT=%~dp0..

if defined VSCMD_ARG_TGT_ARCH goto :haveToolchain

set VCVARS=
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo [ERROR] Could not find vcvars64.bat.
    exit /b 1
)
call "%VCVARS%" >nul

:haveToolchain
if not exist "%ROOT%\tests\build\sp" mkdir "%ROOT%\tests\build\sp"

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\sp\\" ^
    "%ROOT%\tests\searchprobe.cpp" ^
    "%ROOT%\src\search.cpp" "%ROOT%\src\common.cpp" ^
    "%ROOT%\src\config.cpp" "%ROOT%\src\defaults.cpp" ^
    "%ROOT%\src\winutil.cpp" "%ROOT%\src\montheme.cpp" "%ROOT%\src\clocktheme.cpp" ^
    /Fe:"%ROOT%\tests\build\searchprobe.exe" ^
    /link /SUBSYSTEM:CONSOLE ^
    user32.lib gdi32.lib advapi32.lib shell32.lib shlwapi.lib ole32.lib dwmapi.lib
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\searchprobe.exe" %*
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
