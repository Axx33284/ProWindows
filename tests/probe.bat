@echo off
REM ===========================================================================
REM  ProWindows - read-only window probe
REM  Prints how each window on this desktop would be classified. Moves nothing.
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
       "%~dp0probe.cpp" ^
       "%ROOT%src\winutil.cpp" ^
       "%ROOT%src\common.cpp" ^
       "%ROOT%src\config.cpp" ^
       "%ROOT%src\montheme.cpp" ^
       /Fe:probe.exe ^
       /link /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib dwmapi.lib ^
       advapi32.lib shlwapi.lib

if errorlevel 1 (
    popd
    echo [ERROR] The probe did not compile.
    exit /b 1
)

echo.
.\probe.exe
popd
