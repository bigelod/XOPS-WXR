@echo off
rem XOPS-WXR - RELEASE build (no probe, no debug keys, no diagnostic log).
rem
rem Compiles with XOPS_DEV_BUILD=0, builds the launcher, and assembles the
rem Release\ folder. Release has no xops_wxr_probe.log, no probe keys, no
rem draw dumps and no frame grabs.
rem
rem Usage:   build\build-release.bat
rem Output:  Release\d3d8.dll, XOPSVR.exe, config.txt, version.txt, INSTALL.txt

setlocal
set ROOT=%~dp0..
set OUTDIR=%ROOT%\out\release
set REL=%ROOT%\Release

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

echo [1/4] compiling RELEASE build...
cl /nologo /LD /O2 /MT /W3 /EHsc ^
   /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DXOPS_DEV_BUILD=0 ^
   /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\\" ^
   "%ROOT%\src\d3d8probe.cpp" "%ROOT%\src\xrapi.cpp" ^
   /link /DEF:"%ROOT%\src\d3d8probe.def" /MACHINE:X86 ^
         /OUT:"%OUTDIR%\d3d8.dll" ^
         /IMPLIB:"%OUTDIR%\d3d8probe.lib" user32.lib ws2_32.lib gdi32.lib
if errorlevel 1 (echo [FAIL] release build failed. & exit /b 1)

echo [2/4] building launcher...
pushd "%ROOT%\launcher-xr"
call build.cmd
if errorlevel 1 (popd & echo [FAIL] launcher build failed. & exit /b 1)
popd

echo [3/4] assembling %REL%...
if not exist "%REL%" mkdir "%REL%"
copy /y "%OUTDIR%\d3d8.dll" "%REL%\d3d8.dll" >nul
copy /y "%ROOT%\launcher-xr\XOPSVR.exe" "%REL%\XOPSVR.exe" >nul
copy /y "%ROOT%\launcher-xr\version.txt" "%REL%\version.txt" >nul
copy /y "%ROOT%\build\release-config.txt" "%REL%\config.txt" >nul
copy /y "%ROOT%\build\release-INSTALL.txt" "%REL%\INSTALL.txt" >nul

echo [4/4] release files:
for %%F in ("%REL%\d3d8.dll" "%REL%\XOPSVR.exe") do echo        %%~nxF  %%~zF bytes
echo.
echo Release ready in %REL%
endlocal
