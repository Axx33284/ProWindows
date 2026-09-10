@echo off
REM ===========================================================================
REM  Prints what this machine can tell us about CPU and GPU temperature, and
REM  which source answered. Read-only; starts nothing.
REM
REM    tempprobe.bat        one pass
REM    tempprobe.bat 20     keep printing for twenty seconds
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
    echo [ERROR] Could not find vcvars64.bat. Run this from a Developer
    echo         Command Prompt, or install the C++ build tools.
    exit /b 1
)
call "%VCVARS%" >nul

:haveToolchain
if not exist "%ROOT%\tests\build\tp" mkdir "%ROOT%\tests\build\tp"

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\tp\\" ^
    "%ROOT%\tests\tempprobe.cpp" "%ROOT%\src\thermal.cpp" "%ROOT%\src\common.cpp" ^
    /Fe:"%ROOT%\tests\build\tempprobe.exe" ^
    /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\tempprobe.exe" %1
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
