<#
.SYNOPSIS
    Unpacks a dependency bundle produced by package-deps.ps1 and points CMake at it.

.DESCRIPTION
    UIFE's optional backends are not vendored in this repository. On a fresh
    machine, copy the dependency archive produced by scripts/package-deps.ps1
    (for example over a network share or a release asset), then run this script.
    It unpacks the archive and writes a CMake initial-cache file, so a build
    needs no manual -DZLIB_LIBRARY / -DLIBTSK_LIBRARY ... arguments:

        cmake -S . -B build -G Ninja -C deps/uife-deps.cmake

    Missing optional components are reported but do not fail: CMake degrades the
    corresponding format to "detected but not browsable". zlib is mandatory, so
    a bundle without zlib is rejected.

.PARAMETER ArchivePath
    Dependency archive. Defaults to <repo>\deps\uife-deps-mingw-ucrt64.zip.

.PARAMETER Destination
    Directory the archive is unpacked into. Defaults to <repo>\deps\sdk.

.PARAMETER CMakeCacheFile
    Generated initial-cache file. Defaults to <repo>\deps\uife-deps.cmake.

.PARAMETER CompilerPath
    Compiler used to warn about ABI mismatches against manifest.json.

.PARAMETER Force
    Unpack again even if the destination already looks populated.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1 `
        -ArchivePath D:\transfer\uife-deps.zip -Destination D:\deps
#>
[CmdletBinding()]
param(
    [string]$ArchivePath = "",
    [string]$Destination = "",
    [string]$CMakeCacheFile = "",
    [string]$CompilerPath = "D:\tools\mingw64\bin\g++.exe",
    [switch]$Shared,
    [switch]$Force
)

# Defaults are resolved here rather than in the param block: Windows PowerShell
# 5.1 leaves $PSScriptRoot empty while parameter defaults are evaluated, so the
# documented `powershell -File scripts\fetch-deps.ps1` died on
# "Join-Path: Cannot bind argument to parameter 'Path' because it is an empty
# string". $PSScriptRoot is populated by the time the body runs.
if ([string]::IsNullOrEmpty($PSScriptRoot)) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    $scriptRoot = $PSScriptRoot
}
if ([string]::IsNullOrEmpty($ArchivePath)) {
    $ArchivePath = [System.IO.Path]::GetFullPath(
        (Join-Path $scriptRoot "..\deps\uife-deps-mingw-ucrt64.zip"))
}
if ([string]::IsNullOrEmpty($Destination)) {
    $Destination = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot "..\deps\sdk"))
}
if ([string]::IsNullOrEmpty($CMakeCacheFile)) {
    $CMakeCacheFile = [System.IO.Path]::GetFullPath(
        (Join-Path $scriptRoot "..\deps\uife-deps.cmake"))
}

# static: every libyal component links its own bundled libbfio (community build).
# shared: every libyal component is a DLL over one libbfio-1.dll (the layout the
# closed-source build needs for LGPL-3.0 4(d)(1)). Never both - see Find-Library.
$LinkForm = if ($Shared) { "shared" } else { "static" }

$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host "[deps] $Message" -ForegroundColor Cyan
}

function ConvertTo-CMakePath {
    param([string]$Path)
    return ((Resolve-Path -LiteralPath $Path).Path -replace '\\', '/')
}

function Find-StaticLibrary {
    param([string]$Directory, [string]$Name)

    foreach ($candidate in @("lib$Name.a", "lib$Name.dll.a", "$Name.lib")) {
        $path = Join-Path $Directory $candidate
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return $null
}

# 每个 libyal 组件在包里都同时有静态归档和导入库，必须**整体选同一种形态**。
#
# 混着链接会崩，而且崩得毫无指向性：静态 libewf 把 libbfio 的实现编进了 .a，共享
# libfsapfs 用的是 libbfio-1.dll，同一个进程里就有了两份 libbfio。uife 用一份建句柄、
# 传给另一份去读，结构体布局对不上，最后死在 libbfio_handle_is_open 的读写锁上
# （实测：SIGSEGV in RtlAcquireSRWLockShared，栈顶是 ApfsFsParser::mount）。
function Find-Library {
    param([string]$Directory, [string]$Name, [string]$Form)

    $static = Find-StaticLibrary -Directory $Directory -Name $Name
    if ($null -ne $static -and $static -like "*.dll.a") { $static = $null }

    $shared = Join-Path $Directory "lib$Name.dll.a"
    if (-not (Test-Path -LiteralPath $shared)) { $shared = $null }

    if ($Form -eq "shared" -and $null -ne $shared) { return $shared }
    if ($null -ne $static) { return $static }
    return $shared
}

function Get-CompilerDescription {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return "unknown" }
    $firstLine = (& $Path --version 2>$null | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($firstLine)) { return "unknown" }
    return $firstLine.Trim()
}

if (-not (Test-Path -LiteralPath $ArchivePath)) {
    $message = "Dependency archive not found: {0}`n" -f $ArchivePath
    $message += "Create it on a machine that already has the SDKs with:`n"
    $message += "  powershell -ExecutionPolicy Bypass -File scripts\package-deps.ps1"
    throw $message
}

