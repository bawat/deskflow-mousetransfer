@echo off
REM Incremental rebuild of deskflow-core (reuses the existing build\ ninja cache).
setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\CMake\bin"
set "NINJA=X:\Users\bawat\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe"
set "QTDIR=X:\Qt\6.8.1\msvc2022_64"

call "%VCVARS%" >nul 2>nul
set "PATH=%CMAKE%;%NINJA%;%QTDIR%\bin;%PATH%"

cmake --build build --target deskflow-core
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )
echo === Built build\bin\deskflow-core.exe ===
endlocal
