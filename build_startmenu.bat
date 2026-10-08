@echo off
REM ===========================================================================
REM  ProWindows - builds ProWindows_startmenu.dll, the Start menu styler that
REM  the tiler injects into StartMenuExperienceHost.exe and SearchHost.exe.
REM  Called by build.bat after the exe; run it on its own from a developer
REM  prompt to rebuild just the DLL. Usage: build_startmenu.bat [outdir]
REM
REM  Exit code 0 = DLL built. Anything else is a warning for build.bat, never an
REM  error: the tiler must build and run without the styler.
REM
REM  The Start menu is Windows.UI.Xaml (system XAML), which the SDK's own
REM  C++/WinRT already projects - unlike the Explorer DLL there is nothing to
REM  generate. The Windhawk shim is the Explorer DLL's, compiled again with
REM  PW_STYLER naming this styler and PW_STYLER_PACKAGED (AppContainer hosts).
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set OUTDIR=%ROOT%build
if not "%~1"=="" set OUTDIR=%~1
set SRC=%ROOT%src\startmenu
set SHIM=%ROOT%src\explorer
set MH=%ROOT%third_party\minhook
set DLLNAME=ProWindows_startmenu.dll

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set OBJDIR=%OUTDIR%\startmenu_obj
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM  MinHook is C (BSD-2-Clause, third_party\minhook); the mod is C++20. The
REM  SDK warnings in the C++/WinRT headers are not ours to fix, so /W3 and no
REM  /WX; the mod's own source is kept as published.
cl.exe /nologo /LD /std:c++20 /utf-8 /W3 /EHsc /permissive /DNDEBUG /DUNICODE /D_UNICODE ^
       /DPW_STYLER=L\"startmenu\" /DPW_STYLER_PACKAGED ^
       /O1 /Os /Oi /Gy /MT /GR- /bigobj /Zc:__cplusplus /await ^
       /I"%MH%\include" /I"%SHIM%" ^
       /Fo"%OBJDIR%\\" ^
       "%SRC%\styler.cpp" "%SHIM%\windhawk_shim.cpp" ^
       "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
       /Fe:"%OUTDIR%\%DLLNAME%" ^
       /link /INCREMENTAL:NO /MANIFEST:NO /OPT:REF /OPT:ICF ^
             comctl32.lib d2d1.lib dwmapi.lib gdi32.lib msimg32.lib ole32.lib ^
             oleaut32.lib runtimeobject.lib shlwapi.lib uxtheme.lib user32.lib ^
             advapi32.lib shell32.lib winhttp.lib version.lib
set DLLERR=%errorlevel%

del /q "%OUTDIR%\ProWindows_startmenu.lib" "%OUTDIR%\ProWindows_startmenu.exp" >nul 2>&1
exit /b %DLLERR%
