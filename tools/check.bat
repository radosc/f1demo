@echo off
rem Compiles the given source files only (syntax/type check, no link) into build\check\<tag>.
rem Usage: tools\check.bat <tag> src\ui\foo.cpp [src\ui\bar.cpp ...]
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
set "TAG=%~1"
shift
set "OUT=build\check\%TAG%"
if not exist "%OUT%" mkdir "%OUT%"
set "FILES="
:collect
if "%~1"=="" goto :compile
set "FILES=%FILES% "%~1""
shift
goto :collect
:compile
cl /nologo /c /std:c++17 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Isrc /Fo"%OUT%\\" %FILES%
