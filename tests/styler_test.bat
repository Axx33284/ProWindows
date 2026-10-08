@echo off
REM ===========================================================================
REM  Tests for the File Explorer styler's shim and the ProWindows side that
REM  writes its settings. Everything runs in this process: nothing is injected
REM  and Explorer is never touched. Exit code 1 on failure. Usage: tests\styler_test.bat
REM ===========================================================================
setlocal enabledelayedexpansion
set ROOT=%~dp0..

if defined VSCMD_ARG_TGT_ARCH goto :haveToolchain

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
    if not defined VCVARS if exist %%P set "VCVARS=%%~P"
)

if not defined VCVARS (
    echo [ERROR] Could not find vcvars64.bat.
    exit /b 1
)
call "%VCVARS%" >nul

:haveToolchain
if not exist "%ROOT%\tests\build\styler" mkdir "%ROOT%\tests\build\styler"
set MH=%ROOT%\third_party\minhook

REM  The shim and MinHook are compiled into the test itself, with the mod's entry
REM  points stubbed, so the real lifecycle code runs without an Explorer around it.
cl.exe /nologo /std:c++17 /utf-8 /W3 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /O2 /MT /Fo:"%ROOT%\tests\build\styler\\" ^
    /I"%MH%\include" ^
    "%ROOT%\tests\styler_test.cpp" ^
    "%ROOT%\src\explorer\windhawk_shim.cpp" ^
    "%ROOT%\src\explorerstyler.cpp" "%ROOT%\src\common.cpp" ^
    "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
    /Fe:"%ROOT%\tests\build\styler_test.exe" ^
    /link /INCREMENTAL:NO /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib winhttp.lib ^
    advapi32.lib shlwapi.lib
if errorlevel 1 (
    echo [ERROR] The styler tests did not compile.
    exit /b 1
)

"%ROOT%\tests\build\styler_test.exe" "%ROOT%"
set RESULT=%errorlevel%

REM  The Start menu styler: the same shim compiled the way the Start menu DLL gets
REM  it (PW_STYLER_PACKAGED: signals as files, stored values), plus the ProWindows
REM  side with the app-package ACL. Still nothing is injected.
if not exist "%ROOT%\tests\build\styler_sm" mkdir "%ROOT%\tests\build\styler_sm"
cl.exe /nologo /std:c++17 /utf-8 /W3 /EHsc /permissive- /DNDEBUG /DUNICODE /D_UNICODE ^
    /DPW_STYLER=L\"startmenu\" /DPW_STYLER_PACKAGED ^
    /O2 /MT /Fo:"%ROOT%\tests\build\styler_sm\\" ^
    /I"%MH%\include" ^
    "%ROOT%\tests\styler_startmenu_test.cpp" ^
    "%ROOT%\src\explorer\windhawk_shim.cpp" ^
    "%ROOT%\src\startmenustyler.cpp" "%ROOT%\src\common.cpp" ^
    "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
    /Fe:"%ROOT%\tests\build\styler_startmenu_test.exe" ^
    /link /INCREMENTAL:NO /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib winhttp.lib ^
    advapi32.lib shlwapi.lib dwmapi.lib
if errorlevel 1 (
    echo [ERROR] The Start menu styler tests did not compile.
    exit /b 1
)

"%ROOT%\tests\build\styler_startmenu_test.exe" "%ROOT%"
if errorlevel 1 set RESULT=1
exit /b %RESULT%

