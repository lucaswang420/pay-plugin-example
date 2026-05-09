@echo off
setlocal enabledelayedexpansion
REM Run script for Pay Plugin Backend

REM Parse command line arguments
set BUILD_MODE=Release
if "%1"=="-debug" set BUILD_MODE=Debug
if "%1"=="-release" set BUILD_MODE=Release

echo ========================================
echo Starting Pay Plugin Backend
echo Build Mode: %BUILD_MODE%
echo ========================================

REM Change to PayBackend directory
cd /d "%~dp0.."
echo Working directory: %CD%

REM Check if build directory exists
if not exist build (
    echo Error: build directory not found!
    echo Please run build.bat first.
    pause
    exit /b 1
)

cd build

REM Load Conan environment if available
if exist conanrun.bat (
    echo Loading Conan environment...
    call conanrun.bat
)

REM Check if the requested build exists
if not exist %BUILD_MODE%\PayServer.exe (
    echo Error: PayServer.exe not found in %BUILD_MODE% directory!
    echo Please run build.bat first to build the %BUILD_MODE% version.
    pause
    exit /b 1
)

echo Starting server (%BUILD_MODE% build)...
%BUILD_MODE%\PayServer.exe

:end
pause
