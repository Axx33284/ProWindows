@echo off
REM Generates the WinUI 3 C++/WinRT headers into %1 (build\winrt) for build_explorer.bat.
REM %2 is src\explorer (for fix_winrt.ps1). Exit code 1 = not generated (a warning
REM for the caller; nothing is left half-written). See build_explorer.bat for why.
setlocal enabledelayedexpansion
set "WINRT=%~1"
set "SRC=%~2"

set CPPWINRT=
REM  "%ProgramFiles(x86)%" holds a ')' that would end a parenthesised block.
set "PF86=%ProgramFiles(x86)%"
for /f "delims=" %%V in ('dir /b /ad /o-n "!PF86!\Windows Kits\10\bin" 2^>nul') do (
    if not defined CPPWINRT if exist "!PF86!\Windows Kits\10\bin\%%V\x64\cppwinrt.exe" (
        set "CPPWINRT=!PF86!\Windows Kits\10\bin\%%V\x64\cppwinrt.exe"
        set "SDKVER=%%V"
    )
)
if not defined CPPWINRT (
    echo [warn] cppwinrt.exe not found - Windows SDK missing, styler not built.
    exit /b 1
)
set "UNION=%PF86%\Windows Kits\10\UnionMetadata\%SDKVER%"
set "WAR=%SystemRoot%\SystemApps\Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe"
if not exist "%WAR%\Microsoft.UI.Xaml.winmd" (
    echo [warn] Windows App Runtime metadata not found - styler not built.
    exit /b 1
)
echo       generating WinUI headers into build\winrt...
if exist "%WINRT%" rmdir /s /q "%WINRT%"
"%CPPWINRT%" -in "%WAR%\Microsoft.UI.winmd" -in "%WAR%\Microsoft.UI.Xaml.winmd" ^
    -in "%WAR%\Microsoft.UI.Text.winmd" -in "%WAR%\Microsoft.Foundation.winmd" ^
    -in "%WAR%\Microsoft.Graphics.winmd" ^
    -in "%WAR%\Microsoft.Windows.ApplicationModel.Resources.winmd" ^
    -in "%WAR%\Microsoft.Web.WebView2.Core.winmd" ^
    -ref "%UNION%" -out "%WINRT%" >nul
if errorlevel 1 (
    echo [warn] cppwinrt failed - styler not built.
    if exist "%WINRT%" rmdir /s /q "%WINRT%"
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\fix_winrt.ps1" -Dir "%WINRT%\winrt" <nul
if errorlevel 1 (
    echo [warn] fixing the generated headers failed - styler not built.
    rmdir /s /q "%WINRT%"
    exit /b 1
)
echo ok> "%WINRT%\ready.txt"
exit /b 0
