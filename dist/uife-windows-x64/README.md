# UIFE — Universal Image Forensics Engine

UIFE is a read-only, C++20 command-line image inspection and extraction MVP. It is designed around separate image-driver, filesystem-parser, VFS, and extraction layers so additional forensic formats can be integrated without coupling the CLI to individual parsers. Building requires CMake and zlib; libtsk, libewf, and wimlib remain optional.

## Project layout

```text
include/uife/
  drivers/
    fs/
    image/
  vfs/
src/
  cli/main.cpp
  drivers/
    fs/
    image/
  vfs/
tests/
  uife_tests.cpp
  uife_integration_tests.cpp
```

`IImageDriver` owns read-only sector and byte-range access. `DriverFactory` uses registered magic probes and a RAW fallback. `IFSParser` provides mount, directory listing, metadata, and file reads. The VFS normalizes POSIX-style paths; the recursive extractor checks output paths, restores supported metadata, reports byte progress, and verifies each regular file with SHA-256 after writing.

## Implemented and not implemented

| Capability | Current status |
|---|---|
| RAW / `.img` and ISO9660 image drivers | Implemented; read-only random access |
| VMDK image driver | Implemented for single-file, uncompressed sparse VMDK v1 and descriptor-based `monolithicFlat`/`twoGbMaxExtentFlat` images with one or more FLAT extents (`RW` or `RDONLY`); random reads can cross extent boundaries; parent-linked disks and descriptor extent paths outside the descriptor directory are rejected |
| VDI image driver | Implemented for VDI 1.1 dynamic and fixed images with validated block maps; differencing images and block-extra data are rejected |
| QED image driver | Implemented for standalone QED images without backing files or feature flags; allocated and zero clusters are supported |
| QCOW2 image driver | Implemented for standalone v2/v3 images without encryption, backing files, snapshots, or feature flags; uncompressed mappings and sparse/zero clusters are supported |
| VHDX image driver | Implemented for clean-log, standalone dynamic/fixed images with full payload blocks; validates redundant headers, region tables, metadata, and BAT |
| BIN+CUE image driver | Implemented for one or more BINARY files with MODE1/2048 or MODE1/2352 tracks; concatenates INDEX 01 data extents into 2048-byte sectors and strips MODE1/2352 framing; CUE paths are confined to the descriptor directory |
| MBR partition table | Implemented for primary and extended/logical partitions; EBR chain bounds and cycles are checked |
| GPT partition table | Implemented with header and partition-array CRC checks |
| ISO9660 filesystem parser | Implemented with Joliet UTF-16 names, multi-extent files, directory-record modification timestamps (including UTC offset), dual-endian field consistency checks, and extents bounded by the declared volume size; exposed through the shared VFS |
| Btrfs send stream parser | Built-in read-only reconstruction of standalone v1-v3 streams with CRC32C validation, directories, regular-file writes/truncation, links, symlinks, supported inode metadata and xattrs; incremental snapshots, extent/file-attribute updates, clone/fallocate/encoded-write/verity commands are rejected |
| NTFS, FAT/exFAT, HFS+, and ext2/3/4 filesystem parsers | Optional libtsk backend reads through any supported read-only image driver, including virtual-disk and BIN+CUE drivers; the Windows `fls.exe`/`icat.exe` fallback is limited to RAW files |
| APFS container parser | Optional libtsk APFS pool backend; exposes container volumes under `/<volume-name>` and attempts encrypted-volume opening when a password is supplied through `--password-stdin`. Requires a libtsk build with APFS pool decryption support. Verified with header recognition and corrupt-checksum rejection, but password decryption has not yet been validated against a real encrypted APFS image |
| XFS filesystem parser | XFS superblock detection on supported image drivers; read-only browsing through libtsk only when the installed development headers expose `TSK_FS_TYPE_XFS` |
| Filesystem metadata | libtsk reports mode, UID/GID, link count, available times, and symlink targets; filesystem geometry and ext-family file-size bounds are validated before browsing or extraction |
| Extracted file verification | SHA-256 of source stream compared to SHA-256 read back from the exported file; ISO9660 integration test also verifies the source image hash is unchanged after browsing and extraction |
| Image signature recognition | QCOW2, VMDK, VDI, QED, VHDX, EWF/E01, WIM/ESD, ISO9660, DMG, GHO, BIN+CUE |
| EWF/E01 image driver | Optional libewf-backed, read-only segmented EWF access with random reads; verified with a generated two-segment E01 containing a FAT12 test file, including extraction hash equality and unchanged source hashes. A separate synthetic two-segment EWF under a Chinese Windows path was also opened successfully. Representative real-world EWF samples have not been tested |
| WIM/ESD filesystem parser | Optional wimlib-backed, read-only browsing of images exposed under `/images/N`; file contents are staged on demand in a temporary directory. Reparse-point semantics, ACLs, alternate data streams, split WIMs, and encrypted ESD are not supported |
| Apple DMG image driver | Read-only, single-segment UDIF/koly parsing for XML or binary plist blkx maps with raw, zero/ignore, and zlib runs; segmented images and other compression/encryption run types are rejected |
| Norton Ghost GHO image driver | Read-only random-access view of one to four unencrypted partitions with no compression, Fast LZ, or zlib compression levels 3-9. Same-directory `.ghs` span files are discovered from the matching `.gho` stem, checked for compatible span type, compression, and image ID, and read as one virtual stream without staging or modifying the source files. A valid Track 0 MBR is composed into a disk view only when the primary partition layout exactly matches the decoded partitions; a single partition without a matching MBR is exposed directly, while unsafe multi-partition layouts are rejected. Legacy compression type 1 and encrypted images are identified but rejected |
| Other filesystems and encryption | ReFS signature detection is available, but ReFS browsing is not implemented. XFS is not advertised as browsable unless compatible libtsk support was detected at build time |

