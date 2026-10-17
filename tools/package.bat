@echo off
rem package.bat - build, test and zip the player release on Windows (tools\package.cmake, which says what it does).
rem
rem   tools\package.bat                  build the kit, test it against the game in game\assets, zip it
rem   tools\package.bat --no-test        skip the test (it needs the game in game\assets)
rem   tools\package.bat --test-download  test with the compiler Setup downloads, not the one in toolchain\
rem   tools\package.bat -j N             parallel jobs (default: every core)
rem
rem Needs cmake (3.25 or newer), ninja and git on PATH, as in a Developer Command Prompt for Visual Studio. The
rem compiler, llvm-mingw, is downloaded into toolchain\. A release made here has no setup-linux.
setlocal
set "ARGS="
:args
if "%~1"=="" goto run
if "%~1"=="--no-test" (
    set "ARGS=%ARGS% -DTEST=OFF"
    shift
    goto args
)
if "%~1"=="--test-download" (
    set "ARGS=%ARGS% -DTEST_DOWNLOAD=ON"
    shift
    goto args
)
if "%~1"=="-j" (
    if "%~2"=="" echo -j needs a number& exit /b 2
    set "ARGS=%ARGS% -DJOBS=%~2"
    shift
    shift
    goto args
)
echo unknown option: %~1
exit /b 2

:run
where cmake >nul 2>nul || (echo cmake is not on PATH: use a Developer Command Prompt for Visual Studio& exit /b 1)
where ninja >nul 2>nul || (echo ninja is not on PATH: use a Developer Command Prompt for Visual Studio& exit /b 1)
cmake %ARGS% -P "%~dp0package.cmake"
exit /b %errorlevel%
