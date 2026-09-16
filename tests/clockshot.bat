@echo off
REM ===========================================================================
REM  Renders the desktop clock to PNG - one per style, one per skin -
REM  without putting the tiler on a live desktop. Output lands in tests\shots.
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
if not exist "%ROOT%\tests\build" mkdir "%ROOT%\tests\build"
if not exist "%ROOT%\tests\shots" mkdir "%ROOT%\tests\shots"

cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\\" ^
    "%ROOT%\tests\clockshot.cpp" "%ROOT%\src\clockpaint.cpp" "%ROOT%\src\clocktheme.cpp" ^
    "%ROOT%\src\monpaint.cpp" "%ROOT%\src\montheme.cpp" "%ROOT%\src\common.cpp" ^
    /Fe:"%ROOT%\tests\build\clockshot.exe" ^
    /link gdiplus.lib shell32.lib ole32.lib shlwapi.lib
if errorlevel 1 exit /b 1

"%ROOT%\tests\build\clockshot.exe" "%ROOT%\tests\shots"
exit /b %errorlevel%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