Only allocated filesystem objects and regular-file data returned by the active Sleuth Kit backend are currently restored. POSIX ownership is restored when the parser explicitly reports UID/GID, and extended attributes are restored on Linux and macOS when supplied by a parser; failures are reported rather than silently ignored. The Windows command-line fallback gets file names, sizes, UID/GID, and basic node types from `fls`; it does not recover full permission bits, timestamps, extended attributes, or symlink targets. NTFS alternate data streams, sparse allocation maps, complete Windows ACL/security descriptors, and NTFS reparse-point semantics are not fully restored by this MVP. Unsupported metadata or filesystem behavior must not be mistaken for a complete forensic restoration. Symlink targets are exported only when the libtsk library backend returns them.

The libtsk backend bounds every read to the selected image/partition extent and rejects filesystems whose declared block geometry exceeds that extent. For ext-family filesystems, file sizes beyond the format's 32-bit logical-block address limit are marked invalid, shown as `[!]` in `list`, and refused by extraction rather than used for unbounded reads. APFS container volume names are exposed below a virtual root, with unsafe path-component bytes percent-encoded. APFS file sizes larger than the container image are treated as invalid for conservative extraction. Supply an encrypted APFS password through standard input, not as a command-line argument, using `--password-stdin`; for example, `uife list apfs.raw /Volume --password-stdin < password.txt`. Protect any password file according to your evidence-handling policy. A wrong password or unsupported encrypted volume is reported as an error; UIFE never writes to the source image.

The ISO9660 parser provides directory listing and file reads, including Joliet Unicode names and consecutive multi-extent files. Interleaved files and multi-extent directories are rejected explicitly. It reads through the image driver's random-access interface, so it can browse ISO filesystems exposed by QCOW2, VHDX, VMDK, VDI, QED, DMG, and supported BIN+CUE drivers without staging or modifying backing files. DMG support is limited to single-segment UDIF images with XML or binary property lists and raw, zero/ignore, or zlib blkx runs; segmented, ADC, bzip2, LZFSE, encrypted, and other variants are rejected. BIN+CUE support covers same-directory BINARY files containing sequential MODE1 data tracks (2048-byte sectors or 2352-byte sectors with the 16-byte sync/header removed); tracks are concatenated from each INDEX 01. Audio tracks, MODE2, and mixed sector formats within one BINARY file are rejected. QCOW2 support covers standalone v2/v3 images with standard cluster geometry, no backing files, encryption, snapshots, or feature flags; compressed clusters and unsupported mappings are rejected. VHDX support covers standalone dynamic/fixed images with a zero log GUID, no parent, and fully present, absent, or zero payload blocks; logged images, differencing disks, and partially present blocks are rejected rather than replayed or approximated. VMDK support includes single-file, uncompressed sparse v1 and descriptor-based flat extents; descriptor extents may span multiple files and reads can cross their boundaries. Snapshots and compressed sparse grains are not supported. VDI support includes version 1.1 dynamic/fixed images with standard block size and no block-extra bytes; differencing images are rejected. QED support is limited to standalone images with no backing file and no compatible, autoclear, or incompatible feature flags; other variants are rejected. XFS superblocks are identified from validated geometry; browsing is registered only when the detected libtsk development version exposes XFS support. When libtsk is available, its library backend reads through the image driver's bounded, read-only callback; the Windows Sleuth Kit command-line fallback is limited to RAW source files. HFS+ detection validates the volume header signature, version, allocation-block geometry, and image bounds before exposing the TSK parser. APFS support depends on libtsk's APFS pool and pool-decryption APIs and currently covers single-physical-store containers supported by that backend; encrypted-volume support has not been validated against a real sample, and Fusion containers are not included.

