@echo off
REM ===========================================================================
REM  Runs MSVC /analyze over the files named on the command line (or every
REM  source file when none are), compiling only - no link, no output binary.
REM
REM  The project's review passes have always quoted an /analyze run; this makes
REM  that one command instead of a remembered incantation.
REM
REM    tests\analyze.bat                     everything
REM    tests\analyze.bat wm search moddrag   just those
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
if not exist "%ROOT%\tests\build\an" mkdir "%ROOT%\tests\build\an"

set FILES=
if "%~1"=="" (
    for %%F in ("%ROOT%\src\*.cpp") do set FILES=!FILES! "%%F"
) else (
    for %%A in (%*) do set FILES=!FILES! "%ROOT%\src\%%A.cpp"
)

REM  /analyze:WX- so an analyser note does not stop the run before the rest of
REM  the files have been looked at. The point is the whole report.
cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
       /c /analyze /analyze:WX- /Fo:"%ROOT%\tests\build\an\\" %FILES%
set ERR=%errorlevel%

echo.
if "%ERR%"=="0" (echo   analyze: clean) else (echo   analyze: exit %ERR%)
exit /b %ERR%

:try
if defined VCVARS exit /b 0
if exist %1 set VCVARS=%~1
exit /b 0
