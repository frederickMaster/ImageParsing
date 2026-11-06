# 生成 UIFE 自建测试样本（VHD 系列）
#
# 需要：管理员权限（diskpart 挂载虚拟磁盘）、Windows Server 2016+ 或支持 ReFS 的版本。
# 产物：<OutputDir>\ntfs.vhd、fat32.vhd、refs.vhd
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\make_samples.ps1 -OutputDir D:\tmp\samples
#
# 说明：
#   - NTFS 样本包含 ADS（secret / Zone.Identifier / 仅流文件）、稀疏文件、
#     NTFS 压缩文件、目录联接（junction）、文件符号链接、硬链接、EFS 尝试。
#   - FAT32 样本为最小可浏览树（FAT32 需要 >= 64MB 卷）。
#   - ReFS 需要 >= 1GB 卷，Server 2016 实测 64MB 会报“卷太小”。
#   - 脚本会为每个卷分配临时盘符并在结束时分离虚拟磁盘。

param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDir,
    [switch]$KeepMounted
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Invoke-DiskPart {
    param([string[]]$Lines, [string]$Tag)
    $script = Join-Path $env:TEMP "uife-diskpart-$Tag.txt"
    # diskpart 只接受 ASCII/ANSI 脚本，这里保持 ascii 以避免中文路径乱码。
    $Lines | Set-Content -Encoding ascii -Path $script
    & diskpart /s $script | Out-Host
    Remove-Item $script -Force -ErrorAction SilentlyContinue
}

function New-SampleVhd {
    param([string]$Path, [int]$SizeMb, [string]$FileSystem, [string]$Letter)
    if (Test-Path $Path) {
        Invoke-DiskPart -Tag 'detach' -Lines @(
            "select vdisk file=`"$Path`"",
            "detach vdisk"
        )
        Remove-Item $Path -Force -ErrorAction SilentlyContinue
    }
    Invoke-DiskPart -Tag "mk-$FileSystem" -Lines @(
        "create vdisk file=`"$Path`" maximum=$SizeMb type=fixed",
        "select vdisk file=`"$Path`"",
        "attach vdisk",
        "create partition primary",
        "format fs=$FileSystem quick label=UIFE$($FileSystem.ToUpper())",
        "assign letter=$Letter"
    )
}

function Close-SampleVhd {
    param([string]$Path)
    Invoke-DiskPart -Tag 'detach' -Lines @(
        "select vdisk file=`"$Path`"",
        "detach vdisk"
    )
}

function New-DeterministicFile {
    param([string]$Path, [int]$Bytes, [int]$Seed)
    $data = New-Object byte[] $Bytes
    (New-Object Random $Seed).NextBytes($data)
    [IO.File]::WriteAllBytes($Path, $data)
}

# ---- NTFS ----------------------------------------------------------------
$ntfs = Join-Path $OutputDir 'ntfs.vhd'
New-SampleVhd -Path $ntfs -SizeMb 64 -FileSystem 'ntfs' -Letter 'X'
try {
    'fixture-data' | Set-Content X:\hello.txt
    'secret-ads-content' | Set-Content X:\hello.txt:secret
    "`u{FEFF}[ZoneTransfer]`r`nZoneId=3" | Set-Content X:\hello.txt:Zone.Identifier
    'adsonly' | Set-Content X:\onlyads.txt:payload

    # 稀疏文件：4MB 长度，前 1MB 标记为未分配
    $handle = [IO.File]::Open('X:\sparse.dat', 'Create')
    $handle.SetLength(4MB)
    $handle.Close()
    & fsutil sparse setflag X:\sparse.dat | Out-Null
    & fsutil sparse setrange X:\sparse.dat 0 1048576 | Out-Null

    New-DeterministicFile -Path 'X:\random.bin' -Bytes (4MB) -Seed 42
    & compact /c /q X:\random.bin | Out-Null

    New-Item -ItemType Directory X:\docs | Out-Null
    'doc' | Set-Content X:\docs\a.txt
    New-Item -ItemType Junction -Path X:\docslink -Target X:\docs | Out-Null
    New-Item -ItemType HardLink -Path X:\hardlink.txt -Target X:\hello.txt | Out-Null
    New-Item -ItemType SymbolicLink -Path X:\symlink.txt -Target X:\hello.txt | Out-Null

    # EFS 加密（在部分环境/卷上不可用，失败不影响其余样本）
    try {
        'encrypted-payload' | Set-Content X:\secret.txt
        & cipher /e /q X:\secret.txt | Out-Null
    } catch {
        Write-Warning "EFS 加密失败（可忽略）：$($_.Exception.Message)"
    }
}
finally {
    if (-not $KeepMounted) { Close-SampleVhd -Path $ntfs }
}

# ---- FAT32 ---------------------------------------------------------------
$fat = Join-Path $OutputDir 'fat32.vhd'
New-SampleVhd -Path $fat -SizeMb 64 -FileSystem 'fat32' -Letter 'Y'
try {
    'fat-content' | Set-Content Y:\hello.txt
    New-Item -ItemType Directory Y:\sub | Out-Null
    'nested' | Set-Content Y:\sub\note.txt
    New-DeterministicFile -Path 'Y:\data.bin' -Bytes (1MB) -Seed 7
}
finally {
    if (-not $KeepMounted) { Close-SampleVhd -Path $fat }
}

# ---- ReFS（可选）---------------------------------------------------------
$refs = Join-Path $OutputDir 'refs.vhd'
try {
    New-SampleVhd -Path $refs -SizeMb 1024 -FileSystem 'refs' -Letter 'Z'
    try {
        'refs-content' | Set-Content Z:\hello.txt
        New-Item -ItemType Directory Z:\sub | Out-Null
        'nested-refs' | Set-Content Z:\sub\note.txt
        New-DeterministicFile -Path 'Z:\data.bin' -Bytes (2MB) -Seed 9
    }
    finally {
        if (-not $KeepMounted) { Close-SampleVhd -Path $refs }
    }
}
catch {
    Write-Warning "ReFS 样本未生成（当前系统可能不支持 ReFS 或卷过小）：$($_.Exception.Message)"
}

Get-ChildItem $OutputDir -Filter *.vhd | Select-Object Name, Length
