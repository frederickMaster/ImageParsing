<#
.SYNOPSIS
    Packs the locally built native SDKs into one relocatable archive.

.DESCRIPTION
    UIFE's optional backends (libtsk, libewf, wimlib, libfsapfs, libfsrefs) and
    zlib are not vendored in this repository. Building them by hand takes hours
    (see docs/DEPENDENCIES.md), so instead the already built SDK trees are packed
    into a single archive that another machine can unpack in seconds.

    The archive layout is:

        manifest.json
        zlib/{include,lib}
        tsk/{include,lib}
        libbfio/{include,lib,bin}
        libewf/{include,lib,bin}
        wimlib/{include,lib,bin}
        libfsapfs/{include,lib,bin}
        libfsrefs/{include,lib,bin}

    The libyal components carry both forms: the static archive used by the
    community build and the shared library (import library plus DLL) used by the
    closed-source build, which links them as DLLs so LGPL-3.0 section 4(d)(1)
    applies instead of the relinking requirement.

    Components that are missing on this machine are skipped with a warning, so a
    partial bundle can still be produced.

    IMPORTANT: the bundle is ABI specific. An archive produced with MinGW-w64
    UCRT cannot be linked by MSVC and vice versa. manifest.json records the
    compiler that produced it so fetch-deps.ps1 can warn on mismatch.

.PARAMETER ToolsRoot
    Root directory that holds the SDK folders. Defaults to D:\tools.

.PARAMETER OutputArchive
    Destination zip. Defaults to <repo>\deps\uife-deps-mingw-ucrt64.zip.

.PARAMETER CompilerPath
    Compiler used to record the toolchain in manifest.json.

.PARAMETER Force
    Overwrite an existing archive.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package-deps.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package-deps.ps1 `
        -ToolsRoot D:\tools -OutputArchive D:\transfer\uife-deps.zip
#>
[CmdletBinding()]
param(
    [string]$ToolsRoot = "D:\tools",
    [string]$OutputArchive = "",
    [string]$CompilerPath = "D:\tools\mingw64\bin\g++.exe",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# See fetch-deps.ps1: $PSScriptRoot is empty while parameter defaults are
# evaluated under Windows PowerShell 5.1, so resolve the default in the body.
if ([string]::IsNullOrEmpty($PSScriptRoot)) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    $scriptRoot = $PSScriptRoot
}
if ([string]::IsNullOrEmpty($OutputArchive)) {
    $OutputArchive = [System.IO.Path]::GetFullPath(
        (Join-Path $scriptRoot "..\deps\uife-deps-mingw-ucrt64.zip"))
}

function Write-Step {
    param([string]$Message)
    Write-Host "[deps] $Message" -ForegroundColor Cyan
}

function Resolve-Component {
    param([string]$Root)

    $components = New-Object System.Collections.ArrayList

    $null = $components.Add([pscustomobject]@{
        Name = "zlib"
        Path = (Join-Path $Root "zlib")
    })

    # Sleuth Kit is installed under a versioned directory such as tsk-4.15.0-ucrt64.
    $tsk = Get-ChildItem -LiteralPath $Root -Directory -Filter "tsk-*-ucrt64" `
        -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -ne $tsk) {
        $null = $components.Add([pscustomobject]@{ Name = "tsk"; Path = $tsk.FullName })
    }

    foreach ($entry in @(
            @{ Name = "libbfio"; Directory = "libbfio-sdk" },
            @{ Name = "libewf"; Directory = "libewf-sdk" },
            @{ Name = "wimlib"; Directory = "wimlib-sdk" },
            @{ Name = "libfsapfs"; Directory = "libfsapfs-sdk" },
            @{ Name = "libfsrefs"; Directory = "libfsrefs-sdk" })) {
        $null = $components.Add([pscustomobject]@{
            Name = $entry.Name
            Path = (Join-Path $Root $entry.Directory)
        })
    }

    return $components
}

