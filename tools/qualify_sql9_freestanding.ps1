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
    "database/portable/gx_index_leaf_traversal.cpp",
    "database/portable/gx_index_internal_node.cpp",
    "database/portable/gx_index_tree_descent.cpp",
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

# The production census includes the explicit source set above; R3Z adds one
# production unit (40 to 41). This separate probe includes the freestanding
# public boundary, calls its declarations, and links the production objects.
$probeSource = "tests/freestanding/gx_index_key_boundary_probe.cpp"
$runnerSource = "tests/freestanding/gx_index_key_boundary_runner.cpp"
$probeFixture = Join-Path $repoRoot "tests/fixtures/NDB1R4P11R3Z_production_leaf_chain.hex"
if (-not (Test-Path -LiteralPath $probeFixture)) {
    throw "The R3Z production-writer leaf-chain fixture is missing: $probeFixture"
}
$probeStem = "gx_index_key_boundary_probe"
$probePath = Join-Path $repoRoot $probeSource
$runnerPath = Join-Path $repoRoot $runnerSource
$probeObject = Join-Path $outputRoot ($probeStem + ".o")
$implementationObject = Join-Path $outputRoot "database__portable__gx_index_text_key.o"
$booleanImplementationObject = Join-Path $outputRoot "database__portable__gx_index_boolean_key.o"
$integerImplementationObject = Join-Path $outputRoot "database__portable__gx_index_integer_key.o"
$physicalKeyImplementationObject = Join-Path $outputRoot "database__portable__gx_index_physical_key.o"
$leafEntryImplementationObject = Join-Path $outputRoot "database__portable__gx_index_leaf_entry.o"
$leafTraversalImplementationObject = Join-Path $outputRoot "database__portable__gx_index_leaf_traversal.o"
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
        $leafEntryImplementationObject $leafTraversalImplementationObject `
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
    "gxIndexLeafValidate",
    "gxIndexLeafLookupChain"
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
        $leafEntryImplementationObject $leafTraversalImplementationObject `
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
    $probeRunOutput = @(& $probeExecutable $probeFixture 2>&1)
    $probeRunExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
$probeRunOutput | Set-Content -LiteralPath (Join-Path $outputRoot ($probeStem + ".run.log")) -Encoding utf8
if ($probeRunExit -ne 0) {
    throw "Freestanding-compiled production index operation probe returned $probeRunExit."
}
$probeSummary = @(
    "Probe`tCompile`tRelocatableLink`tHostedExecution`tUndefinedSymbols`tProductionSymbols",
    "$probeSource`tPASS`tPASS`tPASS`tnone`t$($requiredIndexKeySymbols -join ',')"
)
$probeSummary | Set-Content -LiteralPath (Join-Path $outputRoot "index-boundary-probe.tsv") -Encoding utf8
$probeSummary

# R3AA compiles and executes the production internal-node reader and bounded
# root-to-leaf descent against a tree serialized by the hosted IndexTree
# writer. The existing key/leaf proof above remains an independent R3Z check.
$descentProbeSource = "tests/freestanding/gx_index_internal_descent_probe.cpp"
$descentRunnerSource = "tests/freestanding/gx_index_internal_descent_runner.cpp"
$descentFixture = Join-Path $repoRoot `
    "tests/fixtures/NDB1R4P11R3AA_production_internal_tree.hex"
if (-not (Test-Path -LiteralPath $descentFixture)) {
    throw "The R3AA production-writer internal-tree fixture is missing: $descentFixture"
}
$descentProbePath = Join-Path $repoRoot $descentProbeSource
$descentRunnerPath = Join-Path $repoRoot $descentRunnerSource
$descentProbeObject = Join-Path $outputRoot "gx_index_internal_descent_probe.o"
$descentLinkedObject = Join-Path $outputRoot "gx_index_internal_descent_probe.linked.o"
$descentExecutable = Join-Path $outputRoot "gx_index_internal_descent_probe.exe"
$descentProbeLog = Join-Path $outputRoot "gx_index_internal_descent_probe.compile.log"
$physicalKeyObject = Join-Path $outputRoot "database__portable__gx_index_physical_key.o"
$leafEntryObject = Join-Path $outputRoot "database__portable__gx_index_leaf_entry.o"
$leafTraversalObject = Join-Path $outputRoot "database__portable__gx_index_leaf_traversal.o"
$internalNodeObject = Join-Path $outputRoot "database__portable__gx_index_internal_node.o"
$treeDescentObject = Join-Path $outputRoot "database__portable__gx_index_tree_descent.o"

$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $descentCompileOutput = @(& $compilerCommand.Source @flags "-c" `
        $descentProbePath "-o" $descentProbeObject 2>&1)
    $descentCompileExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
