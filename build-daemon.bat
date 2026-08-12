@echo off
REM Build ONLY deskflow-daemon against the already-configured build dir (no clean reconfigure).
setlocal
cd /d "%~dp0"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\CMake\bin"
set "NINJA=X:\Users\bawat\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe"
set "QTDIR=X:\Qt\6.8.1\msvc2022_64"
set "VCPKG=X:\vcpkg"
call "%VCVARS%" >nul 2>nul
set "PATH=%CMAKE%;%NINJA%;%QTDIR%\bin;%PATH%"
if not exist build\CMakeCache.txt ( echo [ERROR] build not configured - run build-merged-fork.bat first & exit /b 1 )
cmake --build build --target deskflow-daemon
if errorlevel 1 ( echo [ERROR] daemon build failed & exit /b 1 )
echo === Built deskflow-daemon ===
endlocal
