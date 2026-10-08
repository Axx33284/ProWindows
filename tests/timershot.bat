@echo off
REM ===========================================================================
REM  Screenshots the timer panel on its own - no window manager, so nothing on
REM  the desktop is moved. Output lands in tests\shots.
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
if not exist "%ROOT%\tests\build\ts" mkdir "%ROOT%\tests\build\ts"
if not exist "%ROOT%\tests\shots" mkdir "%ROOT%\tests\shots"

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\ts\\" ^
    "%ROOT%\tests\timershot.cpp" "%ROOT%\src\timer.cpp" "%ROOT%\src\clockpanel_paint.cpp" "%ROOT%\src\alarm.cpp" ^
    "%ROOT%\src\theme.cpp" "%ROOT%\src\winutil.cpp" "%ROOT%\src\common.cpp" ^
    "%ROOT%\src\config.cpp" "%ROOT%\src\defaults.cpp" "%ROOT%\src\montheme.cpp" "%ROOT%\src\clocktheme.cpp" ^
    /Fe:"%ROOT%\tests\build\timershot.exe" ^
    /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\timershot.exe" "%ROOT%\tests\shots"
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
