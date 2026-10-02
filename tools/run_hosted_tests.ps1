# guideXOS SQL -- Phase SQL1
# Configures, builds and runs the hosted database storage test suite.
# Usage:  powershell -ExecutionPolicy Bypass -File tools/run_hosted_tests.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"

Write-Host "Configuring $buildDir ..."
cmake -S $repoRoot -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building ..."
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running tests ..."
ctest --test-dir $buildDir --output-on-failure
exit $LASTEXITCODE
