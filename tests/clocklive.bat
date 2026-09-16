@echo off
REM ===========================================================================
REM  Runs the desktop clock live, without the tiler, and captures it off the
REM  screen: one PNG per style. See tests\clocklive.cpp.
REM  Output lands in tests\shots.
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
if not exist "%ROOT%\tests\build\ui" mkdir "%ROOT%\tests\build\ui"
if not exist "%ROOT%\tests\shots" mkdir "%ROOT%\tests\shots"

REM  The dialog templates live in the resource script; without them there is no
REM  settings window to draw.
pushd "%ROOT%\res"
rc.exe /nologo /fo "%ROOT%\tests\build\ui\app.res" app.rc
if errorlevel 1 ( popd & exit /b 1 )
popd

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\ui\\" ^
    "%ROOT%\tests\clocklive.cpp" ^
    "%ROOT%\src\settings.cpp" "%ROOT%\src\settings_keys.cpp" ^
    "%ROOT%\src\settings_search.cpp" "%ROOT%\src\theme.cpp" ^
    "%ROOT%\src\monitor.cpp" "%ROOT%\src\monpaint.cpp" "%ROOT%\src\montheme.cpp" ^
    "%ROOT%\src\clock.cpp" "%ROOT%\src\clockpaint.cpp" "%ROOT%\src\clocktheme.cpp" "%ROOT%\src\settings_clock.cpp" ^
    "%ROOT%\src\sysinfo.cpp" "%ROOT%\src\thermal.cpp" ^
    "%ROOT%\src\search.cpp" "%ROOT%\src\launcher.cpp" "%ROOT%\src\appicon.cpp" ^
    "%ROOT%\src\wm.cpp" "%ROOT%\src\layout.cpp" "%ROOT%\src\dragguide.cpp" "%ROOT%\src\moddrag.cpp" ^
    "%ROOT%\src\winutil.cpp" "%ROOT%\src\common.cpp" ^
    "%ROOT%\src\config.cpp" "%ROOT%\src\defaults.cpp" "%ROOT%\src\hotkeys.cpp" ^
    /Fe:"%ROOT%\tests\build\clocklive.exe" ^
    /link /SUBSYSTEM:CONSOLE "%ROOT%\tests\build\ui\app.res"
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\clocklive.exe" "%ROOT%\tests\shots"
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