## Build

Requires CMake 3.20+ and a C++20 compiler. libtsk, libewf, and wimlib are optional at configure time. Without them, UIFE still builds the RAW, ISO9660, QCOW2, QED, VHDX, VDI, and documented VMDK drivers; EWF and WIM/ESD remain detection-only. On Windows, installed Sleuth Kit command-line tools are used for RAW NTFS/ext2/3/4 browsing and extraction; on other platforms the development library is required.

For libtsk-enabled builds, install The Sleuth Kit development package and `pkg-config`, or make `tsk/libtsk.h` and the matching `tsk` library discoverable through CMake. Example on Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libtsk-dev
```

EWF/E01 reading is enabled automatically when the libewf development package is found. On Debian/Ubuntu, install it with:

```sh
sudo apt install libewf-dev
```

Set `-DUIFE_ENABLE_LIBEWF=OFF` to disable libewf discovery. When the library is absent or does not provide the required API, EWF remains recognition-only.

WIM and ESD filesystem browsing is enabled when a compatible wimlib development package is found. On Debian/Ubuntu:

```sh
sudo apt install libwim-dev
```

Set `-DUIFE_ENABLE_LIBWIM=OFF` to disable wimlib discovery. For a Windows SDK, set `UIFE_LIBWIM_ROOT` to its root directory; CMake looks for `devel\wimlib.h`, `devel\libwim.lib`, and the wimlib runtime DLL. The build script accepts the same setting:

```powershell
$env:UIFE_LIBWIM_ROOT = "C:\path\to\wimlib-sdk"
.\scripts\build.ps1
```

List the virtual image index directory first, then browse a selected image using `/images/N`, for example `uife list install.wim /images` and `uife list install.wim /images/1`. WIM/ESD files are opened read-only; file data is staged in a private temporary directory on demand and removed with the parser. This does not modify the source image. Encrypted ESD, split WIM sets, reparse-point behavior, complete Windows ACL/security descriptors, and alternate data streams are not currently supported.

On Windows, point `UIFE_LIBEWF_ROOT` at a compiler-compatible libewf SDK containing `include\libewf.h` and a matching library; `scripts\build.ps1` passes these paths to CMake and adds the SDK root to dependency discovery. Static libewf builds also need compatible zlib and bzip2 development libraries discoverable through CMake (or included under the SDK root). The libewf build and ABI must match the selected compiler. UIFE uses libewf's wide-character path APIs on Windows; the SDK must also be built with Unicode system-string support for non-ASCII segment paths. This was verified with a local Unicode-enabled libewf build and a synthetic Chinese-named split EWF fixture.

Build and run tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The CLI argument-validation test verifies that malformed options are rejected before the input image is opened.

Windows PowerShell (MSYS2/MinGW with the matching libtsk development headers and library, if NTFS/ext support is required):

```powershell
.\scripts\build.ps1
```

The build script automatically selects the per-user 4.15.0 SDK when it detects the matching UCRT MinGW compiler. For another installed toolchain/ABI, set `UIFE_LIBTSK_ROOT` to the root of a compatible SDK before running the script.

If libtsk is installed outside the compiler's default search paths, pass its include directory and library explicitly:

```powershell
$tskRoot = "$env:LOCALAPPDATA\TheSleuthKit\4.15.0-ucrt64"
cmake -S . -B build-uife -DCMAKE_BUILD_TYPE=Release `
  "-DLIBTSK_INCLUDE_DIR=$tskRoot\include" `
  "-DLIBTSK_LIBRARY=$tskRoot\lib\libtsk.a"
