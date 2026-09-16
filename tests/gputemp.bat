@echo off
REM  Builds and runs tests\gputemp.cpp: what each GPU-temperature source costs
REM  in private memory on this machine. See the header of that file.
setlocal
set ROOT=%~dp0..
if not defined VSCMD_ARG_TGT_ARCH call "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%ROOT%\tests\build" mkdir "%ROOT%\tests\build"
cl.exe /nologo /std:c++17 /utf-8 /W4 /EHsc /DUNICODE /D_UNICODE /O2 /MT ^
    /Fo:"%ROOT%\tests\build\\" "%ROOT%\tests\gputemp.cpp" ^
    /Fe:"%ROOT%\tests\build\gputemp.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
"%ROOT%\tests\build\gputemp.exe" nvml
"%ROOT%\tests\build\gputemp.exe" nvapi
"%ROOT%\tests\build\gputemp.exe" wmi
