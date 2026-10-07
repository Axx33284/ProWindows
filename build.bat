@echo off
REM ===========================================================================
REM  ProWindows - build script (MSVC, x64)
REM  Produces build\ProWindows.exe - a single self-contained executable
REM  with no runtime dependencies (static CRT).
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set OUTDIR=%ROOT%build
set OUTEXE=ProWindows.exe
REM  Optional first argument overrides the output name, so a build can be made
REM  and tested while an older copy is still running and holding the exe open.
if not "%~1"=="" set OUTEXE=%~1

REM ---------------------------------------------------------------- toolchain
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
        echo         Install "Desktop development with C++" from the Visual Studio
        echo         Build Tools, or run this script from a Developer Command Prompt.
        exit /b 1
    )
    echo [1/4] Setting up MSVC environment...
    call !VCVARS! >nul
    if errorlevel 1 (
        echo [ERROR] vcvars64.bat failed.
        exit /b 1
    )
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM ---------------------------------------------------------------- icon
if not exist "%ROOT%res\app.ico" (
    echo [2/4] Generating icon...
    where python >nul 2>&1 && python "%ROOT%res\gen_icon.py"
    if not exist "%ROOT%res\app.ico" (
        echo [ERROR] res\app.ico is missing and could not be generated.
        echo         Install Python, or supply your own res\app.ico.
        exit /b 1
    )
) else (
    echo [2/4] Icon present.
)

REM ---------------------------------------------------------------- resources
echo [3/4] Compiling resources...
pushd "%ROOT%res"
rc.exe /nologo /fo "%OUTDIR%\app.res" app.rc
set RCERR=%errorlevel%
popd
if not "%RCERR%"=="0" (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

REM ---------------------------------------------------------------- compile
echo [4/4] Compiling and linking...
pushd "%OUTDIR%"

REM  /utf-8 matters: the sources are UTF-8 and contain a degree sign and the
REM  network arrows. Without it MSVC reads them in the machine's ANSI code
REM  page, and those characters reach the overlay as mojibake.
cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
       /O1 /Os /Oi /GL /Gy /MT /GR- /fp:fast ^
       "%ROOT%src\common.cpp" ^
       "%ROOT%src\config.cpp" ^
       "%ROOT%src\defaults.cpp" ^
       "%ROOT%src\winutil.cpp" ^
       "%ROOT%src\layout.cpp" ^
       "%ROOT%src\wm.cpp" ^
       "%ROOT%src\hotkeys.cpp" ^
       "%ROOT%src\moddrag.cpp" ^
       "%ROOT%src\theme.cpp" ^
       "%ROOT%src\rowlist.cpp" ^
       "%ROOT%src\modal.cpp" ^
       "%ROOT%src\sysinfo.cpp" ^
       "%ROOT%src\thermal.cpp" ^
       "%ROOT%src\montheme.cpp" ^
       "%ROOT%src\monpaint.cpp" ^
       "%ROOT%src\monitor.cpp" ^
       "%ROOT%src\clocktheme.cpp" ^
       "%ROOT%src\clockpaint.cpp" ^
       "%ROOT%src\clock.cpp" ^
       "%ROOT%src\settings_clock.cpp" ^
       "%ROOT%src\dragguide.cpp" ^
       "%ROOT%src\appicon.cpp" ^
       "%ROOT%src\launcher.cpp" ^
       "%ROOT%src\search.cpp" ^
       "%ROOT%src\ipc.cpp" ^
       "%ROOT%src\settings.cpp" ^
       "%ROOT%src\settings_pages.cpp" ^
       "%ROOT%src\settings_monitor.cpp" ^
       "%ROOT%src\settings_keys.cpp" ^
       "%ROOT%src\settings_search.cpp" ^
       "%ROOT%src\main.cpp" ^
       /Fe:"%OUTEXE%" ^
       /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO ^
             /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ^
             "%OUTDIR%\app.res"

set CLERR=%errorlevel%

REM ---------------------------------------------------------------- control CLI
REM  prowindowsctl.exe - the console front end. Separate executable because a
REM  /SUBSYSTEM:WINDOWS binary has no stdout, so `ProWindows.exe --msg` can
REM  print to a terminal but cannot be piped or captured. Same code underneath.
if "%CLERR%"=="0" (
    echo       plus prowindowsctl.exe...
    cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
           /O1 /Os /Oi /Gy /MT /GR- ^
           "%ROOT%src\ctl_main.cpp" ^
           "%ROOT%src\ipc.cpp" ^
           "%ROOT%src\common.cpp" ^
           /Fe:"prowindowsctl.exe" ^
           /link /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO ^
                 /SUBSYSTEM:CONSOLE
    set CLERR=%errorlevel%
)

del /q *.obj >nul 2>&1
popd

if not "%CLERR%"=="0" (
    echo.
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
for %%F in ("%OUTDIR%\%OUTEXE%") do echo   Built %%~fF  (%%~zF bytes^)
echo.
echo Run it, then look for the tray icon. Right-click the icon for shortcuts.
endlocal
exit /b 0
