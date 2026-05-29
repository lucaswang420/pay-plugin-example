param(
    [string]$ExePath
)

if (-not $ExePath -or $ExePath.Trim().Length -eq 0) {
    Write-Host "Missing PayBackendTests.exe path."
    exit 1
}

if (-not (Test-Path $ExePath)) {
    Write-Host "PayBackendTests.exe not found at: $ExePath"
    exit 1
}

$exeDir = Split-Path $ExePath -Parent
Set-Location $exeDir

$rawOutput = & $ExePath -l 2>$null
# Filter out Drogon log lines (timestamps) and keep only test names
$tests = @()
foreach ($line in $rawOutput) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0) { continue }
    if ($trimmed -eq "Available Tests:") { continue }
    # Skip Drogon log lines (they start with timestamp like "20260528 ...")
    if ($trimmed -match '^\d{8}\s+\d{2}:\d{2}:') { continue }
    $tests += $trimmed
}
if ($tests.Count -eq 0) {
    Write-Host "Failed to list tests."
    exit 1
}

foreach ($t in $tests) {
    Write-Host "Running $t"
    & $ExePath -r $t
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) {
        $exitCode = if ($?) { 0 } else { 1 }
    }
    if ($exitCode -ne 0) {
        Write-Host "FAILED $t"
        exit $exitCode
    }
}

Write-Host "All tests passed."
exit 0
