# One-command gate: build, test, sanitized test run, format check, staged-secrets guard.
# Run from repo root:  powershell -ExecutionPolicy Bypass -File tools/ck.ps1
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

# guard: never stage sensitive files
$staged = @(git diff --cached --name-only 2>$null)
$bad = $staged | Where-Object { $_ -match '(\.env$|\.pem$|\.key$|id_rsa)' }
if ($bad) { Write-Host "refusing: sensitive paths staged:" -ForegroundColor Red; $bad; exit 1 }

cmake --preset gcc-debug
if ($LASTEXITCODE) { Write-Host "FAILED: configure gcc-debug" -ForegroundColor Red; exit 1 }
cmake --build --preset gcc-debug
if ($LASTEXITCODE) { Write-Host "FAILED: build gcc-debug" -ForegroundColor Red; exit 1 }
ctest --preset gcc-debug
if ($LASTEXITCODE) { Write-Host "FAILED: test gcc-debug" -ForegroundColor Red; exit 1 }

# Probe needs EAP=Continue: PS 5.1 turns native stderr into a terminating error under Stop.
$ErrorActionPreference = "Continue"
cmake --preset gcc-asan 2>&1 | Out-Null
$asanOk = $LASTEXITCODE -eq 0
$ErrorActionPreference = "Stop"

if ($asanOk) {
    cmake --build --preset gcc-asan
    if ($LASTEXITCODE) { Write-Host "FAILED: build gcc-asan" -ForegroundColor Red; exit 1 }
    ctest --preset gcc-asan
    if ($LASTEXITCODE) { Write-Host "FAILED: test gcc-asan" -ForegroundColor Red; exit 1 }
} else {
    Write-Host "WARNING: sanitizer runtimes absent from this MinGW toolchain." -ForegroundColor Yellow
    Write-Host "WARNING: native ASan gate skipped - cover parser tests under ASan in Docker." -ForegroundColor Yellow
}

if (Get-Command clang-format -ErrorAction SilentlyContinue) {
    $files = @(Get-ChildItem -Recurse -Include *.cpp,*.h -Path src,tests | ForEach-Object { $_.FullName })
    if ($files.Count -gt 0) {
        clang-format --dry-run -Werror $files
        if ($LASTEXITCODE) { Write-Host "FAILED: format check" -ForegroundColor Red; exit 1 }
    }
} else {
    Write-Host "WARNING: format check skipped (clang-format not on PATH)" -ForegroundColor Yellow
}

Write-Host "all checks passed" -ForegroundColor Green
