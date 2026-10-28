@echo off
rem package-android.bat - build, test and zip the Android release on Windows: the APK builder (tools\package-android.cmake,
rem which says what it does).
rem
rem   tools\package-android.bat                  build the kit, test it against the game in assets\, zip it
rem   tools\package-android.bat --no-test        skip the test (it needs the game in assets\)
rem   tools\package-android.bat --test-download  test with the compiler the builder downloads, not the one in toolchain\
rem   tools\package-android.bat -j N             parallel jobs (default: every core)
rem
rem Needs CMake 3.25 or newer, Ninja and Git on PATH (winget install --id Kitware.CMake / Ninja-build.Ninja /
rem Git.Git, or tools\build.bat offers to). The compiler, Zig, is downloaded into toolchain\. No Visual Studio.
rem Also the Android NDK and SDK (ANDROID_NDK_HOME, ANDROID_HOME) and a JDK 17 to 21 (JAVA_HOME). A release made here
rem has apk-builder-windows.exe alone.
setlocal
rem before any shift: shift moves %0 too, and %~dp0 with it
set "HERE=%~dp0"
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
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links" set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
where cmake >nul 2>nul || (echo cmake is missing: winget install --id Kitware.CMake -e& exit /b 1)
where ninja >nul 2>nul || (echo ninja is missing: winget install --id Ninja-build.Ninja -e& exit /b 1)
where git >nul 2>nul || (echo git is missing: winget install --id Git.Git -e& exit /b 1)
cmake %ARGS% -P "%HERE%package-android.cmake"
exit /b %errorlevel%
