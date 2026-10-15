@echo off
rem Builds the standalone MVE decoder test tool build\mvedump\mvedump.exe (not part of the game build).
rem Usage: tools\build_mvedump.bat
rem Then:  build\mvedump\mvedump.exe data\movies\iplogo.mve <output prefix> [--every N] [--sheet N] [--raw FILE]
setlocal
cd /d "%~dp0\.."
where cl >nul 2>nul
if not errorlevel 1 goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -all -prerelease -products * -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSINSTALL=%%i"
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
:havecl
set "OUT=build\mvedump"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /O2 /MT /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Isrc /Fo"%OUT%\\" /Fe"%OUT%\mvedump.exe" tools\mvedump.cpp src\media\mve.cpp src\core\png.cpp src\core\deflate.cpp src\core\base.cpp
if errorlevel 1 exit /b 1
echo Built %OUT%\mvedump.exe
