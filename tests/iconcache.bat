@echo off
REM ===========================================================================
REM  Checks that a shell icon survives the search bar's icon cache the right
REM  way up: fetched, saved, released, read back, saved again. Creates no
REM  window and launches nothing. Prints PASS or FAIL per stage.
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
if not exist "%ROOT%\tests\build\ic" mkdir "%ROOT%\tests\build\ic"

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\ic\\" ^
    "%ROOT%\tests\iconcache.cpp" ^
    "%ROOT%\src\appicon.cpp" "%ROOT%\src\common.cpp" ^
    /Fe:"%ROOT%\tests\build\iconcache.exe" ^
    /link /SUBSYSTEM:CONSOLE ^
    user32.lib gdi32.lib advapi32.lib shell32.lib shlwapi.lib ole32.lib
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\iconcache.exe" %*
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
