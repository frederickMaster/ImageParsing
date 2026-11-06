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

    # 第三方库的许可证全文由本仓库的组件清单指向，随包一份；缺失就失败，不允许"漏声明"。
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

    # libyal 的共享后端彼此有依赖：libewf / libfsapfs / libfsrefs 都导入 libbfio，
    # 也都导入 MinGW 的线程运行时。少一个就是打出一个装得上、跑不起来的包。
    $dllNames = @($runtimeDlls | ForEach-Object { $_.Name })
    $libyalDlls = @($dllNames | Where-Object { $_ -match '^lib(ewf|fsapfs|fsrefs)-\d+\.dll$' })
    if ($libyalDlls.Count -gt 0) {
        $required = @(
            ($dllNames | Where-Object { $_ -match '^libbfio-\d+\.dll$' }) +
            ($dllNames | Where-Object { $_ -match '^libwinpthread-\d+\.dll$' }))
        if ($required.Count -lt 2) {
            throw ("Shared libyal backends ({0}) need libbfio and libwinpthread " +
                   "next to the executable; found only: {1}") -f `
                ($libyalDlls -join ", "), ($dllNames -join ", ")
        }
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

    # LGPL-3.0 §4(b) / CPL-1.0 §3 要求把许可证全文随二进制一起分发。全文不入库到
    # 根目录，统一放在 docs/licenses/，缺一份就失败。
    $licenseSource = Join-Path $repositoryRoot "docs\licenses"
    $licenseTarget = Join-Path $stagingDirectory "licenses"
    foreach ($license in @(
            "LGPL-3.0.txt",
            "GPL-3.0.txt",
            "CPL-1.0.txt",
            "Zlib.txt")) {
        $licensePath = Join-Path $licenseSource $license
        if (!(Test-Path -LiteralPath $licensePath -PathType Leaf)) {
            throw "License text is missing from the repository: $licensePath"
        }
        if (!(Test-Path -LiteralPath $licenseTarget -PathType Container)) {
            New-Item -ItemType Directory -Path $licenseTarget | Out-Null
        }
        Copy-Item -LiteralPath $licensePath -Destination $licenseTarget
    }

    Compress-Archive -Path (Join-Path $stagingDirectory "*") `
        -DestinationPath $outputArchive -CompressionLevel Optimal
} finally {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}

Write-Output "Created UIFE package: $outputArchive"
