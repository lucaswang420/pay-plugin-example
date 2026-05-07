@echo off
setlocal enabledelayedexpansion
REM Test script for Pay Plugin Backend

echo ========================================
echo Pay Plugin Backend Test Runner
echo ========================================

REM Change to PayBackend directory
cd /d "%~dp0.."
echo Working directory: %CD%
echo.

REM Parse command line arguments
set LIST_TESTS=0
set RUN_ALL=1
set TEST_NAME=

:parse_args
if "%1"=="" goto end_parse
if /i "%1"=="-l" (
    set LIST_TESTS=1
    set RUN_ALL=0
    shift
    goto parse_args
)
if /i "%1"=="-r" (
    set RUN_ALL=0
    shift
    set TEST_NAME=%1
    shift
    goto parse_args
)
if /i "%1"=="-s" (
    set SHOW_SUCCESS=1
    shift
    goto parse_args
)
echo Unknown option: %1
echo Usage: %0 [-l] [-r test_name] [-s]
echo   -l          List available tests
echo   -r <name>   Run a specific test
echo   -s          Show successful tests
endlocal
exit /b 1
:end_parse

REM Determine test executable path
set TEST_EXE=
if exist "%CD%\build\test\Release\PayBackendTests.exe" (
    set TEST_EXE=%CD%\build\test\Release\PayBackendTests.exe
) else if exist "%CD%\build\Release\PayBackendTests.exe" (
    set TEST_EXE=%CD%\build\Release\PayBackendTests.exe
) else (
    echo Error: Test executable not found
    echo Expected locations:
    echo   - %CD%\build\test\Release\PayBackendTests.exe
    echo   - %CD%\build\Release\PayBackendTests.exe
    echo.
    echo Please build the project first using build.bat
    endlocal
    exit /b 1
)

echo Test executable: !TEST_EXE!
echo.

REM List tests if requested
if %LIST_TESTS% equ 1 (
    echo ========================================
    echo Available Tests
    echo ========================================
    "!TEST_EXE!" -l
    endlocal
    exit /b 0
)

REM Run specific test if requested
if not "%TEST_NAME%"=="" (
    echo ========================================
    echo Running Test: %TEST_NAME%
    echo ========================================
    "!TEST_EXE!" -r %TEST_NAME%
    if %errorlevel% neq 0 (
        echo.
        echo Error: Test failed
        endlocal
        exit /b 1
    )
    echo.
    echo Test passed successfully!
    endlocal
    exit /b 0
)

REM Run all tests
echo ========================================
echo Running All Tests
echo ========================================
echo.

REM Set environment for tests
set DB_HOST=localhost
set DB_PORT=5432
set DB_NAME=pay_test
set DB_USER=test
set DB_PASS=123456

REM Run tests
if defined SHOW_SUCCESS (
    "!TEST_EXE!" -s
) else (
    "!TEST_EXE!"
)

if %errorlevel% neq 0 (
    echo.
    echo ========================================
    echo TESTS FAILED
    echo ========================================
    endlocal
    exit /b 1
)

echo.
echo ========================================
echo ALL TESTS PASSED
echo ========================================
endlocal
exit /b 0
