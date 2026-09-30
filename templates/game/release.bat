@echo off
set "GAME_EXE=Game"
cd /d "%~dp0"
set "OUT=out\build\win-amd64-release"
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || exit /b 1
del /q "%GAME_EXE%.zip" 2>nul
7z a "%GAME_EXE%.zip" ".\%OUT%\%GAME_EXE%.exe" .\README.md
