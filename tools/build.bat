@echo off
rem build.bat - build Game.exe on Windows with the pinned Zig, the compiler releases and players use.
rem
rem   tools\build.bat          configure (first run: downloads Zig into toolchain\), recompile, build
rem   tools\build.bat --pdb    also write Game.pdb, for Visual Studio or WinDbg (turn it off again with
rem                            cmake -DXBE_PDB=OFF out\build\zig-windows)
rem
rem Needs CMake 3.25 or newer, Ninja and Git; it offers to install them with winget. No Visual Studio.
setlocal
set "GAME_TITLE=Conker: Live and Reloaded"
set "GAME_XBE=default.xbe"
set "GAME_EXE=Game"
title %GAME_TITLE% - Build
rem the repository root, where assets\ and out\ are
cd /d "%~dp0.."
echo.
echo   %GAME_TITLE% - build
echo   ================================================
echo.

set "OUT=%CD%\out\build\zig-windows"
set "PDB="
if /i "%~1"=="--pdb" set "PDB=1"

if not exist assets\%GAME_XBE% goto :nogame

echo [1/3] Tools
call :findtools
rem CI is set on GitHub Actions: no prompts there.
if not defined TOOLS_OK if defined CI goto :byhand
if not defined TOOLS_OK (
    where winget >nul 2>nul || goto :byhand
    choice /c YN /n /m "      Install them with winget? [Y/N] "
    if errorlevel 2 goto :byhand
    if defined NEED_CMAKE winget install --id Kitware.CMake -e --source winget
    if defined NEED_NINJA winget install --id Ninja-build.Ninja -e --source winget
    if defined NEED_GIT winget install --id Git.Git -e --source winget
    call :findtools
    if not defined TOOLS_OK goto :byhand
)
for /f "tokens=*" %%v in ('cmake --version ^| findstr /i version') do echo       %%v

echo [2/3] Compiler and configure
cmake -DWHAT=zig-windows -P "%~dp0configure.cmake" || goto :fail
if defined PDB cmake -DXBE_PDB=ON "%OUT%" >nul || goto :fail

echo [3/3] Game code
echo       the first build recompiles %GAME_XBE% and compiles the result, it takes a while ...
cmake --build "%OUT%" --target %GAME_EXE% || goto :fail

echo.
echo   %GAME_TITLE% built.
echo   %OUT%\%GAME_EXE%.exe
echo.
if defined CI exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if not errorlevel 2 (start "" /d "%OUT%" "%OUT%\%GAME_EXE%.exe") else (pause)
exit /b 0

rem helpers

rem cmake, ninja and git on PATH, also where their installers put them (a shell opened before installing lacks them)
:findtools
set TOOLS_OK=
set NEED_CMAKE=
set NEED_NINJA=
set NEED_GIT=
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links" set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
where cmake >nul 2>nul || set NEED_CMAKE=1
where ninja >nul 2>nul || set NEED_NINJA=1
where git   >nul 2>nul || set NEED_GIT=1
if defined NEED_CMAKE echo       missing: cmake
if defined NEED_NINJA echo       missing: ninja
if defined NEED_GIT echo       missing: git
if not defined NEED_CMAKE if not defined NEED_NINJA if not defined NEED_GIT set TOOLS_OK=1
exit /b 0

:byhand
echo.
echo       Install by hand, then run this again:
echo         winget install --id Kitware.CMake -e
echo         winget install --id Ninja-build.Ninja -e
echo         winget install --id Git.Git -e
echo       or from cmake.org, github.com/ninja-build/ninja/releases and git-scm.com.
goto :fail

:nogame
echo   Put the extracted game in the assets folder first:
echo     %CD%\assets
echo   It needs %GAME_XBE% with the game's folders beside it.
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined CI pause
exit /b 1
