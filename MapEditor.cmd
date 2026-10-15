@echo off
rem Opens the map editor (default map: junkdemo).
rem Usage: MapEditor.cmd [map] [--mod folder] [--save-as name]
start "" "%~dp0bin\FalloutDemoHD.exe" --editor %*
