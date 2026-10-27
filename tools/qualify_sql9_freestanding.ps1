param(
    [string]$Compiler = "g++",
    [string]$ServerKernelRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "guideXOSServer_NATIVE_DB_STORAGE\kernel"),
    [string]$OutputDirectory = (Join-Path $env:TEMP "guideXOSSql-NDB1R4P9")
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = [IO.Path]::GetFullPath(
    $(if ([IO.Path]::IsPathRooted($OutputDirectory)) {
        $OutputDirectory
    } else {
        Join-Path $repoRoot $OutputDirectory
    }))
$repoPrefix = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($outputRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must be outside the SQL checkout: $outputRoot"
}
if (Test-Path -LiteralPath $outputRoot) {
    if (Get-ChildItem -LiteralPath $outputRoot -Force | Select-Object -First 1) {
        throw "OutputDirectory must be fresh and empty; refusing to overwrite: $outputRoot"
    }
} else {
    New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
}
if (-not (Test-Path -LiteralPath (Join-Path $ServerKernelRoot "arch\amd64\Makefile.arch"))) {
    throw "ServerKernelRoot does not contain the AMD64 target configuration: $ServerKernelRoot"
}
$serverKernelRoot = [IO.Path]::GetFullPath($ServerKernelRoot)
$serverRoot = Split-Path -Parent $serverKernelRoot

$sources = @(
    "database/database_format.cpp",
    "database/database_checksum.cpp",
    "database/database_io.cpp",
    "database/database_native_io.cpp",
    "database/database_header.cpp",
    "database/database_page.cpp",
    "database/database_file.cpp",
    "database/database_engine.cpp",
    "database/database_buffer.cpp",
    "database/database_catalog.cpp",
    "database/database_index.cpp",
    "database/database_heap.cpp",
    "database/database_wal.cpp",
    "database/portable/gx_allocator.cpp",
    "database/portable/gx_allocator_hosted.cpp",
    "database/portable/gx_array.cpp",
    "database/portable/gx_page_visit_set.cpp",
    "database/portable/gx_index_boolean_key.cpp",
    "database/portable/gx_index_integer_key.cpp",
    "database/portable/gx_index_text_key.cpp",
    "database/portable/gx_index_physical_key.cpp",
    "database/portable/gx_index_leaf_entry.cpp",
    "database/portable/gx_owned_page.cpp",
    "database/portable/gx_heap_page_builder.cpp",
    "database/portable/gx_recovered_pages.cpp",
    "database/portable/gx_page_cache.cpp",
    "database/portable/gx_page_codec.cpp",
    "database/portable/gx_wal_record.cpp",
    "database/portable/gx_wal_scan.cpp",
    "database/portable/gx_wal_page_image.cpp",
    "database/portable/gx_transaction_pages.cpp",
    "database/portable/gx_transaction_page_index.cpp",
    "database/portable/gx_transaction_page_overlay.cpp",
    "database/database_transaction.cpp",
    "database/database_relational.cpp",
    "database/database_sql_token.cpp",
    "database/database_sql_tokenizer.cpp",
    "database/database_sql_parser.cpp",
    "database/database_sql_expr.cpp",
    "database/database_sql.cpp"
)

# Mirrors kernel/arch/amd64/Makefile.arch and the common CFLAGS in
# kernel/Makefile, with only SQL's repository include roots substituted.
$flags = @(
    "-std=c++14", "-ffreestanding", "-O2", "-Wall", "-Wextra",
    "-fno-exceptions", "-fno-rtti", "-nostdlib", "-nostdinc++",
    "-fno-builtin", "-DGXOS_BARE_METAL", "-m64", "-mcmodel=large",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-fno-pic", "-fno-pie", "-fno-PIC", "-fno-PIE",
    "-mno-stack-arg-probe", "-fno-stack-check", "-fno-stack-protector",
    "-D__x86_64__", "-I$serverKernelRoot", "-I$serverRoot",
    "-I$(Join-Path $serverKernelRoot 'core\include')",
    "-I$(Join-Path $serverKernelRoot 'arch\amd64\include')",
    "-Idatabase/portable", "-Idatabase",
    "-fdiagnostics-color=never", "-H"
)

