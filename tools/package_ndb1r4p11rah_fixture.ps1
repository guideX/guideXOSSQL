param(
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) "tests\fixtures\NDB1R4P11R3AH"),
    [Parameter(Mandatory = $true)]
    [string]$ServerBaselineCommit
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$generator = Join-Path $repoRoot "build\database_ndb1r4p11r3ag_test.exe"
$output = [IO.Path]::GetFullPath($OutputDirectory)

if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
    throw "Build database_ndb1r4p11r3ag_test before packaging the fixture: $generator"
}
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite an existing fixture package: $output"
}

$sqlWriterCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sqlWriterCommit -notmatch '^[0-9a-f]{40}$') {
    throw "Could not resolve the SQL source commit."
}
if ($ServerBaselineCommit -notmatch '^[0-9a-f]{40}$') {
    throw "ServerBaselineCommit must be a full 40-character commit ID."
}

& $generator --package-fixture $output $sqlWriterCommit $ServerBaselineCommit
if ($LASTEXITCODE -ne 0) {
    throw "Fixture packaging failed with exit code $LASTEXITCODE."
}