function Get-CompilerDescription {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return "unknown (compiler not found: $Path)"
    }
    $firstLine = (& $Path --version 2>$null | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($firstLine)) {
        return "unknown"
    }
    return $firstLine.Trim()
}

$outputDirectory = Split-Path -Parent $OutputArchive
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    $null = New-Item -ItemType Directory -Path $outputDirectory -Force
}

if ((Test-Path -LiteralPath $OutputArchive) -and -not $Force) {
    throw "Archive already exists: $OutputArchive (use -Force to overwrite)"
}

$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("uife-deps-" + [guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Path $staging -Force

try {
    Write-Step "staging in $staging"

    $packed = New-Object System.Collections.ArrayList
    foreach ($component in (Resolve-Component -Root $ToolsRoot)) {
        if (-not (Test-Path -LiteralPath $component.Path)) {
            Write-Warning "skip $($component.Name): $($component.Path) not found"
            continue
        }

        $target = Join-Path $staging $component.Name
        $null = New-Item -ItemType Directory -Path $target -Force

        foreach ($sub in @("include", "lib", "bin")) {
            $source = Join-Path $component.Path $sub
            if (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination $target -Recurse -Force
            }
        }

        # Keep license files next to the binaries so redistributing the bundle stays legal.
        Get-ChildItem -LiteralPath $component.Path -File -Filter "COPYING*" `
            -ErrorAction SilentlyContinue | ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            }

        if (-not (Test-Path -LiteralPath (Join-Path $target "include"))) {
            Write-Warning "skip $($component.Name): no include directory"
            Remove-Item -LiteralPath $target -Recurse -Force
            continue
        }

        $sizeBytes = (Get-ChildItem -LiteralPath $target -Recurse -File |
            Measure-Object -Property Length -Sum).Sum
        $sizeMb = [math]::Round($sizeBytes / 1MB, 1)
        Write-Step ("packed {0,-10} {1,7} MB  <- {2}" -f $component.Name, $sizeMb, $component.Path)
        $null = $packed.Add($component.Name)
    }

    if ($packed.Count -eq 0) {
        throw "Nothing to pack: no SDK found under $ToolsRoot"
    }

    $manifest = [ordered]@{
        format     = "uife-deps/1"
        generated  = (Get-Date).ToString("s")
        triplet    = "mingw-ucrt64"
        compiler   = (Get-CompilerDescription -Path $CompilerPath)
        components = @($packed)
        sourceRoot = $ToolsRoot
    }
    $manifestPath = Join-Path $staging "manifest.json"
    ($manifest | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    if (Test-Path -LiteralPath $OutputArchive) {
        Remove-Item -LiteralPath $OutputArchive -Force
    }
    # Do not use Compress-Archive: on Windows PowerShell 5.1 it writes entry names
    # with backslashes (libbfio\bin\libbfio-1.dll), while the ZIP spec requires
    # forward slashes. Expand-Archive tolerates it, but unzip/bsdtar on a POSIX
    # host unpacks that into flat files whose names contain backslashes - a
    # failure with no useful symptom. Write the entry names ourselves instead.
    #
    # NOTE: keep this file ASCII only. PowerShell 5.1 reads a BOM-less script as
    # ANSI, and non-ASCII bytes in it were observed to break type resolution
    # further down ("Cannot find type [System.IO.Compression.ZipArchiveMode]").
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::Open(
        $OutputArchive, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $stagingLength = $staging.TrimEnd('\').Length + 1
        Get-ChildItem -LiteralPath $staging -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($stagingLength) -replace '\\', '/'
            $null = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $_.FullName, $relative,
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
    }
    finally {
        $zip.Dispose()
    }

    $archiveMb = [math]::Round((Get-Item -LiteralPath $OutputArchive).Length / 1MB, 1)
    Write-Step "wrote $OutputArchive ($archiveMb MB)"
}
finally {
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
