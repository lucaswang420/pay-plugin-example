@echo off
setlocal enabledelayedexpansion

REM ========================================
REM One-Click Build and Test Script
REM ========================================
REM This script performs a complete build and test cycle:
REM 1. Reinitialize database
REM 2. Regenerate ORM models
REM 3. Rebuild project
REM 4. Run tests
REM ========================================

echo.
echo ========================================
echo One-Click Build and Test
echo ========================================
echo.

REM Store the script directory
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%.."
set PROJECT_DIR=%CD%
echo Project directory: %PROJECT_DIR%
echo.

REM ========================================
REM Step 1: Reinitialize Database
REM ========================================
echo ========================================
echo Step 1: Reinitializing pay_test database
echo ========================================
call "%SCRIPT_DIR%setup_database.bat"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] Database initialization failed
    goto cleanup_and_exit
)
echo [SUCCESS] Database initialized
echo.

REM ========================================
REM Step 2: Regenerate ORM Models
REM ========================================
echo ========================================
echo Step 2: Regenerating ORM models
echo ========================================
call "%SCRIPT_DIR%generate_models.bat" -y
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] ORM model generation failed
    goto cleanup_and_exit
)
echo [SUCCESS] ORM models regenerated
echo.

REM ========================================
REM Step 3: Rebuild Project
REM ========================================
echo ========================================
echo Step 3: Rebuilding project
echo ========================================
call "%SCRIPT_DIR%build.bat"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] Build failed
    goto cleanup_and_exit
)
echo [SUCCESS] Project built
echo.

REM ========================================
REM Step 4: Run Tests
REM ========================================
echo ========================================
echo Step 4: Running tests
echo ========================================
call "%SCRIPT_DIR%test.bat"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] Tests failed
    goto cleanup_and_exit
)
echo [SUCCESS] All tests passed
echo.

REM ========================================
REM Success Summary
REM ========================================
echo ========================================
echo ALL STEPS COMPLETED SUCCESSFULLY!
echo ========================================
echo.
echo Summary:
echo   [1/4] Database initialization    - PASS
echo   [2/4] ORM model generation       - PASS
echo   [3/4] Project build              - PASS
echo   [4/4] Unit tests                 - PASS
echo.
echo ========================================
echo Build and Test Cycle Complete
echo ========================================
goto cleanup_and_exit

REM ========================================
REM Cleanup and Exit
REM ========================================
:cleanup_and_exit

REM Pause before exit
echo.
echo Press any key to exit...
pause >nul

endlocal
exit /b 0
