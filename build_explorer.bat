@echo off
REM ===========================================================================
REM  ProWindows - builds ProWindows_explorer.dll, the File Explorer styler that
REM  the tiler injects into explorer.exe. Called by build.bat after the exe;
REM  run it on its own from a developer prompt (or via build.bat) to rebuild
REM  just the DLL. Usage: build_explorer.bat [outdir]   (default: build)
REM
REM  Exit code 0 = DLL built. Anything else is a warning for build.bat, never an
REM  error: the tiler must build and run without the styler.
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set OUTDIR=%ROOT%build
if not "%~1"=="" set OUTDIR=%~1
set SRC=%ROOT%src\explorer
set MH=%ROOT%third_party\minhook
set WINRT=%ROOT%build\winrt
set DLLNAME=ProWindows_explorer.dll

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM ---------------------------------------------------------------- WinUI headers
REM  The mod talks to File Explorer's XAML through C++/WinRT projections of the
REM  Windows App SDK that ships inside Windows 11 (WinUI 3, the Microsoft.UI.*
REM  namespaces - not the WinUI 2 package, whose winmd has no Microsoft.UI.Xaml.h).
REM  They are generated here from the machine's own winmd files with the SDK's
REM  cppwinrt.exe, once, into build\winrt; the Windows.* namespaces come from the
REM  SDK's own C++/WinRT headers (UnionMetadata is the reference for the
REM  generator). Generated output is never committed.
if not exist "%WINRT%\ready.txt" (
    call "%SRC%\gen_winrt.bat" "%WINRT%" "%SRC%"
    if errorlevel 1 exit /b 1
)

REM ---------------------------------------------------------------- compile
set OBJDIR=%OUTDIR%\explorer_obj
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM  MinHook is C (BSD-2-Clause, third_party\minhook); the mod is C++20.
REM  The incoming SDK warnings in the generated headers are not ours to fix, so
REM  /W3 and no /WX; the mod's own source is kept as published.
cl.exe /nologo /LD /std:c++20 /utf-8 /W3 /EHsc /permissive /DNDEBUG /DUNICODE /D_UNICODE ^
       /O1 /Os /Oi /Gy /MT /GR- /bigobj /Zc:__cplusplus /await ^
       /I"%WINRT%" /I"%MH%\include" /I"%SRC%" ^
       /Fo"%OBJDIR%\\" ^
       "%SRC%\styler.cpp" "%SRC%\windhawk_shim.cpp" ^
       "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
       /Fe:"%OUTDIR%\%DLLNAME%" ^
       /link /INCREMENTAL:NO /MANIFEST:NO /OPT:REF /OPT:ICF ^
             comctl32.lib d2d1.lib dwmapi.lib gdi32.lib msimg32.lib ole32.lib ^
             oleaut32.lib runtimeobject.lib shlwapi.lib uxtheme.lib user32.lib ^
             advapi32.lib shell32.lib winhttp.lib
set DLLERR=%errorlevel%

del /q "%OUTDIR%\ProWindows_explorer.lib" "%OUTDIR%\ProWindows_explorer.exp" >nul 2>&1
exit /b %DLLERR%
