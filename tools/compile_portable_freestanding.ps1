param(
    [string]$Compiler = "g++",
    [string]$OutputDirectory = "build/portable-freestanding"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$sources = @(
    "database/portable/gx_allocator.cpp",
    "database/portable/gx_array.cpp",
    "database/portable/gx_recovered_pages.cpp",
    "database/portable/gx_page_cache.cpp",
    "database/portable/gx_owner.cpp",
    "database/portable/gx_string.cpp",
    "database/database_checksum.cpp",
    "database/portable/gx_page_codec.cpp",
    "database/portable/gx_wal_record.cpp",
    "database/portable/gx_wal_scan.cpp",
    "database/portable/gx_wal_page_image.cpp",
    "database/portable/gx_transaction_pages.cpp",
    "database/portable/gx_transaction_page_index.cpp",
    "database/portable/gx_transaction_page_overlay.cpp"
)
$flags = @(
    "-std=c++14", "-ffreestanding", "-O2", "-Wall", "-Wextra",
    "-fno-exceptions", "-fno-rtti",
    "-nostdlib", "-nostdinc++", "-fno-builtin", "-DGXOS_BARE_METAL",
    "-m64", "-mcmodel=large", "-mno-red-zone", "-mno-mmx", "-mno-sse",
    "-mno-sse2", "-fno-pic", "-fno-pie", "-fno-PIC", "-fno-PIE",
    "-mno-stack-arg-probe", "-fno-stack-check", "-fno-stack-protector",
    "-D__x86_64__",
    "-Idatabase/portable", "-Idatabase"
)

foreach ($source in $sources) {
    $sourcePath = Join-Path $repoRoot $source
    $objectPath = Join-Path $outputRoot (([IO.Path]::GetFileNameWithoutExtension($source)) + ".o")
    & $Compiler @flags -c $sourcePath -o $objectPath
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding compile failed for $source (exit $LASTEXITCODE)."
    }
}

$nm = Get-Command nm -ErrorAction SilentlyContinue
if ($nm) {
    $objects = Get-ChildItem -LiteralPath $outputRoot -Filter "*.o"
    foreach ($object in $objects) {
        $undefinedSymbols = & $nm.Source -u -C $object.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Could not inspect unresolved symbols in $($object.Name)."
        }
        # The checksum's established function-local CRC table uses the
        # guard entry points implemented by the Server kernel C++ runtime.
        $forbiddenRuntime = $undefinedSymbols | Where-Object {
            $_ -cmatch 'std::|bad_alloc|__gxx_personality|typeinfo|__dynamic_cast|_ZTI|_ZTV|malloc|calloc|realloc|free|memcpy|memmove|memset|printf|iostream' -or
            (($_ -cmatch '__cxa_') -and
             -not ($object.Name -eq "database_checksum.o" -and
                   $_ -cmatch '__cxa_guard_(acquire|release)'))
        }
        if ($forbiddenRuntime) {
            throw "Unexpected hosted/runtime symbol in $($object.Name): $forbiddenRuntime"
        }
        Write-Output "$($object.Name): $($undefinedSymbols -join ', ')"
    }
} else {
    Write-Warning "nm was not found; object compilation succeeded, symbol inspection was skipped."
}
