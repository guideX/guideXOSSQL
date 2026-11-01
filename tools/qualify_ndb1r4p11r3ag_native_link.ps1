param(
    [string]$ServerSourceDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "..\guideXOSServer_NATIVE_DB_STORAGE"),
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\r3ag-native-link-runs")
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$serverRoot = [IO.Path]::GetFullPath($ServerSourceDir)
$serverKernel = Join-Path $serverRoot "kernel"
$serverBuild = Join-Path $serverKernel "build\amd64"
$serverObjectsRoot = Join-Path $serverBuild "obj"
$outputRoot = [IO.Path]::GetFullPath($OutputRoot)
$serverPrefix = $serverRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($outputRoot.StartsWith($serverPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputRoot must remain outside the read-only Server checkout."
}

$requiredPaths = @(
    (Join-Path $serverKernel "core\include\kernel\native_database_file.h"),
    (Join-Path $serverKernel "core\native_database_file.cpp"),
    (Join-Path $serverKernel "core\vfs.cpp"),
    (Join-Path $serverBuild "bin\kernel.pe"),
    (Join-Path $serverBuild "bin\kernel.elf"),
    (Join-Path $serverObjectsRoot "core\native_database_file.o"),
    (Join-Path $serverObjectsRoot "core\vfs.o")
)
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required checked Server source/build artifact is missing: $requiredPath"
    }
}
foreach ($pair in @(
    @{
        Source = Join-Path $serverKernel "core\native_database_file.cpp"
        Object = Join-Path $serverObjectsRoot "core\native_database_file.o"
    },
    @{
        Source = Join-Path $serverKernel "core\vfs.cpp"
        Object = Join-Path $serverObjectsRoot "core\vfs.o"
    }
)) {
    if ((Get-Item -LiteralPath $pair.Source).LastWriteTimeUtc -gt
        (Get-Item -LiteralPath $pair.Object).LastWriteTimeUtc) {
        throw "Server production object is older than its source: $($pair.Object)"
    }
}

