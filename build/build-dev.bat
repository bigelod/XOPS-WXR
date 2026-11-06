@echo off
rem XOPS-WXR - Windows-PC DEV build (probe, debug keys, verbose log).
rem
rem Use this for development on a Windows PC. It compiles with
rem XOPS_DEV_BUILD=1: probe keys, HOOKS/HOOK trace, verbose log, draw dumps,
rem frame grabs and the diagnostic startup banner.
rem
rem Usage:   build\build-dev.bat  [deploy-path]
rem Default deploy path: E:\tmp\Q3Mods\xops096

setlocal
set ROOT=%~dp0..
set OUTDIR=%ROOT%\out\dev
set DEPLOY=%~1
if "%DEPLOY%"=="" set DEPLOY=E:\tmp\Q3Mods\xops096

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo [FAIL] cl.exe not found. Open "x86 Native Tools Command Prompt for VS 2022".
    exit /b 1
)
cl 2>&1 | findstr /i /c:"for x86" >nul
if errorlevel 1 (
    echo [FAIL] This cl.exe is not the x86 compiler.
    exit /b 1
)

if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
mkdir "%OUTDIR%" 2>nul

echo [1/3] compiling DEV build...
cl /nologo /LD /O2 /MT /W3 /EHsc ^
   /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DXOPS_DEV_BUILD=1 ^
   /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\\" ^
   "%ROOT%\src\d3d8probe.cpp" "%ROOT%\src\xrapi.cpp" ^
   /link /DEF:"%ROOT%\src\d3d8probe.def" /MACHINE:X86 ^
         /OUT:"%OUTDIR%\d3d8.dll" ^
         /IMPLIB:"%OUTDIR%\d3d8probe.lib" user32.lib ws2_32.lib gdi32.lib
if errorlevel 1 (echo [FAIL] build failed. & exit /b 1)

echo [2/3] built %OUTDIR%\d3d8.dll
for %%F in ("%OUTDIR%\d3d8.dll") do echo        %%~zF bytes, %%~tF

echo [3/3] deploying to %DEPLOY%
if not exist "%DEPLOY%" (
    echo [WARN] %DEPLOY% does not exist - DLL is at %OUTDIR%\d3d8.dll
    exit /b 0
)
copy /y "%OUTDIR%\d3d8.dll" "%DEPLOY%\d3d8.dll" >nul
if errorlevel 1 (echo [FAIL] copy failed - is the game running? & exit /b 1)

set SRC=
set DST=
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%OUTDIR%\d3d8.dll" SHA256') do if not defined SRC set SRC=%%H
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%DEPLOY%\d3d8.dll" SHA256') do if not defined DST set DST=%%H
if /i "%SRC%"=="%DST%" (echo        match=True  %SRC%) else (echo [FAIL] hash mismatch & exit /b 1)

echo.
echo DEV build ready. Log: %DEPLOY%\xops_wxr_probe.log
endlocal