cmake --build build-uife --config Release
ctest --test-dir build-uife -C Release --output-on-failure
```

The `4.15.0-ucrt64` location is a per-user SDK built from the official Sleuth Kit 4.15.0 sources for 64-bit UCRT MinGW; its static build omits optional zlib and libewf integrations. Other compilers require a libtsk library built for their own architecture and ABI. This Sleuth Kit version does not expose an XFS filesystem type, so XFS remains detection-only. The Chocolatey `sleuthkit` package is a command-line application package; it does not necessarily contain C++ development headers, and its bundled 32-bit MSVC library cannot be linked into a 64-bit MinGW build. Windows UIFE searches `PATH`, the `UIFE_TSK_BIN` directory, and the standard Chocolatey Sleuth Kit 4.4.0 install location for `fls.exe` and `icat.exe`. Set `UIFE_TSK_BIN` if the tools are elsewhere. The command-line fallback stages each selected file in the system temporary directory while reading it; this does not modify the source image. The libtsk development library remains preferable when available because it exposes richer metadata directly.

Disable libtsk discovery explicitly with `-DUIFE_ENABLE_LIBTSK=OFF`.

## CLI

```text
uife --help
uife info <image>
uife inspect <image>              # alias for info
uife list <image> [VFS-path]
uife extract <image> <VFS-path> -o <output-directory>
uife restore <image> -o <output-directory>
```

`uife --help` (or `uife -h`) lists the image formats with active read drivers, filesystem parsers registered in the current build, and formats that are recognized but not readable. These capability lists reflect the compiled dependencies; a readable container format does not imply that every filesystem it contains can be parsed.

Example for RAW NTFS and ext2/3/4 volumes, with libtsk enabled:

```text
uife info evidence.raw
uife list evidence.raw /
uife list evidence.raw / -p 2
uife extract evidence.raw /Users/example.txt -o ./selected
uife extract evidence.raw /Users/example.txt -o ./selected --partition 2
uife restore evidence.raw -o ./restored
uife restore evidence.raw -o ./restored --partition 2
uife list installation.iso /
uife extract installation.iso /README.TXT -o ./selected
```

`info` numbers detected partitions starting at 1. For `list`, `extract`, and `restore`, `--partition N` (or `-p N`) selects that partition; without it, the CLI preserves the existing behavior of selecting the first supported filesystem, or a filesystem directly at offset zero. The selected partition must have a registered parser. `restore` walks only the selected filesystem root. VFS paths are UTF-8, POSIX-style paths beginning with `/`.

CLI commands reject unrecognized options, duplicate output/partition options, missing option values, and extra positional arguments before opening the source image.

The QCOW2 driver supports standalone v2/v3 images with uncompressed clusters and validates their L1/L2 mappings and backing-file bounds. The VHDX driver validates both headers, region-table checksums, metadata geometry, and BAT bounds; it reads standalone clean-log dynamic/fixed disks and does not replay logs or resolve parent disks. The VMDK driver supports single-file sparse v1 images with uncompressed grains and descriptor-based images with `RW ... FLAT` extents; extent paths are required to be relative to and remain inside the descriptor directory, and every extent's bounds are validated. MBR partition scanning and ISO9660 browsing are covered through a multi-extent VMDK fixture, including a filesystem spanning partition and extent boundaries. The VDI driver supports VDI 1.1 dynamic and fixed images with a bounded block map. The QED driver supports standalone images without backing files or feature flags. ISO9660 and libtsk library parsers read through the image driver's bounded, read-only interface, so supported filesystems can also reside inside these virtual disks; the Windows `fls.exe`/`icat.exe` fallback still requires a RAW source file.

Btrfs send streams are detected by the `btrfs-stream` stream header and parsed as command sequences, not as block-device images. `info`, `list`, and `extract` can operate on self-contained streams; all command CRC32C values and path components are validated, and gaps in file writes read as zeroes. Incremental snapshots (including clone references), extent/file-attribute updates, fallocate/encoded writes, and verity operations are rejected because a single stream does not contain enough information to safely reconstruct those semantics. Metadata and xattrs available in supported stream commands are exposed through the VFS; verify platform support and privileges when restoring ownership or extended attributes.

E01 is readable when UIFE is built with a compatible libewf SDK; `info`, `list`, and `extract`/`restore` use the same commands as other supported image drivers. Without libewf, EWF/E01 is recognized but cannot be read. The DMG reader requires zlib and deliberately rejects compression and encryption methods that it cannot safely decode.

## Safety

The RAW driver opens source images using binary input only; filesystem parsers are expected to be read-only. Export writes are confined to the requested output directory, reject existing output files, and reject symlinked parent components. These controls do not replace hardware write blockers or a validated forensic workflow. Run against a verified evidence copy, record source hashes independently, and preserve the original media read-only. This MVP has not undergone forensic certification or validation against a representative corpus of damaged/encrypted images.
