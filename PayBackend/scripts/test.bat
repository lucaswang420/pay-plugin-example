@echo off
setlocal enabledelayedexpansion
REM Test script for Pay Plugin Backend using CTest

echo ========================================
echo Pay Plugin Backend Test Runner (CTest)
echo ========================================

REM Change to build directory
cd /d "%~dp0..\build"
echo Build directory: %CD%
echo.

REM Parse command line arguments
set LIST_TESTS=0
set RUN_ALL=1
set TEST_PATTERN=
set VERBOSE=1
set OUTPUT=0
set BUILD_TYPE=Release

:parse_args
if "%~1"=="" goto end_parse
if /i "%~1"=="-l" (
    set LIST_TESTS=1
    set RUN_ALL=0
    shift
    goto parse_args
)
if /i "%~1"=="-r" (
    set RUN_ALL=0
    shift
    set TEST_PATTERN=%~1
    shift
    goto parse_args
)
if /i "%~1"=="-v" (
    set VERBOSE=1
    shift
    goto parse_args
)
if /i "%~1"=="-o" (
    set OUTPUT=1
    shift
    goto parse_args
)
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
echo Unknown option: %~1
echo Usage: %0 [-l] [-r pattern] [-v] [-o]
echo   -l          List available tests
echo   -r <pattern> Run tests matching pattern (e.g., -r Idempotency)
echo   -v          Verbose output (show test details)
echo   -o          Output log file
endlocal
exit /b 1
:end_parse

REM Check if CTest configuration exists
if not exist "CTestTestfile.cmake" (
    if not exist "test\CTestTestfile.cmake" (
        echo Error: CTest configuration not found
        echo Expected locations:
        echo   - %CD%\CTestTestfile.cmake
        echo   - %CD%\test\CTestTestfile.cmake
        echo.
        echo Please build the project first using build.bat
        endlocal
        exit /b 1
    )
)

REM Determine test directory
set TEST_DIR=.
if not exist "CTestTestfile.cmake" (
    set TEST_DIR=test
)

echo Test directory: %CD%\!TEST_DIR!
echo.

REM Set environment for tests
set DB_HOST=localhost
set DB_PORT=5432
set DB_NAME=pay_test
set DB_USER=test
set DB_PASS=123456

REM Build CTest command (without --test-dir since we're already in the build dir)
set CTEST_CMD=ctest -C %BUILD_TYPE%

REM List tests if requested
if %LIST_TESTS% equ 1 (
    echo ========================================
    echo Available Tests
    echo ========================================
    !CTEST_CMD! -N
    endlocal
    exit /b 0
)

REM Run specific tests if pattern provided
if not "%TEST_PATTERN%"=="" (
    echo ========================================
    echo Running Tests Matching: %TEST_PATTERN%
    echo ========================================
    echo.

    if %VERBOSE% equ 1 (
        if %OUTPUT% equ 1 (
            !CTEST_CMD! -R "%TEST_PATTERN%" -V --output-on-failure > test_results.log 2>&1
            type test_results.log
        ) else (
            !CTEST_CMD! -R "%TEST_PATTERN%" -V --output-on-failure
        )
    ) else (
        if %OUTPUT% equ 1 (
            !CTEST_CMD! -R "%TEST_PATTERN%" --output-on-failure > test_results.log 2>&1
            type test_results.log
        ) else (
            !CTEST_CMD! -R "%TEST_PATTERN%" --output-on-failure
        )
    )

    if %errorlevel% neq 0 (
        echo.
        echo ========================================
        echo TESTS FAILED
        echo ========================================
        if %OUTPUT% equ 1 (
            echo Results saved to: %CD%\test_results.log
        )
        endlocal
        exit /b 1
    )

    echo.
    echo Tests passed successfully!
    if %OUTPUT% equ 1 (
        echo Results saved to: %CD%\test_results.log
    )
    endlocal
    exit /b 0
)

REM Run all tests
echo ========================================
echo Running All Tests
echo ========================================
echo.

if %VERBOSE% equ 1 (
    if %OUTPUT% equ 1 (
        !CTEST_CMD! -V --output-on-failure > test_results.log 2>&1
        type test_results.log
    ) else (
        !CTEST_CMD! -V --output-on-failure
    )
) else (
    if %OUTPUT% equ 1 (
        !CTEST_CMD! --output-on-failure > test_results.log 2>&1
        type test_results.log
    ) else (
        !CTEST_CMD! --output-on-failure
    )
)

if %errorlevel% neq 0 (
    echo.
    echo ========================================
    echo TESTS FAILED
    echo ========================================
    if %OUTPUT% equ 1 (
        echo Results saved to: %CD%\test_results.log
    )
    endlocal
    exit /b 1
)

echo.
echo ========================================
echo ALL TESTS PASSED
echo ========================================
if %OUTPUT% equ 1 (
    echo Results saved to: %CD%\test_results.log
)
endlocal
exit /b 0
