# Shared ZIP writer.
#
# Why not Compress-Archive: on Windows PowerShell 5.1 it stores entry names with
# backslashes (licenses\CPL-1.0.txt). Windows Explorer and Expand-Archive cope,
# unzip/bsdtar do not - they unpack that into flat files whose names contain a
# backslash, with no useful error. The ZIP spec requires forward slashes, so the
# entry names are written here instead of being left to the cmdlet.
#
# Keep this file ASCII: PowerShell 5.1 decodes a BOM-less script as ANSI, and
# non-ASCII bytes in such a file were observed to break type resolution further
# down ("Cannot find type [System.IO.Compression.ZipArchiveMode]", reported on a
# line that has nothing to do with compression).

function Write-ZipArchive {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $source = (Resolve-Path -LiteralPath $SourceDirectory).Path.TrimEnd('\')
    if (Test-Path -LiteralPath $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }

    $zip = [System.IO.Compression.ZipFile]::Open(
        $DestinationPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($source.Length + 1) -replace '\\', '/'
            $null = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $_.FullName, $relative,
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
    }
    finally {
        $zip.Dispose()
    }
}