$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$nmCommand = Get-Command -Name "nm" -ErrorAction SilentlyContinue
$summary = [Collections.Generic.List[string]]::new()
$summary.Add("Source`tCompile`tFirstDiagnostic`tUndefinedSymbols")
$sources | Set-Content -LiteralPath (Join-Path $outputRoot "production-sources.txt") -Encoding utf8

Push-Location $repoRoot
try {
    foreach ($source in $sources) {
        $sourcePath = Join-Path $repoRoot $source
        $stem = $source -replace '[\\/:]', '__' -replace '\.cpp$', ''
        $objectPath = Join-Path $outputRoot ($stem + ".o")
        $logPath = Join-Path $outputRoot ($stem + ".compile.log")
        # Windows PowerShell 5.1 turns native stderr into ErrorRecords. With
        # ErrorActionPreference=Stop, the normal -H include trace terminates
        # this script before the compiler's exit code and diagnostics can be
        # recorded. Capture native output under Continue, then restore Stop for
        # the script's own PowerShell operations.
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $compileOutput = @(& $compilerCommand.Source @flags "-c" $sourcePath "-o" $objectPath 2>&1)
            $compileExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        $compileOutput | Set-Content -LiteralPath $logPath -Encoding utf8
        $compileResult = if ($compileExit -eq 0) { "PASS" } else { "FAIL" }
        $firstDiagnostic = ""
        if ($compileExit -ne 0) {
            $errorLine = Select-String -LiteralPath $logPath `
                -Pattern 'fatal error:| error:|undefined reference' |
                Select-Object -First 1
            if ($errorLine) { $firstDiagnostic = $errorLine.Line.Trim() }
        }

        $undefined = ""
        if ($compileExit -eq 0 -and $nmCommand) {
            $savedErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $nmOutput = @(& $nmCommand.Source "-u" "-C" $objectPath 2>&1)
                $nmExit = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $savedErrorActionPreference
            }
            if ($nmExit -ne 0) {
                throw "nm failed for $source; see $outputRoot"
            }
            $nmOutput | Set-Content -LiteralPath (Join-Path $outputRoot ($stem + ".symbols.txt")) -Encoding utf8
            $undefined = (($nmOutput | ForEach-Object { ([string]$_).Trim() }) -join " | ")
        }
        $summary.Add("$source`t$compileResult`t$firstDiagnostic`t$undefined")
    }
} finally {
    Pop-Location
}

# The production census includes the explicit source set above; R3X adds one
# production unit (39 to 40). This separate probe includes the freestanding
# public boundary, calls its declarations, and links the production objects.
$probeSource = "tests/freestanding/gx_index_key_boundary_probe.cpp"
$runnerSource = "tests/freestanding/gx_index_key_boundary_runner.cpp"
$probeStem = "gx_index_key_boundary_probe"
$probePath = Join-Path $repoRoot $probeSource
$runnerPath = Join-Path $repoRoot $runnerSource
$probeObject = Join-Path $outputRoot ($probeStem + ".o")
$implementationObject = Join-Path $outputRoot "database__portable__gx_index_text_key.o"
$booleanImplementationObject = Join-Path $outputRoot "database__portable__gx_index_boolean_key.o"
$integerImplementationObject = Join-Path $outputRoot "database__portable__gx_index_integer_key.o"
$physicalKeyImplementationObject = Join-Path $outputRoot "database__portable__gx_index_physical_key.o"
$leafEntryImplementationObject = Join-Path $outputRoot "database__portable__gx_index_leaf_entry.o"
$linkedObject = Join-Path $outputRoot ($probeStem + ".linked.o")
$probeExecutable = Join-Path $outputRoot ($probeStem + ".exe")
$probeLog = Join-Path $outputRoot ($probeStem + ".compile.log")

Push-Location $repoRoot
try {
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $probeOutput = @(& $compilerCommand.Source @flags "-c" $probePath "-o" $probeObject 2>&1)
    $probeCompileExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
$probeOutput | Set-Content -LiteralPath $probeLog -Encoding utf8
if ($probeCompileExit -ne 0) {
    throw "Freestanding index-key probe failed to compile; see $probeLog"
}
} finally {
    Pop-Location
}

$ldCommand = Get-Command -Name "ld" -ErrorAction Stop
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $linkOutput = @(& $ldCommand.Source "-r" "--no-undefined" $probeObject `
        $implementationObject $integerImplementationObject `
        $booleanImplementationObject $physicalKeyImplementationObject `
        $leafEntryImplementationObject `
        "-o" $linkedObject 2>&1)
    $linkExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($linkExit -ne 0) {
    $linkOutput | Set-Content -LiteralPath (Join-Path $outputRoot ($probeStem + ".link.log")) -Encoding utf8
    throw "Freestanding index-key probe failed to link against the production implementation."
}

if (-not $nmCommand) {
    throw "nm is required to verify portable index-key symbols and linkage."
}
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $probeUndefined = @(& $nmCommand.Source "-u" "-C" $linkedObject 2>&1)
    $probeUndefinedExit = $LASTEXITCODE
    $probeSymbols = @(& $nmCommand.Source "-g" "-C" $linkedObject 2>&1)
    $probeSymbolsExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($probeUndefinedExit -ne 0 -or $probeSymbolsExit -ne 0) {
    throw "nm failed while inspecting the freestanding index-key probe."
}
if ($probeUndefined.Count -ne 0) {
    throw "Freestanding index-key probe has unresolved symbols: $($probeUndefined -join ' | ')"
}
$requiredIndexKeySymbols = @(
    "gxEncodeIndexTextKey",
    "gxDecodeIndexTextKey",
    "gxEncodeIndexBooleanKey",
    "gxDecodeIndexBooleanKey",
    "gxEncodeIndexInt32Key",
    "gxDecodeIndexInt32Key",
    "gxEncodeIndexInt64Key",
    "gxDecodeIndexInt64Key",
    "gxIndexKeyIsNull",
    "gxCompareIndexKeys",
    "gxCompareIndexPhysicalKeyViews",
    "gxIndexLeafCursorInit",
    "gxIndexLeafCursorReadNext",
    "gxIndexLeafLookup",
    "gxIndexLeafValidate"
)
foreach ($symbol in $requiredIndexKeySymbols) {
    if (-not ($probeSymbols | Where-Object { ([string]$_) -match ("\bT\s+" + [regex]::Escape($symbol) + "(?:$|\()") })) {
        throw "Freestanding index-key probe is missing production symbol $symbol"
    }
}

# Execute the same freestanding-compiled probe and production objects as a
# hosted test executable. The relocatable no-undefined link above remains the
# check for unresolved production/runtime symbols.
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $hostLinkOutput = @(& $compilerCommand.Source $runnerPath $probeObject `
        $implementationObject $integerImplementationObject `
        $booleanImplementationObject $physicalKeyImplementationObject `
        $leafEntryImplementationObject `
        "-o" $probeExecutable 2>&1)
    $hostLinkExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($hostLinkExit -ne 0) {
    $hostLinkOutput | Set-Content -LiteralPath (Join-Path $outputRoot ($probeStem + ".host-link.log")) -Encoding utf8
    throw "Could not build the hosted runner for the freestanding production probe."
}
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $null = & $probeExecutable 2>&1
    $probeRunExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($probeRunExit -ne 0) {
    throw "Freestanding-compiled production index operation probe returned $probeRunExit."
}
$probeSummary = @(
    "Probe`tCompile`tRelocatableLink`tHostedExecution`tUndefinedSymbols`tProductionSymbols",
    "$probeSource`tPASS`tPASS`tPASS`tnone`t$($requiredIndexKeySymbols -join ',')"
)
$probeSummary | Set-Content -LiteralPath (Join-Path $outputRoot "index-boundary-probe.tsv") -Encoding utf8
$probeSummary

$summaryPath = Join-Path $outputRoot "qualification.tsv"
$summary | Set-Content -LiteralPath $summaryPath -Encoding utf8
Get-Content -LiteralPath $summaryPath
Write-Output "Artifacts: $outputRoot"
