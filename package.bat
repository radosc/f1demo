@echo off
rem Assembles dist\FalloutDemoHD\ : the game, its documentation and the mods folder.
rem Usage: package.bat [withdata]
rem   withdata  also copies the converted data\ folder (otherwise the game converts
rem             FALLDEMO.DAT on first start; place the folder next to the demo's files).
setlocal
cd /d "%~dp0"
if not exist bin\FalloutDemoHD.exe (
    echo bin\FalloutDemoHD.exe not found - run build.bat first.
    exit /b 1
)
set "OUT=dist\FalloutDemoHD"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\bin"
copy /y bin\FalloutDemoHD.exe "%OUT%\bin\" >nul
copy /y README.md "%OUT%\" >nul
copy /y MODDING.md "%OUT%\" >nul
copy /y MapEditor.cmd "%OUT%\" >nul
xcopy /e /i /q /y mods "%OUT%\mods" >nul
if /i "%~1"=="withdata" (
    echo Copying converted data...
    xcopy /e /i /q /y data "%OUT%\data" >nul
)
echo Packaged to %OUT%
