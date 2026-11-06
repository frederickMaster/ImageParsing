# 可复现构建自检：同一份源码、两个独立构建目录，产出必须逐字节相同。
#
# 为什么需要它：闭源交付时，"我拿到的 exe 是否真的由这份源码构建" 只能靠
# 可复现构建回答。如果两次构建的哈希不同，这份二进制就没有可审计性——即使
# 功能完全正常。
#
# 用法：
#   pwsh -File scripts/repro-check.ps1
#   pwsh -File scripts/repro-check.ps1 -WorkRoot D:/tmp/repro -KeepArtifacts
#
# 退出码：0 一致，1 不一致，2 环境缺失。

[CmdletBinding()]
param(
    [string]$WorkRoot = "",
    [string]$CMakeExecutable = "",
    [string]$Generator = "Ninja",
    [string]$BuildType = "Release",
    [string]$Target = "uife",
    [string]$ExecutableName = "uife.exe",
    # 依赖路径（zlib/libtsk/libewf/libwim/libfsapfs/libfsrefs）随机器而异，
    # 由调用方透传；两次构建必须拿到**完全一样**的一套参数，否则差异来自配置
    # 而不是构建过程，检查就没有意义。
    [string[]]$CMakeArguments = @(),
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"

$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-CMake {
    if ($CMakeExecutable) {
        return (Resolve-Path $CMakeExecutable).Path
    }
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw "找不到 cmake；请用 -CMakeExecutable 指定路径"
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and $Generator -eq "Ninja") {
    throw "找不到 ninja；请安装 ninja 或用 -Generator 指定其它生成器"
}

$cmake = Resolve-CMake
if (-not $WorkRoot) {
    $WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("uife-repro-" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null

# 固定时间戳来源：任何遵守 SOURCE_DATE_EPOCH 的构建步骤都会拿到同一个值，
# 不遵守的步骤则在这里被暴露成哈希差异，而不是悄悄漂移。
$sourceDateEpoch = "1700000000"
$env:SOURCE_DATE_EPOCH = $sourceDateEpoch

$builds = @(
    @{ Name = "a"; Directory = (Join-Path $WorkRoot "build-a") },
    @{ Name = "b"; Directory = (Join-Path $WorkRoot "build-b") }
)

$hashes = @{}
try {
    foreach ($build in $builds) {
        Write-Output "== 构建 $($build.Name): $($build.Directory)"
        & $cmake -S $sourceDirectory -B $build.Directory -G $Generator `
            "-DCMAKE_BUILD_TYPE=$BuildType" @CMakeArguments | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "配置失败: $($build.Directory)"
        }
        & $cmake --build $build.Directory --target $Target | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "构建失败: $($build.Directory)"
        }
        $exe = Join-Path $build.Directory $ExecutableName
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "构建产物缺失: $exe"
        }
        $hashes[$build.Name] = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        Write-Output "   SHA-256 $($hashes[$build.Name])"
    }

    if ($hashes["a"] -ne $hashes["b"]) {
        Write-Output ""
        Write-Output "不可复现：两次构建的 SHA-256 不同。"
        Write-Output "常见原因：__DATE__/__TIME__、绝对路径（-ffile-prefix-map 未生效）、"
        Write-Output "依赖包版本不一致、链接器写入时间戳（MSVC 需要 /Brepro）。"
        exit 1
    }

    Write-Output ""
    Write-Output "可复现：两次构建逐字节一致（$($hashes["a"])）。"
    exit 0
} finally {
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Output "构建目录保留在 $WorkRoot"
    }
}
