@echo off
setlocal enabledelayedexpansion
REM Run script for Pay Plugin Backend

echo ========================================
echo Starting Pay Plugin Backend
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

REM Check for debug build first
if exist Debug\PayServer.exe (
    echo Starting server (Debug build)...
    Debug\PayServer.exe
    goto :end
)

REM Check for release build
if not exist Release\PayServer.exe (
    echo Error: PayServer.exe not found in Debug or Release directories!
    echo Please run build.bat first.
    pause
    exit /b 1
)

echo Starting server (Release build)...
Release\PayServer.exe

:end
pause
