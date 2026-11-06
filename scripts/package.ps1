param(
    [string]$BuildDirectory = "",
    [string]$OutputArchive = "",
    [string]$Generator = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 leaves $PSScriptRoot empty while parameter defaults are
# evaluated, so resolve them here (see fetch-deps.ps1 for the full story).
if ([string]::IsNullOrEmpty($PSScriptRoot)) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    $scriptRoot = $PSScriptRoot
}
if ([string]::IsNullOrEmpty($BuildDirectory)) {
    $BuildDirectory = Join-Path $scriptRoot "..\build-uife"
}
if ([string]::IsNullOrEmpty($OutputArchive)) {
    $OutputArchive = Join-Path $scriptRoot "..\dist\uife-windows-x64.zip"
}
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
$licenseTarget = Join-Path $stagingDirectory "licenses"
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
        # 这里必须逐项判断，不能把两个 Where-Object 的结果用 "+" 拼起来：
        # 单元素管道会退化成字符串，`"libbfio-1.dll" + "libwinpthread-1.dll"`
        # 做的是字符串连接，@() 包起来只有 1 个元素，于是"两个都在"也判成缺失，
        # 打包直接失败（实测报"found only: libbfio-1.dll, ..., libwinpthread-1.dll"）。
        $missing = @()
        foreach ($entry in @(
                @{ Pattern = '^libbfio-\d+\.dll$'; Name = 'libbfio-<n>.dll' },
                @{ Pattern = '^libwinpthread-\d+\.dll$'; Name = 'libwinpthread-<n>.dll' })) {
            if (-not ($dllNames -match $entry.Pattern)) {
                $missing += $entry.Name
            }
        }
        if ($missing.Count -gt 0) {
            throw ("Shared libyal backends ({0}) need {1} next to the executable; " +
                   "found only: {2}") -f `
                ($libyalDlls -join ", "), ($missing -join ", "), ($dllNames -join ", ")
        }
    }

    # wimlib 的许可全文从仓库里取，不在 SDK 目录里找。
    #
    # 以前这里会在 libwim 的 include 目录附近找 COPYING*.txt，其中包含一个
    # COPYING.libdivsufsort-lite.txt —— 上游 wimlib 1.14.5 根本没有这个文件
    # （libdivsufsort-lite 的 MIT 声明只写在 src/divsufsort.c 的文件头里），
    # 于是只要包里带 libwim-15.dll，打包必然抛"Required wimlib license notice is
    # missing"，发布流程直接断在那里。现在这些文本入库在 docs/licenses/，由
    # docs/LICENSES.md 说明出处（含来源包的 SHA256），打包只做复制与存在性检查。
    if ($runtimeDlls | Where-Object { $_.Name -match '^libwim(-\d+)?\.dll$' }) {
        $wimlibLicenseSource = Join-Path $repositoryRoot "docs\licenses"
        foreach ($license in @(
                "wimlib-COPYING.txt",
                "wimlib-COPYING.GPLv3.txt",
                "wimlib-COPYING.LGPL.txt",
                "MIT-libdivsufsort-lite.txt")) {
            $licensePath = Join-Path $wimlibLicenseSource $license
            if (!(Test-Path -LiteralPath $licensePath -PathType Leaf)) {
                throw "Required wimlib license notice is missing: $licensePath"
            }
            if (!(Test-Path -LiteralPath $licenseTarget -PathType Container)) {
                New-Item -ItemType Directory -Path $licenseTarget | Out-Null
            }
            Copy-Item -LiteralPath $licensePath -Destination $licenseTarget
        }
    }

    # LGPL-3.0 §4(b)（libyal）/ LGPL-2.1 §6(a)（wimlib）/ CPL-1.0 §3 要求把许可证
    # 全文随二进制一起分发。全文不散放在仓库根目录，统一放在 docs/licenses/，
    # 缺一份就失败。
    $licenseSource = Join-Path $repositoryRoot "docs\licenses"
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

    . (Join-Path $scriptRoot "zip-utils.ps1")
    Write-ZipArchive -SourceDirectory $stagingDirectory -DestinationPath $outputArchive
} finally {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}

Write-Output "Created UIFE package: $outputArchive"
