@echo off
REM Configuration Check Script for Windows
REM Validates PayBackend/config.json for missing or placeholder values

echo ========================================
echo PayBackend Configuration Check
echo ========================================
echo.

set CONFIG_FILE=PayBackend\config.json
set ISSUES_FOUND=0

REM Check if config file exists
if not exist "%CONFIG_FILE%" (
    echo [ERROR] ERROR: config.json not found at %CONFIG_FILE%
    exit /b 1
)

echo [PASS] config.json found
echo.

REM Check WeChat Pay placeholders
echo Checking WeChat Pay configuration...
echo.

findstr /C:"YOUR_WECHAT" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [ERROR] WeChat Pay AppID: Placeholder value detected
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] WeChat Pay AppID: Configured
)

findstr /C:"YOUR_MCH_ID" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [ERROR] Merchant ID: Placeholder value detected
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] Merchant ID: Configured
)

findstr /C:"YOUR_CERT_SERIAL" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [ERROR] Serial No: Placeholder value detected
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] Serial No: Configured
)

findstr /C:"YOUR_API_V3_KEY" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [ERROR] API v3 Key: Placeholder value detected
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] API v3 Key: Configured
)

findstr /C:"example.com" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [ERROR] Notify URL: Placeholder value detected
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] Notify URL: Configured
)

REM Check certificate directory
echo.
echo Checking certificate files...
echo.

if not exist "PayBackend\certs\" (
    echo [ERROR] Certificate directory not found
    echo    Expected path: PayBackend\certs\
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] Certificate directory exists
    
    if not exist "PayBackend\certs\apiclient_key.pem" (
        echo [ERROR] Private key certificate not found
        set /a ISSUES_FOUND+=1
    ) else (
        echo [PASS] Private key certificate found
    )
    
    if not exist "PayBackend\certs\wechatpay_platform.pem" (
        echo [ERROR] Platform certificate not found
        set /a ISSUES_FOUND+=1
    ) else (
        echo [PASS] Platform certificate found
    )
)

REM Check HTTPS
echo.
echo Checking HTTPS configuration...
echo.

findstr /C:"\"https\": false" "%CONFIG_FILE%" >/dev/null
if %errorlevel% equ 0 (
    echo [WARNING]️  HTTPS is disabled ^(HTTP only^)
    echo    Impact: All traffic is unencrypted
    echo    Recommendation: Enable for production
) else (
    echo [PASS] HTTPS is enabled
)

REM Check environment variables
echo.
echo Checking environment variables...
echo.

if "%PAY_API_KEY%%PAY_API_KEYS%"=="" (
    echo [WARNING]️  No API keys configured in environment
    echo    Set PAY_API_KEY or PAY_API_KEYS environment variable
    set /a ISSUES_FOUND+=1
) else (
    echo [PASS] API keys configured in environment
)

REM Summary
echo.
echo ========================================
echo Check Summary
echo ========================================

if %ISSUES_FOUND%==0 (
    echo [PASS] All critical configurations are valid!
    echo.
    echo [WARNING]️  Warnings may still exist. Review above for recommendations.
    exit /b 0
) else (
    echo [ERROR] Found %ISSUES_FOUND% configuration issue(s) that need attention
    echo.
    echo Next steps:
    echo 1. Review configuration documentation: docs\configuration_status.md
    echo 2. Update config.json with actual values
    echo 3. Obtain WeChat Pay credentials and certificates
    echo 4. Run this script again to verify
    exit /b 1
)