$alreadyThere = (Test-Path -LiteralPath $Destination) -and
    ((Get-ChildItem -LiteralPath $Destination -Directory -ErrorAction SilentlyContinue).Count -gt 0)
if ($alreadyThere -and -not $Force) {
    Write-Step "destination already populated, skipping unpack: $Destination (use -Force to redo)"
}
else {
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    $null = New-Item -ItemType Directory -Path $Destination -Force
    Write-Step "unpacking $ArchivePath"
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Destination -Force
}

$manifestPath = Join-Path $Destination "manifest.json"
if (Test-Path -LiteralPath $manifestPath) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Write-Step ("bundle: {0} ({1}), built {2}" -f $manifest.triplet, $manifest.generated, $manifest.compiler)

    $current = Get-CompilerDescription -Path $CompilerPath
    if ($manifest.compiler -ne $current) {
        Write-Warning ("ABI mismatch risk: bundle was built with '$($manifest.compiler)', " +
            "this machine has '$current'. Static libraries from a different toolchain will not link.")
    }
}
else {
    Write-Warning "manifest.json missing; cannot verify ABI compatibility"
}

$entries = New-Object System.Collections.Generic.List[string]
function Add-CacheEntry {
    param([string]$Variable, [string]$Value, [string]$Type = "FILEPATH")
    $script:entries.Add(('set({0} "{1}" CACHE {2} "" FORCE)' -f $Variable, $Value, $Type))
}

function Add-Component {
    param([string]$Name, [string]$LibraryName, [string]$Form = "static")

    $root = Join-Path $Destination $Name
    $includeDirectory = Join-Path $root "include"
    $libraryDirectory = Join-Path $root "lib"

    if (-not (Test-Path -LiteralPath $includeDirectory)) {
        Write-Warning "skip ${Name}: no include directory in bundle"
        return $false
    }

    $library = $null
    if (Test-Path -LiteralPath $libraryDirectory) {
        $library = Find-Library -Directory $libraryDirectory -Name $LibraryName -Form $Form
    }
    if ($null -eq $library) {
        Write-Warning "skip ${Name}: no import/static library in bundle"
        return $false
    }

    return [pscustomobject]@{
        Root      = (ConvertTo-CMakePath $root)
        Include   = (ConvertTo-CMakePath $includeDirectory)
        Library   = (ConvertTo-CMakePath $library)
        LibraryDir = (ConvertTo-CMakePath $libraryDirectory)
    }
}

# zlib is mandatory: DMG/GHO decompression depends on it.
$zlib = Add-Component -Name "zlib" -LibraryName "z"
if ($null -eq $zlib -or $zlib -eq $false) {
    throw "Bundle has no usable zlib; DMG/GHO support cannot be built without it"
}
Add-CacheEntry -Variable "ZLIB_INCLUDE_DIR" -Value $zlib.Include -Type "PATH"
Add-CacheEntry -Variable "ZLIB_LIBRARY" -Value $zlib.Library
Add-CacheEntry -Variable "ZLIB_LIBRARY_RELEASE" -Value $zlib.Library
Write-Step "zlib      $($zlib.Library)"

$components = @(
    @{ Directory = "tsk"; Library = "tsk" },
    @{ Directory = "libbfio"; Library = "bfio" },
    @{ Directory = "libewf"; Library = "ewf" },
    @{ Directory = "wimlib"; Library = "wim" },
    @{ Directory = "libfsapfs"; Library = "fsapfs" },
    @{ Directory = "libfsrefs"; Library = "fsrefs" }
)

# 形态由 -Shared 决定，默认全静态（社区/内部构建，二进制不依赖 libyal 的 DLL）。
# 记录每个 libyal 组件实际选中的形态，末尾做一致性检查。
$chosenForms = @{}

