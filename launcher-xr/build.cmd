@echo off
rem Builds XOPSVR.exe (the XOPS-WXR WinlatorXR launcher).
rem Requires the Visual Studio 2022 C++ x86 toolchain.
setlocal
if not defined VCINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
    if errorlevel 1 (
        echo [FAIL] vcvarsall not found. Run this from an x86 Developer prompt.
        exit /b 1
    )
)
where cl.exe >nul 2>&1
if errorlevel 1 (echo [FAIL] cl.exe not found. & exit /b 1)
cl 2>&1 | findstr /i /c:"for x86" >nul
if errorlevel 1 (echo [FAIL] this is not the x86 compiler. & exit /b 1)

cd /d "%~dp0"
rc.exe /nologo /DUNICODE /D_UNICODE /fo XOPSVR.res XOPSVR.rc
if errorlevel 1 (echo [FAIL] rc failed. & exit /b 1)

cl.exe /nologo /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /c XOPSVR.cpp
if errorlevel 1 (echo [FAIL] compile failed. & exit /b 1)

link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:XOPSVR.exe XOPSVR.obj XOPSVR.res ^
    user32.lib comctl32.lib winhttp.lib
if errorlevel 1 (echo [FAIL] link failed. & exit /b 1)

echo Built %~dp0XOPSVR.exe