$descentCompileOutput | Set-Content -LiteralPath $descentProbeLog -Encoding utf8
if ($descentCompileExit -ne 0) {
    throw "Freestanding internal-node descent probe failed to compile; see $descentProbeLog"
}

$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $descentLinkOutput = @(& $ldCommand.Source "-r" "--no-undefined" `
        $descentProbeObject $physicalKeyObject $leafEntryObject `
        $leafTraversalObject $internalNodeObject $treeDescentObject `
        "-o" $descentLinkedObject 2>&1)
    $descentLinkExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($descentLinkExit -ne 0) {
    $descentLinkOutput | Set-Content -LiteralPath `
        (Join-Path $outputRoot "gx_index_internal_descent_probe.link.log") -Encoding utf8
    throw "Freestanding internal-node descent probe failed its no-undefined link."
}

$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $descentUndefined = @(& $nmCommand.Source "-u" "-C" `
        $descentLinkedObject 2>&1)
    $descentUndefinedExit = $LASTEXITCODE
    $descentSymbols = @(& $nmCommand.Source "-g" "-C" `
        $descentLinkedObject 2>&1)
    $descentSymbolsExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($descentUndefinedExit -ne 0 -or $descentSymbolsExit -ne 0 -or
    $descentUndefined.Count -ne 0) {
    throw "Freestanding internal-node descent probe has unresolved symbols or nm failed."
}
$requiredDescentSymbols = @(
    "gxIndexInternalNodeCursorInit",
    "gxIndexInternalNodeCursorReadNext",
    "gxIndexInternalNodeSelectChild",
    "gxIndexTreeDescendToLeaf",
    "gxIndexLeafValidate",
    "gxIndexLeafLookupChain"
)
foreach ($symbol in $requiredDescentSymbols) {
    if (-not ($descentSymbols | Where-Object {
        ([string]$_) -match ("\bT\s+" + [regex]::Escape($symbol) + "(?:$|\()")
    })) {
        throw "Freestanding descent proof is missing production symbol $symbol"
    }
}

$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $descentHostLinkOutput = @(& $compilerCommand.Source $descentRunnerPath `
        $descentProbeObject $physicalKeyObject $leafEntryObject `
        $leafTraversalObject $internalNodeObject $treeDescentObject `
        "-o" $descentExecutable 2>&1)
    $descentHostLinkExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($descentHostLinkExit -ne 0) {
    $descentHostLinkOutput | Set-Content -LiteralPath `
        (Join-Path $outputRoot "gx_index_internal_descent_probe.host-link.log") -Encoding utf8
    throw "Could not build the hosted runner for the freestanding descent proof."
}
$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $descentRunOutput = @(& $descentExecutable $descentFixture 2>&1)
    $descentRunExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
$descentRunOutput | Set-Content -LiteralPath `
    (Join-Path $outputRoot "gx_index_internal_descent_probe.run.log") -Encoding utf8
if ($descentRunExit -ne 0) {
    throw "Freestanding production internal-node descent probe returned $descentRunExit."
}
$descentProbeSummary = @(
    "Probe`tCompile`tRelocatableLink`tHostedExecution`tUndefinedSymbols`tProductionSymbols",
    "$descentProbeSource`tPASS`tPASS`tPASS`tnone`t$($requiredDescentSymbols -join ',')"
)
$descentProbeSummary | Set-Content -LiteralPath `
    (Join-Path $outputRoot "index-internal-descent-probe.tsv") -Encoding utf8
$descentProbeSummary

$summaryPath = Join-Path $outputRoot "qualification.tsv"
$summary | Set-Content -LiteralPath $summaryPath -Encoding utf8
Get-Content -LiteralPath $summaryPath
Write-Output "Artifacts: $outputRoot"