foreach ($component in $components) {
    $resolved = Add-Component -Name $component.Directory -LibraryName $component.Library -Form $LinkForm
    if ($null -eq $resolved -or $resolved -eq $false) { continue }
    # 只统计真正会被链接进来的 libyal 组件；libbfio 自己没有静态形态时也会
    # 解析成 .dll.a，但静态配置下它根本不会被链进去，不该触发下面的告警。
    if ($component.Directory -in @("libewf", "libfsapfs", "libfsrefs")) {
        $chosenForms[$component.Directory] = if ($resolved.Library -like "*.dll.a") { "shared" } else { "static" }
    }

    switch ($component.Directory) {
        "tsk" {
            Add-CacheEntry -Variable "LIBTSK_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBTSK_LIBRARY" -Value $resolved.Library
        }
        "libewf" {
            Add-CacheEntry -Variable "LIBEWF_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBEWF_LIBRARY" -Value $resolved.Library
        }
        "libbfio" {
            # 只有共享形态的 libyal 才会用到它；静态库把 libbfio 打进了自己内部。
            # 静态配置下不给出导入库，免得平白多一个 libbfio-1.dll 的运行时依赖。
            Add-CacheEntry -Variable "UIFE_LIBBFIO_ROOT" -Value $resolved.Root -Type "PATH"
            if ($LinkForm -eq "shared") {
                Add-CacheEntry -Variable "UIFE_LIBBFIO_LIBRARY" -Value $resolved.Library
            }
        }
        "wimlib" {
            Add-CacheEntry -Variable "LIBWIM_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBWIM_LIBRARY" -Value $resolved.Library
            $runtime = Join-Path $resolved.LibraryDir "libwim-15.dll"
            if (Test-Path -LiteralPath $runtime) {
                Add-CacheEntry -Variable "UIFE_LIBWIM_RUNTIME" -Value (ConvertTo-CMakePath $runtime)
            }
        }
        "libfsapfs" {
            # 以前只钉 ROOT，形态交给 find_library 猜：包里两种都有时它挑 .dll.a，
            # 于是和钉死成静态的 libewf 混在一起，就有了上面说的两份 libbfio。
            Add-CacheEntry -Variable "LIBFSAPFS_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBFSAPFS_LIBRARY" -Value $resolved.Library
            Add-CacheEntry -Variable "UIFE_LIBFSAPFS_ROOT" -Value $resolved.Root -Type "PATH"
        }
        "libfsrefs" {
            Add-CacheEntry -Variable "LIBFSREFS_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBFSREFS_LIBRARY" -Value $resolved.Library
            Add-CacheEntry -Variable "UIFE_LIBFSREFS_ROOT" -Value $resolved.Root -Type "PATH"
        }
    }
    Write-Step ("{0,-9} {1}" -f $component.Directory, $resolved.Library)
}

$formsSeen = @($chosenForms.Values | Sort-Object -Unique)
if ($formsSeen.Count -gt 1) {
    # 包里缺某种形态时会走到这里。宁可吵，也别让人在一堆 DLL 里查一个空指针。
    $detail = ($chosenForms.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ", "
    Write-Warning ("libyal components resolved to mixed link forms ($detail). " +
        "Static and shared libyal each carry their own libbfio; using both in one " +
        "binary corrupts the libbfio handle and crashes on the first APFS/ReFS mount. " +
        "Rebuild the bundle so every libyal component has the requested form.")
}

$cacheDirectory = Split-Path -Parent $CMakeCacheFile
if (-not (Test-Path -LiteralPath $cacheDirectory)) {
    $null = New-Item -ItemType Directory -Path $cacheDirectory -Force
}

$header = @(
    "# Generated by scripts/fetch-deps.ps1 -- do not edit by hand.",
    "# Usage: cmake -S . -B build -G Ninja -C `"$CMakeCacheFile`"",
    ""
)
($header + $entries) | Set-Content -LiteralPath $CMakeCacheFile -Encoding ASCII

Write-Step "wrote $CMakeCacheFile"
Write-Host ""
Write-Host "Next step:" -ForegroundColor Green
$buildCommand = "  cmake -S . -B build -G Ninja -C `"$CMakeCacheFile`" " +
    "-DCMAKE_CXX_COMPILER=$CompilerPath -DCMAKE_BUILD_TYPE=Release"
Write-Host $buildCommand
