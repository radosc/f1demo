@echo off
rem Builds bin\FalloutDemoHD.exe with the MSVC x64 toolchain.
rem Usage: build.bat [debug]
rem (No delayed expansion: the project path may contain "!" characters.)
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - install Visual Studio 2019+ Build Tools with the C++ workload.
    exit /b 1
)
set "VSINSTALL="
rem -all also lists installs flagged "incomplete" that still have a working compiler
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -all -prerelease -products * -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSINSTALL=%%i"
)
if "%VSINSTALL%"=="" (
    echo No Visual Studio installation with the C++ x64 build tools was found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
:havecl

set "CFG=release"
set "CFLAGS=/nologo /std:c++17 /EHsc /W3 /utf-8 /MP /Zi /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Isrc"
set "LFLAGS=/SUBSYSTEM:WINDOWS /DEBUG"
if /i "%~1"=="debug" goto :debugcfg
set "CFLAGS=%CFLAGS% /MT /O2 /Oi /DNDEBUG"
set "LFLAGS=%LFLAGS% /OPT:REF /OPT:ICF"
goto :cfgdone
:debugcfg
set "CFG=debug"
set "CFLAGS=%CFLAGS% /MTd /Od /D_DEBUG /RTC1"
:cfgdone

set "OBJDIR=build\%CFG%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist bin mkdir bin

rem response file with every source file
if exist "%OBJDIR%\sources.rsp" del "%OBJDIR%\sources.rsp"
for /r src %%f in (*.cpp) do echo "%%f">>"%OBJDIR%\sources.rsp"

rc /nologo /fo "%OBJDIR%\resource.res" src\resource.rc
if errorlevel 1 exit /b 1

rem OUTEXE overrides the output (for example to build while the game is running)
if not defined OUTEXE set "OUTEXE=bin\FalloutDemoHD.exe"
cl %CFLAGS% /Fo"%OBJDIR%\\" /Fd"%OBJDIR%\vc.pdb" @"%OBJDIR%\sources.rsp" "%OBJDIR%\resource.res" /Fe"%OUTEXE%" /link %LFLAGS% user32.lib gdi32.lib shell32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib xaudio2.lib winmm.lib comdlg32.lib comctl32.lib
if errorlevel 1 exit /b 1
echo Built %OUTEXE% (%CFG%)
