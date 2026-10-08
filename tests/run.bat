@echo off
REM ===========================================================================
REM  ProWindows - layout geometry tests
REM  Compiles the pure part of the tiler (layout.cpp) with a console harness
REM  and asserts on the rectangles it produces. Touches nothing on the desktop.
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0..\
set OUT=%~dp0build

if not defined VSCMD_ARG_TGT_ARCH (
    set VCVARS=
    for %%P in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if not defined VCVARS if exist %%P set VCVARS=%%P
    )
    if not defined VCVARS (
        echo [ERROR] Could not find vcvars64.bat.
        exit /b 1
    )
    call !VCVARS! >nul
)

if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"

cl.exe /nologo /std:c++17 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
       /MT /GR- ^
       "%~dp0layout_test.cpp" ^
       "%ROOT%src\layout.cpp" ^
       "%ROOT%src\common.cpp" ^
       "%ROOT%src\config.cpp" ^
       "%ROOT%src\defaults.cpp" ^
       "%ROOT%src\winutil.cpp" ^
       "%ROOT%src\montheme.cpp" "%ROOT%src\clocktheme.cpp" ^
       /Fe:layout_test.exe ^
       /link /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib dwmapi.lib ^
       advapi32.lib shlwapi.lib

if errorlevel 1 (
    popd
    echo [ERROR] The tests did not compile.
    exit /b 1
)

echo.
REM Explicitly relative: NoDefaultCurrentDirectoryInExePath is set on some
REM machines, and then a bare name is not found even in the current directory.
.\layout_test.exe
set RESULT=%ERRORLEVEL%

REM The timer model: pure functions over a state, with the clock passed in.
cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
       /MT /GR- ^
       "%~dp0timer_test.cpp" ^
       "%ROOT%src\timer.cpp" "%ROOT%src\clockpanel_paint.cpp" "%ROOT%src\alarm.cpp" "%ROOT%src\theme.cpp" ^
       "%ROOT%src\winutil.cpp" "%ROOT%src\common.cpp" "%ROOT%src\config.cpp" ^
       "%ROOT%src\defaults.cpp" "%ROOT%src\montheme.cpp" "%ROOT%src\clocktheme.cpp" ^
       /Fe:timer_test.exe ^
       /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib ^
       dwmapi.lib advapi32.lib shlwapi.lib
if errorlevel 1 (
    popd
    echo [ERROR] The timer tests did not compile.
    exit /b 1
)
echo.
.\timer_test.exe
if errorlevel 1 set RESULT=1

REM The alarm model: local-time schedules over a fake zone, no machine zone needed.
cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
       /MT /GR- ^
       "%~dp0alarm_test.cpp" ^
       "%ROOT%src\timer.cpp" "%ROOT%src\clockpanel_paint.cpp" "%ROOT%src\alarm.cpp" "%ROOT%src\theme.cpp" ^
       "%ROOT%src\winutil.cpp" "%ROOT%src\common.cpp" "%ROOT%src\config.cpp" ^
       "%ROOT%src\defaults.cpp" "%ROOT%src\montheme.cpp" "%ROOT%src\clocktheme.cpp" ^
       /Fe:alarm_test.exe ^
       /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib ^
       dwmapi.lib advapi32.lib shlwapi.lib
if errorlevel 1 (
    popd
    echo [ERROR] The alarm tests did not compile.
    exit /b 1
)
echo.
.\alarm_test.exe
if errorlevel 1 set RESULT=1
popd
exit /b %RESULT%
