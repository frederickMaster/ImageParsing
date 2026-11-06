param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-uife"),
    [string]$OutputArchive = (Join-Path $PSScriptRoot "..\dist\uife-windows-x64.zip"),
    [string]$Generator = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$buildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputArchive = [System.IO.Path]::GetFullPath($OutputArchive)

if (Test-Path -LiteralPath $outputArchive) {
    if (!$Force) {
        throw "Output archive already exists; choose another path or pass -Force: $outputArchive"
    }
    if (!(Test-Path -LiteralPath $outputArchive -PathType Leaf)) {
        throw "Output archive path exists and is not a file: $outputArchive"
    }
    Remove-Item -LiteralPath $outputArchive -Force
}

$buildScript = Join-Path $PSScriptRoot "build.ps1"
$buildArguments = @{
    Configuration = "Release"
    BuildDirectory = $buildDirectory
}
if ($Generator) {
    $buildArguments.Generator = $Generator
}
& $buildScript @buildArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$executable = Join-Path $buildDirectory "uife.exe"
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable was not produced: $executable"
}

$archiveDirectory = Split-Path -Parent $outputArchive
if (!(Test-Path -LiteralPath $archiveDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $archiveDirectory -Force | Out-Null
}

$stagingDirectory = Join-Path $archiveDirectory (
    "uife-package-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
try {
    Copy-Item -LiteralPath $executable -Destination $stagingDirectory
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "README.md") `
        -Destination $stagingDirectory

    # 静态链接的第三方库（libtsk / libewf / libfsapfs / libfsrefs / zlib）没有可
    # 发现的 COPYING 文件，因此随包一份组件清单；缺失就失败，不允许"漏声明"。
    $notices = Join-Path $repositoryRoot "docs\THIRD-PARTY-NOTICES.txt"
    if (!(Test-Path -LiteralPath $notices -PathType Leaf)) {
        throw "Third-party notices are missing: $notices"
    }
    Copy-Item -LiteralPath $notices -Destination $stagingDirectory

    $runtimeDlls = Get-ChildItem -LiteralPath $buildDirectory -Filter "*.dll" `
        -File -ErrorAction SilentlyContinue
    foreach ($runtimeDll in $runtimeDlls) {
        Copy-Item -LiteralPath $runtimeDll.FullName `
            -Destination $stagingDirectory
    }

    if ($runtimeDlls | Where-Object { $_.Name -match '^libwim(-\d+)?\.dll$' }) {
        $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
        $wimInclude = $env:UIFE_LIBWIM_ROOT
        if (Test-Path -LiteralPath $cachePath) {
            $cacheEntry = Select-String -Path $cachePath `
                -Pattern "^LIBWIM_INCLUDE_DIR:PATH=(.+)$" |
                Select-Object -First 1
            if ($cacheEntry) {
                $wimInclude = $cacheEntry.Matches[0].Groups[1].Value
            }
        }
        if ($wimInclude) {
            if (Test-Path -LiteralPath (Join-Path $wimInclude "COPYING.txt")) {
                $licenseDirectory = $wimInclude
            } elseif (Test-Path -LiteralPath (Join-Path $wimInclude "..\COPYING.txt")) {
                $licenseDirectory = [System.IO.Path]::GetFullPath(
                    (Join-Path $wimInclude ".."))
            } elseif (Test-Path -LiteralPath (Join-Path $wimInclude "..\..\COPYING.txt")) {
                $licenseDirectory = [System.IO.Path]::GetFullPath(
                    (Join-Path $wimInclude "..\.."))
            } else {
                throw "Could not locate wimlib license notices from: $wimInclude"
            }
        } else {
            throw "A libwim runtime DLL is present, but its license directory is unknown"
        }

        foreach ($license in @(
                "COPYING.txt",
                "COPYING.GPLv3.txt",
                "COPYING.LGPL.txt",
                "COPYING.libdivsufsort-lite.txt")) {
            $licensePath = Join-Path $licenseDirectory $license
            if (!(Test-Path -LiteralPath $licensePath -PathType Leaf)) {
                throw "Required wimlib license notice is missing: $licensePath"
            }
            Copy-Item -LiteralPath $licensePath -Destination $stagingDirectory
        }
    }

    Compress-Archive -Path (Join-Path $stagingDirectory "*") `
        -DestinationPath $outputArchive -CompressionLevel Optimal
} finally {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}

Write-Output "Created UIFE package: $outputArchive"
