# UIFE — Universal Image Forensics Engine

[![CI](https://github.com/frederickMaster/ImageParsing/actions/workflows/ci.yml/badge.svg)](https://github.com/frederickMaster/ImageParsing/actions/workflows/ci.yml)

UIFE is a read-only, C++20 command-line image inspection and extraction MVP. It is designed around separate image-driver, filesystem-parser, VFS, and extraction layers so additional forensic formats can be integrated without coupling the CLI to individual parsers. Building requires CMake and zlib; libtsk, libewf, wimlib, libfsapfs, and libfsrefs remain optional.

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
  cli_argument_tests.cmake
  compat_matrix.py          # 真实样本兼容性矩阵（见 tests/corpus/README.md）
  corpus/README.md          # 语料目录约定
scripts/
  make_samples.ps1          # 自建 NTFS/FAT32/ReFS 样本（diskpart + VHD）
  tools/make_e01.c          # libewf 写支持 → E01 样本
  tools/make_wim.c          # wimlib → WIM 样本
docs/SAMPLES.md             # 样本生成与语料说明
  corpus/README.md          # 语料目录约定；样本本身不入库
.github/workflows/ci.yml    # Linux/macOS/Windows 构建矩阵
CMakePresets.json
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
| ISO9660 filesystem parser | Implemented with Joliet UTF-16 names, Rock Ridge NM names, PX mode/UID/GID/link-count/hard-link identity metadata, SL symbolic links, and bounded CE continuation areas when announced by the root SUSP SP record; also supports multi-extent files, directory-record modification timestamps (including UTC offset), dual-endian field consistency checks, and extents bounded by the declared volume size. CE continuations are capped at 1 MiB each/4 MiB total with depth and cycle checks. Volume-root/host symlink components and special nodes are explicitly unsupported |
| Btrfs send stream parser | Built-in read-only reconstruction of standalone v1-v3 streams with CRC32C validation, directories, regular-file writes/truncation, links, symlinks, supported inode metadata and xattrs; incremental snapshots, extent/file-attribute updates, clone/fallocate/encoded-write/verity commands are rejected |
| NTFS, FAT/exFAT, HFS+, and ext2/3/4 filesystem parsers | Optional libtsk backend reads through any supported read-only image driver, including virtual-disk and BIN+CUE drivers; the Windows `fls.exe`/`icat.exe` fallback is limited to RAW files |
| APFS container parser | Optional `libfsapfs` backend (libyal 20261030). The container is handed to libfsapfs through a libbfio handle backed by `IImageDriver::read_at`, so E01/VHDX/QCOW2/VHD sources are browsed directly instead of being staged on disk first. Supports multi-volume containers (volume 0 is mounted), UTF-8 names, POSIX mode/UID/GID/link count, creation/modification/access times, symlink targets, and extended attributes (`com.apple.*`) reported and exported as named streams. Encrypted volumes are unlocked with a password read from stdin. Not yet validated against a real macOS APFS image; Fusion containers and per-snapshot roots are not exposed. libtsk cannot serve as the APFS backend: `tsk_fs_open_pool()` / `tsk_fs_open_pool_decrypt()` are declared in its public header but never implemented, in 4.15.0 and upstream alike |
| XFS filesystem parser | XFS superblock detection on supported image drivers. Browsing requires a backend with `TSK_FS_TYPE_XFS`; neither TSK 4.15.0 nor the upstream development branch provides XFS, so XFS stays detection-only |
| Filesystem metadata | libtsk reports mode, UID/GID, link count, available times, and symlink targets; filesystem geometry and ext-family file-size bounds are validated before browsing or extraction. For NTFS, the parser additionally reads `$STANDARD_INFORMATION` file-attribute flags (sparse, compressed, encrypted, reparse point), the `$Secure` `security_id`, named `$DATA` alternate data streams (name and size), and `$REPARSE_POINT` symlink/junction targets (tag plus substitute path). For uncompressed, unencrypted NTFS default data streams the parser also exposes the on-disk allocation map as byte-range extents (allocated versus sparse), capped at 4096 runs per file. `$Secure:$SDS` is decoded into a `security_id` index of self-relative security descriptors: `list --json` reports the owner/group SID and every DACL/SACL ACE (type, inherited flag, raw mask, SDDL-form SID) per file. SID-to-account-name resolution is intentionally not performed (no SAM/registry lookup) |
| Extracted file verification | SHA-256 of source stream compared to SHA-256 read back from the exported file; ISO9660 integration test also verifies the source image hash is unchanged after browsing and extraction |
| Image signature recognition | QCOW2, VMDK, VDI, QED, VHDX, EWF/E01, WIM/ESD, ISO9660, DMG, GHO, BIN+CUE |
| EWF/E01 image driver | Optional libewf-backed, read-only segmented EWF access with random reads; verified with a generated two-segment E01 containing a FAT12 test file, including extraction hash equality and unchanged source hashes. A separate synthetic two-segment EWF under a Chinese Windows path was also opened successfully. Representative real-world EWF samples have not been tested |
| WIM/ESD filesystem parser | Optional wimlib-backed, read-only browsing of images exposed under `/images/N`; file contents are staged on demand in a temporary directory. Split WIM sets (`.swm`) are supported when opened from the first part with all numbered parts in the same directory. Named data streams are enumerated with name and size but are not exported, because wimlib does not expose a per-stream extraction API. Reparse-point semantics, ACLs, security descriptors, and encrypted ESD are not supported |
| Apple DMG image driver | Read-only, single-segment UDIF/koly parsing for XML or binary plist blkx maps with raw, zero/ignore, and zlib runs; segmented images and other compression/encryption run types are rejected |
| Norton Ghost GHO image driver | Read-only random-access view of one to four unencrypted partitions with no compression, Fast LZ, or zlib compression levels 3-9. Same-directory `.ghs` span files are discovered from the matching `.gho` stem, checked for compatible span type, compression, and image ID, and read as one virtual stream without staging or modifying the source files. A valid Track 0 MBR is composed into a disk view only when the primary partition layout exactly matches the decoded partitions; a single partition without a matching MBR is exposed directly, while unsafe multi-partition layouts are rejected. Legacy compression type 1 and encrypted images are identified but rejected |
| Other filesystems and encryption | ReFS has an optional `libfsrefs` backend: the volume is opened through the same libbfio bridge as APFS, and directory listing plus timestamps/attribute flags work. Two hard limits are documented rather than hidden: libfsrefs exposes no file-content read API, and the library only implements the early ReFS layout, so volumes created by Windows Server 2016 or later fail inside libfsrefs (`invalid metadata block signature`) — reproduced with libfsrefs' own `fsrefsinfo`, not an adapter bug. Because no content can be read, `extract`/`restore` refuse up front instead of writing empty files. XFS is detection-only because no supported library exposes it |

## 项目进度与待办（更新于 2026-11-04）

本项目仍是持续开发中的只读 CLI MVP，不等同于原始需求中“100% 格式覆盖”或经过认证的取证产品。下列“已实现”表示代码存在相应能力；具体能否使用仍取决于格式变体、可选依赖和输入镜像的结构。识别到格式不代表能够解析全部变体。

### 已实现

- **基础框架与安全措施**：C++20/CMake 命令行程序；镜像驱动、文件系统解析器、VFS 和提取器分层。源镜像按只读方式访问，解析操作不应修改源镜像；提取器限制输出路径、拒绝覆盖已有文件，并对常规文件做导出后 SHA-256 校验。
- **容器/镜像驱动**：RAW/IMG、ISO、VMDK、VDI、QED、QCOW2、VHDX、BIN+CUE、DMG、GHO，以及在启用 libewf 时的 EWF/E01。每类驱动只支持上表和后文明确列出的变体；不支持的压缩、加密、父盘、日志或高级特性会拒绝处理。
- **分区及文件系统**：MBR/GPT 分区扫描；ISO9660/Joliet 和部分 Rock Ridge；Btrfs send v1-v3 的独立流解析；依赖 libtsk 的 NTFS、FAT/exFAT、HFS+、ext2/3/4 与 XFS（仅当后端暴露该类型）；依赖 libfsapfs 的 APFS（含加密卷解锁，扩展属性按命名数据流暴露）；依赖 libfsrefs 的 ReFS（目录与元数据；该库不提供内容读取）。
- **分卷镜像**：EWF 分段读取；GHO 同目录 `.ghs` 分卷自动发现与只读逻辑读取；WIM `.swm` 从首卷打开、引用同目录编号分卷，不生成合并镜像。
- **CLI 产品化（2026-11-04）**：统一退出码表（0 成功 / 1 运行时错误 / 2 用法错误 / 3 镜像驱动未接入 / 4 文件系统解析器缺失）；`--json` 机器可读输出（info/probe/list/extract/restore/bench；错误以 `{"error":{...}}` 输出到 stdout）；`--manifest <文件>` 生成可在导出目录内直接 `sha256sum -c` 的清单；`batch` 批处理清单（逐行执行、失败不中断、JSON 模式输出 JSONL）；`probe` 逐位置报告识别结果与后端可用性；`bench` 顺序读取吞吐基准。EWF 驱动在 Windows 下通过临时切换工作目录规避 libewf 源码构建版 libcpath 的绝对路径展开缺陷（详见 2026-11-03 工作记录）。
- **元数据与数据流（2026-11-04）**：libtsk NTFS 后端解析 `$STANDARD_INFORMATION` 的文件属性标志、`security_id`，枚举命名 `$DATA` 备用数据流，解析 `$REPARSE_POINT` 的符号链接/联接点目标，并以字节区间形式报告未压缩/未加密默认数据流的分配映射（含稀疏洞，最多 4096 段）；`$Secure:$SDS` 被解码为 security_id → 安全描述符索引，`list --json` 输出 owner/group SID 与每条 DACL/SACL ACE（类型、inherited 位、原始掩码、SDDL 形式 SID；不做 SID→账户名解析）；提取器把可读的命名流导出为 `<文件名>__ads__<编码流名>` 侧车文件，逐个做 SHA-256 校验并写入清单。WIM 命名流可枚举（wimlib 无单流提取 API），标记为不可导出而不中断整卷提取。
- **验证及交付**：已有合成格式/集成测试覆盖常规读取、边界拒绝、部分提取哈希、ADS 侧车导出与源镜像哈希不变。当前 Windows MinGW Release 与无 WIM 构建的 CTest 均为 4/4 通过；Windows x64 ZIP 可通过 `scripts/package.ps1` 生成。新增 `tests/compat_matrix.py` 样本兼容性矩阵入口（`-DUIFE_CORPUS_DIR` 启用 CTest 用例）、`.github/workflows/ci.yml` 三平台构建矩阵与 `CMakePresets.json`；CI 尚未在真实仓库跑绿。（注：2026-11-02 时的原始构建目录指向已失效的工具链；2026-11-03 起本机可复现构建位于 `build-local`/`build-local-tsk`/`build-local-full`，工具链与依赖见 `.workbuddy/memory/2026-11-03.md`。）
- **产品范围**：按用户决定仅维护 CLI，不包含 Qt GUI。

### 尚未完成 / 验收缺口

| 项目 | 当前状态与后续工作 |
|---|---|
| ReFS | 已接入 `libfsrefs`（20261011 快照）后端并完成构建：容器经 libbfio 桥接读取镜像驱动，可枚举目录、读取创建/修改/访问时间与文件属性标志。两条硬限制已确认：库里没有读取文件内容的接口（`extract`/`restore` 直接拒绝，不生成空文件），且该库只实现早期 ReFS 布局，Windows Server 2016 及以后创建的卷会在 ministore 容器树处失败（用其自带 `fsrefsinfo` 对同一卷复现）。因此 ReFS 目前属于“后端在位、真实卷仍读不了”，需要等库更新或换后端。 |
| XFS | 可检测部分 superblock；当前本机 libtsk SDK 没有暴露 XFS 类型，因此没有可用浏览后端。需接入有相应能力的后端并验证镜像样本。 |
| APFS 与加密 | 已接入 `libfsapfs`（20261030）后端：容器经 libbfio 回调直接读取镜像驱动，不落盘中转；支持多卷容器（挂载卷 0）、时间戳/POSIX 权限/符号链接目标、`com.apple.*` 扩展属性（按命名数据流枚举与导出），以及 `--password-stdin` 解锁加密卷。libtsk 路线确认不可行（`tsk_fs_open_pool*` 只有声明没有实现）。仍缺 APFS 真实样本验证，融合容器与快照未作为独立根暴露。 |
| WIM/ESD 加密 | 普通 WIM/ESD 与 split WIM 可按已列能力浏览；命名数据流可枚举名称与大小但无法导出（wimlib 未公开单流接口）；加密 ESD 解密尚未实现。 |
| 元数据完整恢复 | 已能导出 NTFS 命名数据流内容、报告文件属性标志（稀疏/压缩/加密/重解析点）、`security_id`、重解析目标，以及未压缩/未加密数据流的分配映射（extents，含稀疏洞）。尚未完成：reparse point 的原生重建（当前只导出目标与标签）、WIM ACL/安全描述符、SID→账户名解析（需要读 SAM/注册表），以及 CLI Sleuth Kit 后备路径的元数据（该路径只提供文件名、大小、UID/GID 与基本类型）。NTFS 的 `$Secure:$SDS` 已可解码（owner/group SID 与 DACL/SACL ACE）；压缩/加密数据流的分配映射不做推断。 |
| 格式变体覆盖 | 现有驱动仅覆盖表中明确列出的受限变体，不等于对 QCOW2/VMDK/VDI/VHDX/DMG/GHO/EWF 等格式的所有版本、压缩、加密、差分盘和损坏情况提供支持。须逐项扩展并用独立样本验证。 |
| 全平台构建 | Windows Release 已在本机验证；macOS/Linux 的 CI/交叉构建、依赖组合及行为由新增的 `.github/workflows/ci.yml`（Linux core、Linux 可选后端、macOS core、Windows MSVC core）覆盖，但尚未在真实仓库跑绿。 |
| 兼容率及性能指标 | 已提供样本兼容性矩阵脚本、`uife bench` 顺序读取基准，以及可复现的自建样本（`scripts/make_samples.ps1` + `scripts/tools/`，见 `docs/SAMPLES.md`）；本机自建语料 9/9 通过（含 ReFS 卷：预期 list 失败，用于锁住 libfsrefs 的能力边界）。仍缺真实/损坏/加密/多卷样本，99% 识别成功率与 100 MB/s、10 GB/10 秒指标未测量。 |
| 取证验证 | 当前测试以合成样本和有限 SDK 验证为主。已提供把真实样本接入 CTest/CI 的矩阵工具，但语料本身不在仓库内；尚未对各格式真实样本、损坏镜像、不同来源分卷及边界/压力场景执行完整兼容性矩阵测试，也未取得取证认证。 |

建议后续顺序：先补充并验证真实样本测试矩阵，再选定可维护的 ReFS/XFS/APFS 后端，之后完善元数据/加密支持，最后进行跨平台构建和性能基准。任何“已识别”格式均应继续保持明确的能力状态和不支持原因。

Only allocated filesystem objects and regular-file data returned by the active Sleuth Kit backend are currently restored. POSIX ownership is restored when the parser explicitly reports UID/GID, and extended attributes are restored on Linux and macOS when supplied by a parser; failures are reported rather than silently ignored. The Windows command-line fallback gets file names, sizes, UID/GID, and basic node types from `fls`; it does not recover full permission bits, timestamps, extended attributes, or symlink targets. NTFS alternate data streams are exported as sidecar files when the libtsk backend is used; sparse allocation maps are reported in metadata for uncompressed and unencrypted streams, but exported files are not re-created as sparse files. Complete Windows ACL/security descriptors and reparse-point re-materialisation remain unsupported: reparse targets are reported, not re-created, and only the `security_id` index is exposed for ACL work done by downstream tools. Unsupported metadata or filesystem behavior must not be mistaken for a complete forensic restoration. Symlink targets are exported only when the libtsk library backend returns them.

The libtsk backend bounds every read to the selected image/partition extent and rejects filesystems whose declared block geometry exceeds that extent. For ext-family filesystems, file sizes beyond the format's 32-bit logical-block address limit are marked invalid, shown as `[!]` in `list`, and refused by extraction rather than used for unbounded reads. APFS container volume names are exposed below a virtual root, with unsafe path-component bytes percent-encoded. APFS file sizes larger than the container image are treated as invalid for conservative extraction. Supply an encrypted APFS password through standard input, not as a command-line argument, using `--password-stdin`; for example, `uife list apfs.raw /Volume --password-stdin < password.txt`. Protect any password file according to your evidence-handling policy. A wrong password or unsupported encrypted volume is reported as an error; UIFE never writes to the source image.

The ISO9660 parser provides directory listing and file reads, including Joliet Unicode names and consecutive multi-extent files. Interleaved files and multi-extent directories are rejected explicitly. It reads through the image driver's random-access interface, so it can browse ISO filesystems exposed by QCOW2, VHDX, VMDK, VDI, QED, DMG, and supported BIN+CUE drivers without staging or modifying backing files. DMG support is limited to single-segment UDIF images with XML or binary property lists and raw, zero/ignore, or zlib blkx runs; segmented, ADC, bzip2, LZFSE, encrypted, and other variants are rejected. BIN+CUE support covers same-directory BINARY files containing sequential MODE1 data tracks (2048-byte sectors or 2352-byte sectors with the 16-byte sync/header removed); tracks are concatenated from each INDEX 01. Audio tracks, MODE2, and mixed sector formats within one BINARY file are rejected. QCOW2 support covers standalone v2/v3 images with standard cluster geometry, no backing files, encryption, snapshots, or feature flags; compressed clusters and unsupported mappings are rejected. VHDX support covers standalone dynamic/fixed images with a zero log GUID, no parent, and fully present, absent, or zero payload blocks; logged images, differencing disks, and partially present blocks are rejected rather than replayed or approximated. VMDK support includes single-file, uncompressed sparse v1 and descriptor-based flat extents; descriptor extents may span multiple files and reads can cross their boundaries. Snapshots and compressed sparse grains are not supported. VDI support includes version 1.1 dynamic/fixed images with standard block size and no block-extra bytes; differencing images are rejected. QED support is limited to standalone images with no backing file and no compatible, autoclear, or incompatible feature flags; other variants are rejected. XFS superblocks are identified from validated geometry; browsing is registered only when the detected libtsk development version exposes XFS support. When libtsk is available, its library backend reads through the image driver's bounded, read-only callback; the Windows Sleuth Kit command-line fallback is limited to RAW source files. HFS+ detection validates the volume header signature, version, allocation-block geometry, and image bounds before exposing the TSK parser. APFS containers are read by the optional libfsapfs backend, which receives a libbfio handle over the image driver instead of a temporary copy of the partition; volume 0 of a container is mounted, extended attributes are exposed and exported as named streams, and encrypted volumes are unlocked with a password read from stdin. This backend has not been validated against a real macOS image yet, and Fusion containers and per-snapshot roots are not exposed.

## Build

Requires CMake 3.20+ and a C++20 compiler. libtsk, libewf, wimlib, libfsapfs, and libfsrefs are optional at configure time. Without them, UIFE still builds the RAW, ISO9660, QCOW2, QED, VHDX, VDI, and documented VMDK drivers; EWF and WIM/ESD remain detection-only. On Windows, installed Sleuth Kit command-line tools are used for RAW NTFS/ext2/3/4 browsing and extraction; on other platforms the development library is required.

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

List the virtual image index directory first, then browse a selected image using `/images/N`, for example `uife list install.wim /images` and `uife list install.wim /images/1`. WIM/ESD files are opened read-only; file data is staged in a private temporary directory on demand and removed with the parser. Split WIM sets can be opened by selecting the first `.swm` part; all numbered sibling parts must be present in the same directory. No merged WIM is created, and incomplete or mismatched sets are reported as errors. This does not modify the source images. Encrypted ESD, reparse-point behavior, complete Windows ACL/security descriptors, and alternate data streams are not currently supported.

On Windows, point `UIFE_LIBEWF_ROOT` at a compiler-compatible libewf SDK containing `include\libewf.h` and a matching library; `scripts\build.ps1` passes these paths to CMake and adds the SDK root to dependency discovery. Static libewf builds also need compatible zlib and bzip2 development libraries discoverable through CMake (or included under the SDK root). The libewf build and ABI must match the selected compiler. UIFE uses libewf's wide-character path APIs on Windows; the SDK must also be built with Unicode system-string support for non-ASCII segment paths. This was verified with a local Unicode-enabled libewf build and a synthetic Chinese-named split EWF fixture.

APFS browsing is enabled when a compatible `libfsapfs` development package is found. Set `UIFE_LIBFSAPFS_ROOT` to its root directory (`include\libfsapfs.h`, `lib\libfsapfs.a`); pkg-config discovery also works when `libfsapfs.pc` is on `PKG_CONFIG_PATH`. A static `libfsapfs.a` requires zlib at link time — the `.pc` file only lists it under `Libs.private`, so UIFE adds `ZLIB::ZLIB` to both the capability probe and the link line. libyal does not install the headers of its bundled libbfio/libcerror, so `src/drivers/fs/ApfsFsParser.cpp` declares the two libbfio entry points it needs; the CMake probe links against them so a build that lacks them falls back to recognition-only instead of failing at the final link. Set `-DUIFE_ENABLE_LIBFSAPFS=OFF` to disable discovery. Building the SDK on Windows is documented in `docs/DEPENDENCIES.md`.

ReFS browsing is enabled when a compatible `libfsrefs` development package is found (`-DUIFE_LIBFSREFS_ROOT=` or pkg-config). libfsrefs ships no release, only a git snapshot; `docs/DEPENDENCIES.md` records how its 16 bundled dependencies were assembled and built on Windows. The backend shares the libbfio bridge with APFS and deliberately reports `can_read_content() == false`, because libfsrefs has no file-content API — `extract` and `restore` therefore fail immediately with an explanation instead of producing empty files. `-DUIFE_ENABLE_LIBFSREFS=OFF` disables discovery.

Build and run tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The CLI argument-validation test verifies that malformed options are rejected before the input image is opened.

Configure presets are provided for the common cases (preset support requires CMake 3.21 or newer):

```sh
cmake --preset core      # no optional backends; always buildable
cmake --build --preset core
ctest --preset core
cmake --preset full      # libtsk/libewf/wimlib/libfsapfs when discovered
```

To include the sample compatibility matrix in CTest, point `UIFE_CORPUS_DIR` at a corpus laid out as described in `tests/corpus/README.md`:

```sh
cmake -S . -B build -DUIFE_CORPUS_DIR=/evidence/uife-corpus
ctest --test-dir build -R compat_matrix --output-on-failure
```

`tests/compat_matrix.py` can also be run directly against any corpus:

```sh
python3 tests/compat_matrix.py --exe build/uife --corpus /evidence/uife-corpus \
  --report build/compat-matrix.md --json build/compat-matrix.json
```

CI coverage is defined in `.github/workflows/ci.yml` (Linux core, Linux with optional backends, macOS core, Windows MSVC core, plus a manually triggered corpus job).

Windows PowerShell (MSYS2/MinGW with the matching libtsk development headers and library, if NTFS/ext support is required):

```powershell
.\scripts\build.ps1
```

The build script automatically selects the per-user 4.15.0 SDK when it detects the matching UCRT MinGW compiler. For another installed toolchain/ABI, set `UIFE_LIBTSK_ROOT` to the root of a compatible SDK before running the script.

To build, run the tests, and package a Windows x64 Release ZIP with the executable, runtime DLLs, usage notes, and third-party license notices:

```powershell
.\scripts\package.ps1 `
  -Generator "MinGW Makefiles" `
  -BuildDirectory ".\build-uife-package" `
  -OutputArchive ".\dist\uife-windows-x64.zip"
```

Set `UIFE_LIBTSK_ROOT`, `UIFE_LIBEWF_ROOT`, `UIFE_LIBWIM_ROOT`, and `UIFE_LIBFSAPFS_ROOT` to compatible SDKs before packaging when those optional backends are needed. The script refuses to overwrite an existing archive unless `-Force` is explicitly supplied.

If libtsk is installed outside the compiler's default search paths, pass its include directory and library explicitly:

```powershell
$tskRoot = "$env:LOCALAPPDATA\TheSleuthKit\4.15.0-ucrt64"
cmake -S . -B build-uife -DCMAKE_BUILD_TYPE=Release `
  "-DLIBTSK_INCLUDE_DIR=$tskRoot\include" `
  "-DLIBTSK_LIBRARY=$tskRoot\lib\libtsk.a"
cmake --build build-uife --config Release
ctest --test-dir build-uife -C Release --output-on-failure
```

The `4.15.0-ucrt64` location is a per-user SDK built from the official Sleuth Kit 4.15.0 sources for 64-bit UCRT MinGW; its static build omits optional zlib and libewf integrations. Other compilers require a libtsk library built for their own architecture and ABI. This Sleuth Kit version exposes neither an XFS filesystem type nor working APFS pool entry points (the pool functions are declared but not implemented upstream), so XFS remains detection-only and APFS is served by the separate libfsapfs SDK described above. The Chocolatey `sleuthkit` package is a command-line application package; it does not necessarily contain C++ development headers, and its bundled 32-bit MSVC library cannot be linked into a 64-bit MinGW build. Windows UIFE searches `PATH`, the `UIFE_TSK_BIN` directory, and the standard Chocolatey Sleuth Kit 4.4.0 install location for `fls.exe` and `icat.exe`. Set `UIFE_TSK_BIN` if the tools are elsewhere. The command-line fallback stages each selected file in the system temporary directory while reading it; this does not modify the source image. The libtsk development library remains preferable when available because it exposes richer metadata directly.

Disable libtsk discovery explicitly with `-DUIFE_ENABLE_LIBTSK=OFF`.

## CLI

```text
uife --help
uife info <image>
uife inspect <image>              # alias for info
uife probe <image>                # detection + backend availability per location
uife list <image> [VFS-path]
uife extract <image> <VFS-path> -o <output-directory> [--manifest <file>]
uife restore <image> -o <output-directory> [--manifest <file>]
uife bench <image> [--size <bytes>]
uife batch <manifest-file>
```

### Global options

- `--json` — machine-readable JSON output for every command (place it anywhere before the command arguments; `batch` emits one JSON object per manifest line).
- `--manifest <file>` — after `extract`/`restore`, write a `sha256sum -c` compatible manifest (`<sha256>  <path>` per line) to the given file. Paths are relative to the output directory, so `sha256sum -c SHA256SUMS` works from inside that directory.
- `--password-stdin` — APFS encrypted-volume password from stdin (list/extract/restore only).
- `--size <bytes>` — read limit for `bench` (defaults to the whole image).

### Exit codes

| Code | Meaning |
| ---- | ------- |
| 0 | Success |
| 1 | Runtime error (I/O failure, parse failure, unreadable manifest, batch line failure) |
| 2 | Usage error (unknown command/option, invalid or missing arguments, out-of-range partition number) |
| 3 | Image format recognized but no read driver compiled in |
| 4 | Filesystem recognized but no parser available in the current build |

In `--json` mode, errors are reported as a single `{"error":{"code":N,"message":"..."}}` object on stdout instead of stderr text.

### Capability probing and throughput benchmark

`uife probe <image>` reports one line per location (the image start and every detected partition) with the recognised filesystem and whether the current build can browse it, for example `direct: offset=0 length=45056 fs=ISO9660 [可浏览]`. The JSON form adds `scheme`, `partition_type`, `filesystem`, and `readable` per location, which makes it suitable as the front end of a compatibility matrix.

`uife bench <image> [--size <bytes>]` reads the image sequentially through the read-only driver and reports bytes, elapsed time, and MiB/s. It measures the image driver (including decompression for virtual disks) only; it does not mount a filesystem, so it is a lower bound for end-to-end extraction throughput. Results depend on the host, cache state, and image layout, and must be reported together with that context.

### Alternate data streams

NTFS named data streams are exported as sidecar files next to the parent file: `<name>__ads__<encoded-stream>`. The encoded stream name keeps only `[A-Za-z0-9._-]` and percent-encodes every other byte; names longer than 96 encoded characters are truncated and suffixed with a FNV-1a hash so different streams cannot collide. Each sidecar is verified with SHA-256 like a regular file, is listed in the manifest, and carries its source stream name in JSON output (`"stream": "Zone.Identifier"`). Streams attached to directories are reported in listings and JSON but are not exported; hard-link duplicates are not re-exported because they share the same MFT record. Streams that a backend can only describe but not read — currently WIM named streams, which wimlib does not expose per stream — are reported by `list --json` with `"readable": false` and are skipped by extraction instead of failing the run.

### Batch mode

`uife batch <manifest-file>` executes one command per manifest line (omit the leading `uife`). Blank lines and lines starting with `#` are ignored. Processing continues after a failed line; the final exit code is 0 only when every line succeeded. With `--json`, each line produces `{"line":N,"command":"...","status":"ok","result":{...}}` or `{"line":N,"command":"...","status":"error","code":N,"message":"..."}` on stdout, one object per line (JSONL).

Example manifest:

```text
# survey all evidence images
info D:/evidence/disk1.E01
list D:/evidence/disk2.raw /
extract D:/evidence/disk2.raw /Users -o D:/out/disk2 --manifest D:/out/disk2/SHA256SUMS
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
