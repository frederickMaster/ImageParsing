param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$Generator = "",
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-uife")
)

$ErrorActionPreference = "Stop"
$buildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

$configureArgs = @(
    "-S", (Join-Path $PSScriptRoot ".."),
    "-B", $buildDirectory,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DUIFE_ENABLE_LIBTSK=ON",
    "-DUIFE_ENABLE_LIBEWF=ON"
)
$selectedGenerator = $Generator
if ($Generator) {
    $configureArgs += @("-G", $Generator)
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $configureArgs += @("-G", "MinGW Makefiles")
    $selectedGenerator = "MinGW Makefiles"
}

$tskRoot = $env:UIFE_LIBTSK_ROOT
if (!$tskRoot -and $selectedGenerator -match "MinGW") {
    $compiler = Get-Command g++ -ErrorAction SilentlyContinue
    $defaultTskRoot = Join-Path $env:LOCALAPPDATA "TheSleuthKit\4.15.0-ucrt64"
    if ($compiler -and $compiler.Source -match "UCRT" -and
        (Test-Path (Join-Path $defaultTskRoot "include\tsk\libtsk.h")) -and
        (Test-Path (Join-Path $defaultTskRoot "lib\libtsk.a"))) {
        $tskRoot = $defaultTskRoot
    }
}

if ($tskRoot) {
    $tskInclude = Join-Path $tskRoot "include"
    $tskLibrary = Join-Path $tskRoot "lib\libtsk.a"
    if (!(Test-Path (Join-Path $tskInclude "tsk\libtsk.h")) -or
        !(Test-Path $tskLibrary)) {
        throw "UIFE_LIBTSK_ROOT must contain include\tsk\libtsk.h and lib\libtsk.a"
    }
    $configureArgs += @(
        "-DLIBTSK_INCLUDE_DIR=$tskInclude",
        "-DLIBTSK_LIBRARY=$tskLibrary"
    )
}

$ewfRoot = $env:UIFE_LIBEWF_ROOT
if ($ewfRoot) {
    $ewfInclude = Join-Path $ewfRoot "include"
    if (!(Test-Path (Join-Path $ewfInclude "libewf.h"))) {
        throw "UIFE_LIBEWF_ROOT must contain include\libewf.h"
    }
    $ewfLibraryCandidates = @(
        (Join-Path $ewfRoot "lib\libewf.a"),
        (Join-Path $ewfRoot "lib\libewf.lib"),
        (Join-Path $ewfRoot "lib\ewf.lib")
    )
    $ewfLibrary = $ewfLibraryCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (!$ewfLibrary) {
        throw "UIFE_LIBEWF_ROOT must contain lib\libewf.a or a libewf import library"
    }
    $configureArgs += @(
        "-DLIBEWF_INCLUDE_DIR=$ewfInclude",
        "-DLIBEWF_LIBRARY=$ewfLibrary"
    )
    $cmakePrefixPaths = @($ewfRoot)
    if ($env:CMAKE_PREFIX_PATH) {
        $cmakePrefixPaths += $env:CMAKE_PREFIX_PATH -split ';' |
            Where-Object { $_ }
    }
    $configureArgs += "-DCMAKE_PREFIX_PATH=$($cmakePrefixPaths -join ';')"
}

$wimRoot = $env:UIFE_LIBWIM_ROOT
if ($wimRoot) {
    $wimIncludeCandidates = @(
        (Join-Path $wimRoot "devel"),
        (Join-Path $wimRoot "include")
    )
    $wimInclude = $wimIncludeCandidates |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ "wimlib.h") } |
        Select-Object -First 1
    if (!$wimInclude) {
        throw "UIFE_LIBWIM_ROOT must contain devel\wimlib.h or include\wimlib.h"
    }

    $wimLibraryCandidates = @(
        (Join-Path $wimInclude "libwim.lib"),
        (Join-Path $wimRoot "lib\libwim.a"),
        (Join-Path $wimRoot "lib\libwim.lib")
    )
    $wimLibrary = $wimLibraryCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (!$wimLibrary) {
        throw "UIFE_LIBWIM_ROOT must contain a matching wimlib library"
    }

    $configureArgs += @(
        "-DLIBWIM_INCLUDE_DIR=$wimInclude",
        "-DLIBWIM_LIBRARY=$wimLibrary"
    )
    $wimRuntime = Join-Path $wimRoot "libwim-15.dll"
    if (Test-Path -LiteralPath $wimRuntime) {
        $configureArgs += "-DUIFE_LIBWIM_RUNTIME=$wimRuntime"
    }
}

cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

cmake --build $buildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
exit $LASTEXITCODE