$runRoot = Join-Path $outputRoot ("NDB1R4P11R3AG_" +
    [DateTime]::UtcNow.ToString("yyyyMMdd_HHmmss_ffff") + "_" +
    [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

function Invoke-LoggedTool {
    param(
        [string]$Tool,
        [string[]]$Arguments,
        [string]$LogPath,
        [string]$FailureText
    )
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $toolOutput = @(& $Tool @Arguments 2>&1)
        $toolExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    $toolOutput | Set-Content -LiteralPath $LogPath -Encoding utf8
    if ($toolExit -ne 0) {
        throw "$FailureText (exit $toolExit); see $LogPath"
    }
    return ,$toolOutput
}

function Get-UndefinedSymbols {
    param([string]$Path, [string]$NmTool, [string]$LogPath)
    $lines = Invoke-LoggedTool $NmTool @("-u", "-C", $Path) $LogPath `
        "Could not inspect undefined symbols in $Path"
    return @($lines | ForEach-Object { ([string]$_).Trim() } |
        Where-Object { $_ } | Sort-Object -Unique)
}

$compiler = (Get-Command "g++" -ErrorAction Stop).Source
$linker = (Get-Command "ld" -ErrorAction Stop).Source
$nm = (Get-Command "nm" -ErrorAction Stop).Source
$objcopy = (Get-Command "objcopy" -ErrorAction Stop).Source
$flags = @(
    "-std=c++14", "-ffreestanding", "-O2", "-Wall", "-Wextra",
    "-fno-exceptions", "-fno-rtti", "-nostdlib", "-nostdinc++",
    "-fno-builtin", "-DGXOS_BARE_METAL", "-m64", "-mcmodel=large",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-fno-pic", "-fno-pie", "-fno-PIC", "-fno-PIE",
    "-mno-stack-arg-probe", "-fno-stack-check", "-fno-stack-protector",
    "-D__x86_64__", "-I$serverKernel", "-I$serverRoot",
    "-I$(Join-Path $serverKernel 'core\include')",
    "-I$(Join-Path $serverKernel 'arch\amd64\include')",
    "-Idatabase\native", "-Idatabase\portable", "-Idatabase"
)

$sources = @(
    "database/native/gx_native_sql_index_page_provider.cpp",
    "database/database_checksum.cpp",
    "database/database_format.cpp",
    "database/portable/gx_page_codec.cpp",
    "database/portable/gx_allocator.cpp",
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
    "database/portable/gx_index_lookup.cpp"
)
$sqlObjects = [Collections.Generic.List[string]]::new()
$compileRows = [Collections.Generic.List[string]]::new()
Push-Location $repoRoot
try {
    foreach ($source in $sources) {
        $sourcePath = Join-Path $repoRoot $source
        $stem = $source -replace '[\\/:]', '__' -replace '\.cpp$', ''
        $objectPath = Join-Path $runRoot ($stem + ".o")
        $logPath = Join-Path $runRoot ($stem + ".compile.log")
        [void](Invoke-LoggedTool $compiler (@($flags) + @("-c", $sourcePath, "-o", $objectPath)) `
            $logPath "Freestanding compilation failed for $source")
        $sqlObjects.Add($objectPath)
        $compileRows.Add("$source`tPASS")
    }

    # Compile the exact Server bridge source to prove source compatibility.
    # The kernel link below uses the existing production object from the
    # checked Server kernel build, never this duplicate qualification object.
    $bridgeSource = Join-Path $serverKernel "core\native_database_file.cpp"
    $bridgeObject = Join-Path $runRoot "native_database_file.source.o"
    [void](Invoke-LoggedTool $compiler (@($flags) + @(
        "-c", $bridgeSource, "-o", $bridgeObject
    )) (Join-Path $runRoot "native_database_file.source.compile.log") `
        "Freestanding compilation failed for the Server file bridge")
    $bridgeExports = @(& $nm "-g" $bridgeObject | ForEach-Object {
        $line = ([string]$_).Trim()
        if ($line -match '\s[Tt]\s+(.+)$') { $Matches[1] }
    } | Sort-Object -Unique)
    $productionBridgeObject = Join-Path $serverObjectsRoot "core\native_database_file.o"
    $productionExports = @(& $nm "-g" $productionBridgeObject | ForEach-Object {
        $line = ([string]$_).Trim()
        if ($line -match '\s[Tt]\s+(.+)$') { $Matches[1] }
    } | Sort-Object -Unique)
    $bridgeExportDiff = Compare-Object $bridgeExports $productionExports
    if ($bridgeExportDiff) {
        throw "Fresh bridge source exports differ from the selected production object."
    }
    foreach ($symbol in @("gx_db_file_open", "gx_db_file_get_size",
                          "gx_db_file_read_at", "gx_db_file_close")) {
        if ($symbol -notin $productionExports) {
            throw "Production Server bridge object does not export $symbol"
        }
    }
    $compileRows.Add("kernel/core/native_database_file.cpp`tPASS_SOURCE_AND_OBJECT_ABI")

$forbiddenPattern = 'std::|__gxx_personality|typeinfo|__dynamic_cast|_ZTI|_ZTV|\b(malloc|calloc|realloc|free|memcpy|memmove|memset|printf)\b|iostream'
    foreach ($objectPath in $sqlObjects) {
        $undefined = Get-UndefinedSymbols $objectPath $nm `
            (Join-Path $runRoot ((Split-Path -Leaf $objectPath) + ".undefined.log"))
        $forbidden = @($undefined | Where-Object { $_ -match $forbiddenPattern })
        if ($forbidden) {
            throw "Hosted runtime dependency in $(Split-Path -Leaf $objectPath): $($forbidden -join ', ')"
        }
    }

    $serverObjectFiles = @(Get-ChildItem -LiteralPath $serverObjectsRoot `
        -Recurse -File -Filter "*.o" | Where-Object {
            # This stale test-only assembly object is absent from the current
            # Server source tree and has a second _start symbol.
            $_.Name -ne "qemu_ata_multiboot_boot.o"
        } | Select-Object -ExpandProperty FullName)
    foreach ($requiredObject in @(
        (Join-Path $serverObjectsRoot "core\native_database_file.o"),
        (Join-Path $serverObjectsRoot "core\vfs.o")
    )) {
        if ($requiredObject -notin $serverObjectFiles) {
            throw "Production Server kernel object set omitted $requiredObject"
        }
    }

    $baselineKernel = Join-Path $serverBuild "bin\kernel.pe"
    $candidatePe = Join-Path $runRoot "guideXOSKernelR3AG.pe"
    $candidateElf = Join-Path $runRoot "guideXOSKernelR3AG.elf"
    $libgcc = (& $compiler "-print-libgcc-file-name").Trim()
    $linkArguments = @(
        "-T", (Join-Path $serverKernel "arch\amd64\linker.ld"),
        "-no-pie", "--image-base=0x100000", "-o", $candidatePe
    ) + $serverObjectFiles + @($sqlObjects) + @($libgcc)
    [void](Invoke-LoggedTool $linker $linkArguments `
        (Join-Path $runRoot "kernel-link.log") `
        "The R3AG component failed to link with the checked Server kernel objects")

    $objcopyArguments = @(
        "-I", "pei-x86-64", "-O", "elf64-x86-64",
        "--change-section-address", ".boot+0x100000",
        "--change-section-address", ".text+0",
        "--change-section-address", ".rdata+0",
        "--change-section-address", ".data+0",
        "--change-section-address", ".bss+0",
        "--change-section-address", ".reloc+0",
        "--set-start", "0x100000", $candidatePe, $candidateElf
    )
    [void](Invoke-LoggedTool $objcopy $objcopyArguments `
        (Join-Path $runRoot "kernel-objcopy.log") `
        "Could not produce the AMD64 ELF kernel image")

    $baselineUndefined = Get-UndefinedSymbols $baselineKernel $nm `
        (Join-Path $runRoot "baseline-kernel.undefined.log")
    $candidateUndefined = Get-UndefinedSymbols $candidatePe $nm `
        (Join-Path $runRoot "candidate-kernel.undefined.log")
    $baselineUndefined | Set-Content -LiteralPath `
        (Join-Path $runRoot "baseline-kernel.undefined.unique.txt") -Encoding utf8
    $candidateUndefined | Set-Content -LiteralPath `
        (Join-Path $runRoot "candidate-kernel.undefined.unique.txt") -Encoding utf8
    $undefinedDiff = Compare-Object $baselineUndefined $candidateUndefined
    if ($undefinedDiff) {
        $undefinedDiff | Out-String | Set-Content -LiteralPath `
            (Join-Path $runRoot "kernel.undefined-diff.log") -Encoding utf8
        throw "Candidate kernel introduced or removed unresolved/import symbols."
    }

    $linkedSymbols = @(& $nm "-g" "-C" $candidateElf | ForEach-Object {
        ([string]$_).Trim()
    })
    foreach ($symbol in @(
        "gxNativeSqlIndexPageProviderOpen",
        "gxNativeSqlIndexPageProviderClose",
        "gxIndexLookupAll",
        "gxIndexRangeScan",
        "gx_db_file_open",
        "gx_db_file_get_size",
        "gx_db_file_read_at",
        "gx_db_file_close",
        "kernel::vfs::open",
        "kernel::vfs::read",
        "kernel::vfs::seek",
        "kernel::vfs::file_size"
    )) {
        if (-not ($linkedSymbols | Where-Object { $_ -match [regex]::Escape($symbol) })) {
            throw "Candidate kernel is missing linked production symbol $symbol"
        }
    }
    $linkedSymbols | Where-Object {
        $_ -match 'gxNativeSqlIndexPageProvider(Open|Close|Bind)|gxIndexLookupAll|gxIndexRangeScan|gx_db_file_(open|get_size|read_at|close)|kernel::vfs::(open|read|seek|file_size)'
    } | Set-Content -LiteralPath (Join-Path $runRoot "native-index-linked-symbols.txt") -Encoding utf8

    $serverHead = (& git -C $serverRoot rev-parse HEAD).Trim()
    $serverStatus = @(& git -C $serverRoot status --short)
    if ($serverStatus.Count -ne 0) {
        throw "The checked Server source worktree is not clean; refusing a source attribution claim."
    }
    $compilerVersion = @(& $compiler "--version" | Select-Object -First 1)
    $summary = @(
        "Evidence`tResult",
        "SQL_HEAD`t$((& git -C $repoRoot rev-parse HEAD).Trim())",
        "SERVER_HEAD`t$serverHead",
        "SERVER_WORKTREE`tCLEAN_TRACKED_AND_UNTRACKED",
        "COMPILER`t$compilerVersion",
        "AMD64_FREESTANDING_COMPILE`t$($compileRows.Count)_SOURCES_PASS",
        "SERVER_BRIDGE_SOURCE_COMPILE`tPASS",
        "SERVER_BRIDGE_OBJECT_EXPORT_MATCH`tPASS",
        "SERVER_OBJECTS_LINKED`t$($serverObjectFiles.Count)",
        "KERNEL_LINK`tPASS_TO_PE_AND_ELF",
        "BASELINE_UNDEFINED_IMPORTS`t$($baselineUndefined.Count)",
        "CANDIDATE_UNDEFINED_IMPORTS`t$($candidateUndefined.Count)",
        "SQL_ADDED_UNDEFINED_IMPORTS`t0",
        "GUEST_EXECUTION`tNOT_RUN",
        "ARTIFACT_DIRECTORY`t$runRoot"
    )
    $summary | Set-Content -LiteralPath (Join-Path $runRoot "qualification.tsv") -Encoding utf8
    $compileRows | Set-Content -LiteralPath (Join-Path $runRoot "compiled-sources.tsv") -Encoding utf8
    Get-Content -LiteralPath (Join-Path $runRoot "qualification.tsv")
} finally {
    Pop-Location
}
