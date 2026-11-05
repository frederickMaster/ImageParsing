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
    [string]$ArchivePath = (Join-Path $PSScriptRoot "..\deps\uife-deps-mingw-ucrt64.zip"),
    [string]$Destination = (Join-Path $PSScriptRoot "..\deps\sdk"),
    [string]$CMakeCacheFile = (Join-Path $PSScriptRoot "..\deps\uife-deps.cmake"),
    [string]$CompilerPath = "D:\tools\mingw64\bin\g++.exe",
    [switch]$Force
)

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
    param([string]$Name, [string]$LibraryName)

    $root = Join-Path $Destination $Name
    $includeDirectory = Join-Path $root "include"
    $libraryDirectory = Join-Path $root "lib"

    if (-not (Test-Path -LiteralPath $includeDirectory)) {
        Write-Warning "skip ${Name}: no include directory in bundle"
        return $false
    }

    $library = $null
    if (Test-Path -LiteralPath $libraryDirectory) {
        $library = Find-StaticLibrary -Directory $libraryDirectory -Name $LibraryName
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
    @{ Directory = "libewf"; Library = "ewf" },
    @{ Directory = "wimlib"; Library = "wim" },
    @{ Directory = "libfsapfs"; Library = "fsapfs" },
    @{ Directory = "libfsrefs"; Library = "fsrefs" }
)

foreach ($component in $components) {
    $resolved = Add-Component -Name $component.Directory -LibraryName $component.Library
    if ($null -eq $resolved -or $resolved -eq $false) { continue }

    switch ($component.Directory) {
        "tsk" {
            Add-CacheEntry -Variable "LIBTSK_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBTSK_LIBRARY" -Value $resolved.Library
        }
        "libewf" {
            Add-CacheEntry -Variable "LIBEWF_INCLUDE_DIR" -Value $resolved.Include -Type "PATH"
            Add-CacheEntry -Variable "LIBEWF_LIBRARY" -Value $resolved.Library
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
            Add-CacheEntry -Variable "UIFE_LIBFSAPFS_ROOT" -Value $resolved.Root -Type "PATH"
        }
        "libfsrefs" {
            Add-CacheEntry -Variable "UIFE_LIBFSREFS_ROOT" -Value $resolved.Root -Type "PATH"
        }
    }
    Write-Step ("{0,-9} {1}" -f $component.Directory, $resolved.Library)
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
