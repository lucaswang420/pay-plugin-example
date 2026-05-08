@echo off
setlocal enabledelayedexpansion
REM Build script for Pay Plugin Backend

echo ========================================
echo Pay Plugin Backend Build Script
echo ========================================

REM ========================================
REM Kill any running PayServer processes
REM ========================================
echo Checking for running PayServer processes...
taskkill /F /IM PayServer.exe >nul 2>&1
if %errorlevel% equ 0 (
    echo Killed running PayServer.exe process
    timeout /t 1 /nobreak >nul
) else (
    echo No running PayServer.exe process found
)

REM Change to PayBackend directory
cd /d "%~dp0.."
echo Working directory: %CD%

REM Default build type
set BUILD_TYPE=Release

REM Parse command line arguments
:parse_args
if "%1"=="" goto end_parse
if /i "%1"=="-debug" (
    set BUILD_TYPE=Debug
    shift
    goto parse_args
)
if /i "%1"=="-release" (
    set BUILD_TYPE=Release
    shift
    goto parse_args
)
echo Unknown option: %1
echo Usage: %0 [-debug|-release]
echo   -debug     Build debug version
echo   -release   Build release version (default)
exit /b 1
:end_parse

echo Building with configuration:
echo   Build Type: %BUILD_TYPE%
echo.

REM Check if build directory exists
if not exist build (
    echo Creating build directory...
    mkdir build
)

cd build

REM Install dependencies and configure
echo Installing dependencies...
conan install .. --build=missing -s build_type=%BUILD_TYPE% --output-folder . 
if %errorlevel% neq 0 (
    echo Error: Conan install failed
    cd /d "%~dp0.."
    exit /b 1
)

REM Configure with CMake
echo Configuring project...
cmake .. -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
if %errorlevel% neq 0 (
    echo Error: CMake configuration failed
    cd /d "%~dp0.."
    exit /b 1
)

REM Build
echo Building project...
cmake --build . --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Error: Build failed
    cd /d "%~dp0.."
    exit /b 1
)

REM Copy config.json, .env, and certs to build directory
echo Copying configuration files...
robocopy .. %BUILD_TYPE% config.json .env /NFL /NDL /NJH /NJS /NP
if %ERRORLEVEL% GEQ 8 (
    echo Error: Failed to copy config files
    cd /d "%~dp0.."
    exit /b 1
)

REM Copy certs directory if it exists
if exist "..\certs" (
    robocopy ..\certs %BUILD_TYPE%\certs\ /E /NFL /NDL /NJH /NJS /NP
    if %ERRORLEVEL% GEQ 8 (
        echo Error: Failed to copy certs directory
        cd /d "%~dp0.."
        exit /b 1
    )
)

REM Also copy to test directory
echo Copying configuration files to test directory...
if exist "%BUILD_TYPE%\PayServer.exe" (
    robocopy .. test\%BUILD_TYPE% config.json .env /NFL /NDL /NJH /NJS /NP
    if exist "..\certs" (
        robocopy ..\certs test\%BUILD_TYPE%\certs\ /E /NFL /NDL /NJH /NJS /NP
    )
)

echo ========================================
echo Build complete!
echo ========================================
cd /d "%~dp0.."
endlocal
exit /b 0
