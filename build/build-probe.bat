@echo off
rem XOPS-WXR - build the d3d8 diagnostic probe
rem
rem Run from a "x86 Native Tools Command Prompt for VS 2022".
rem   x86 matters: XOPS is 32-bit and so must this DLL be.
rem
rem No DirectX SDK and no MinHook are needed - the probe declares the handful
rem of D3D8 types it uses and patches vtable slots directly.
rem
rem Two translation units: d3d8probe.cpp (the hooks, the probe, the steering)
rem and xrapi.cpp (the WinlatorXR UDP client). ws2_32.lib is for the latter.
rem
rem Usage:   build\build-probe.bat  [deploy-path]
rem Default deploy path: E:\tmp\Q3Mods\xops096   (the owner's run folder)
rem The other known install is D:\VR\xops096-wxr - pass it as an argument.

setlocal
set ROOT=%~dp0..
set OUTDIR=%ROOT%\out\probe
set DEPLOY=%~1
if "%DEPLOY%"=="" set DEPLOY=E:\tmp\Q3Mods\xops096

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo [FAIL] cl.exe not found.
    echo        Open "x86 Native Tools Command Prompt for VS 2022" and rerun.
    exit /b 1
)

rem Refuse to build 64-bit by accident - a 64-bit DLL will not load into XOPS.
cl 2>&1 | findstr /i /c:"for x86" >nul
if errorlevel 1 (
    echo [FAIL] This cl.exe is not the x86 compiler.
    echo        Use the "x86 Native Tools Command Prompt", not x64.
    exit /b 1
)

if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
mkdir "%OUTDIR%" 2>nul

echo [1/3] compiling...
cl /nologo /LD /O2 /MT /W3 /EHsc ^
   /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN ^
   /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\\" ^
   "%ROOT%\src\d3d8probe.cpp" "%ROOT%\src\xrapi.cpp" ^
   /link /DEF:"%ROOT%\src\d3d8probe.def" /MACHINE:X86 ^
         /OUT:"%OUTDIR%\d3d8.dll" ^
         /IMPLIB:"%OUTDIR%\d3d8probe.lib" user32.lib ws2_32.lib gdi32.lib
if errorlevel 1 (
    echo [FAIL] build failed.
    exit /b 1
)

echo [2/3] built %OUTDIR%\d3d8.dll
for %%F in ("%OUTDIR%\d3d8.dll") do echo        %%~zF bytes, %%~tF

echo [3/3] deploying to %DEPLOY%
if not exist "%DEPLOY%" (
    echo [WARN] %DEPLOY% does not exist - skipping deploy.
    echo        The DLL is at %OUTDIR%\d3d8.dll; copy it next to XOPS.exe.
    exit /b 0
)
if exist "%DEPLOY%\d3d8.dll" (
    echo [WARN] %DEPLOY%\d3d8.dll already exists.
    echo        If that is a REAL d3d8.dll rather than a previous probe build,
    echo        move it aside first - do not overwrite a system DLL copy.
)
copy /y "%OUTDIR%\d3d8.dll" "%DEPLOY%\d3d8.dll" >nul
if errorlevel 1 (
    echo [FAIL] copy failed - is the game running?
    exit /b 1
)

rem Verify the deployed file matches what we just built. certutil prints a
rem header line, the hash, then a trailer - so take the first line after the
rem header and stop. (findstr's character classes proved case-insensitive here,
rem which made an earlier version of this check compare "CertUtil:" to itself
rem and pass vacuously. A check that cannot fail is worse than no check.)
set SRC=
set DST=
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%OUTDIR%\d3d8.dll" SHA256') do if not defined SRC set SRC=%%H
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%DEPLOY%\d3d8.dll" SHA256') do if not defined DST set DST=%%H
if not defined SRC (echo [FAIL] could not hash the built DLL & exit /b 1)
rem No space before the pipe: "echo %SRC% |" would echo a trailing space too,
rem and the anchored pattern would then never match.
echo %SRC%|findstr /i /r "^[0-9a-f][0-9a-f]*$" >nul
if errorlevel 1 (echo [FAIL] hash parse failed, got "%SRC%" & exit /b 1)
if /i "%SRC%"=="%DST%" (echo        match=True  %SRC%) else (echo [FAIL] hash mismatch & exit /b 1)

echo.
echo Done. Run xops096.exe, play a mission for ~30 seconds, walk in a straight
echo line past something you can measure, then QUIT CLEANLY (the end-of-session
echo summary is written on shutdown). Then read:
echo   %DEPLOY%\xops_wxr_probe.log
endlocal
