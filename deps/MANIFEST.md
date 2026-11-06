# 依赖包清单（Windows / MinGW-w64 UCRT64）

这份文件说明 `uife-deps-mingw-ucrt64.zip` 里装了什么、从哪来、怎么重建。

**二进制不入库**（库文件合计约 60 MB，且与工具链 ABI 强绑定，放进 Git 只会误导），
只有这份清单进版本库；包本体作为 Release 资产分发。

## 包本体

| 项 | 值 |
| --- | --- |
| 文件名 | `uife-deps-mingw-ucrt64.zip` |
| 大小 | 23,300,102 字节（22.2 MiB） |
| SHA256 | `fa24590a091fe46141cf3794164af3c4cbba9e795541779ddca45289687dfcfd` |
| 生成时间 | 2026-11-06 17:21（`scripts/package-deps.ps1`） |
| 下载地址 | https://github.com/frederickMaster/ImageParsing/releases/download/deps-mingw-ucrt64/uife-deps-mingw-ucrt64.zip |
| 解包命令 | `powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1` |
| 解包结果 | `deps/sdk/<组件>/{include,lib[,bin]}` + 生成的 `deps/uife-deps.cmake` |

包内**每个 libyal 组件同时带两种形态**：静态归档 `lib/*.a`（社区/内部构建）与导入库
`lib/*.dll.a` + `bin/*.dll`（闭源商业构建）。CMake 只按实际链接的那个文件去找运行时
DLL（`uife_collect_runtime_dll()` 里以 `.dll.a` 结尾为条件），所以两种构建不会互相污染。

**两种形态不能混用**，这是硬约束而不是风格问题。静态 `libewf.a` 把 libbfio 的实现编进了
自己内部，共享 `libfsapfs-1.dll` 用的是 `libbfio-1.dll`——同一个进程里就有了两份 libbfio。
uife 用其中一份建句柄、传给另一份去读，结构体布局对不上，最终 SIGSEGV 在
`libbfio_handle_is_open` 的读写锁上（栈顶是 `ApfsFsParser::mount`）。症状出现在挂载 APFS
时，根因却在链接组合上，只看崩溃栈是想不到的。

所以 `fetch-deps.ps1` 现在**统一决定**形态，并把两个组件从"交给 CMake 猜"改成显式钉住：

| 配置 | 命令 | 结果 |
| --- | --- | --- |
| 静态（默认，CI 用这个） | `fetch-deps.ps1` | `libewf.a` + `libfsapfs.a` + `libfsrefs.a`，libbfio 由各库自带，不链 `libbfio.dll.a` |
| 共享（商业版） | `fetch-deps.ps1 -Shared` | 三个 `.dll.a` + `libbfio.dll.a`，运行时一份 `libbfio-1.dll` |

以前 `LIBFSAPFS_LIBRARY` / `LIBFSREFS_LIBRARY` 只钉了 ROOT，形态留给 `find_library` 猜：
包里两种都有时它挑 `.dll.a`，于是和钉死成静态的 libewf 混在一起——这正是上面那次崩溃的
来源。现在两者都显式钉住，形态不一致时会直接打印告警说明后果。

条目名一律是正斜杠。之前用 `Compress-Archive` 打包时 Windows PowerShell 5.1 会写成反斜杠
（`libbfio\bin\libbfio-1.dll`），Windows 上解包看不出问题，`unzip`/`bsdtar` 却会解出一堆
名字里带反斜杠的平文件。`package-deps.ps1` 现在自己写条目名，不再经过 `Compress-Archive`。

校验：

```powershell
Get-FileHash deps\uife-deps-mingw-ucrt64.zip -Algorithm SHA256   # Windows
sha256sum deps/uife-deps-mingw-ucrt64.zip                        # bash
```

**工具链（ABI 硬约束）**：WinLibs MinGW-w64 **gcc 16.2.0 / UCRT / POSIX threads / SEH / r3**
（`g++.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r3) 16.2.0`）。
这套编译器编出的静态库**不能**给 MSVC 或别的 MinGW 变体链接；换工具链必须重新打包。
CI 的 `windows-mingw-full` 任务把同一个版本钉死并用 `actions/cache` 缓存。

## 组件

| 组件 | 版本 | 许可证 | 头文件 | 库文件 | 体积 | 上游 |
| --- | --- | --- | --- | --- | --- | --- |
| zlib | 1.3.2.1（Chromium motley 分支） | Zlib | `zlib/include` | `libz.a`（另有 `libzs.a` / `libz.dll.a` + `bin/libz.dll`） | 133 KB + 2.6 MB | https://chromium.googlesource.com/chromium/src/third_party/zlib |
| Sleuth Kit | 4.15.0 | CPL-1.0 | `tsk/include` | `libtsk.a` | 18.4 MB | https://github.com/sleuthkit/sleuthkit |
| libbfio | 20260623 | LGPL-3.0-or-later | `libbfio/include` | `libbfio.dll.a` + `bin/libbfio-1.dll` | 0.1 MB + 2.5 MB | https://github.com/libyal/libbfio |
| libewf | 20260924（experimental） | LGPL-3.0-or-later | `libewf/include` | `libewf.a`；`libewf.dll.a` + `bin/libewf-3.dll` | 8.1 MB；0.2 MB + 6.2 MB | https://github.com/libyal/libewf |
| wimlib | 1.14.5 | GPLv3+ 或 LGPL-2.1+（libwim 二选一；内含 MIT 的 libdivsufsort-lite） | `wimlib/include` | `libwim.dll.a` + `libwim-15.dll` | 2.8 MB + 48 KB | https://wimlib.net/ |
| libfsapfs | 20261030 | LGPL-3.0-or-later | `libfsapfs/include` | `libfsapfs.a`；`libfsapfs.dll.a` + `bin/libfsapfs-1.dll` | 6.4 MB；0.1 MB + 4.9 MB | https://github.com/libyal/libfsapfs |
| libfsrefs | 20261011 | LGPL-3.0-or-later | `libfsrefs/include` | `libfsrefs.a`；`libfsrefs.dll.a` + `bin/libfsrefs-1.dll` | 4.8 MB；0.1 MB + 3.5 MB | https://github.com/libyal/libfsrefs |

各组件的许可证与再分发义务见 [`docs/LICENSES.md`](../docs/LICENSES.md)，随发布包分发的
声明在 [`docs/THIRD-PARTY-NOTICES.txt`](../docs/THIRD-PARTY-NOTICES.txt)。
商业版走"独立 DLL"路线（LGPL §4(d)(1)），社区/内部版仍可全静态链接（§4(d)(0)）。

各库的构建参数、以及构建时踩过的坑（缺 `etc/fstab` 导致盘符映射成 `/cygdrive/d`、
release 包自带的 `common/config.h` 声明了 mingw 没有的 `strerror_r`、pkg-config shim、
libyal 不导出 libbfio 头等）都记在 [`docs/DEPENDENCIES.md`](../docs/DEPENDENCIES.md)，
重建前先读那一份。

几个和代码直接相关的结论：

- **zlib 是必需项**，DMG/GHO 解压依赖它；缺失时构建直接失败，其余后端都可以缺。
- **wimlib 用动态导入库**，运行时需要 `libwim-15.dll` 在 `PATH` 上；CMake 通过
  `UIFE_LIBWIM_RUNTIME` 记录 DLL 路径以便复制到输出目录。
- **libtsk 4.15.0 没有 XFS 支持**（`TSK_FS_TYPE_XFS` 从未合并），APFS 的
  `tsk_fs_open_pool*` 只有声明没有实现——CMake 探测失败是正确结果，不是本机编译问题。
- **libfsrefs 只能目录 + 元数据**：`libfsrefs_file_entry_get_size` 有声明无实现，
  所以 ReFS 的 `can_read_content()` 返回 false，`extract` 会直接拒绝而不是产出空文件。

## 重建流程

```powershell
# ① 在装好 SDK 的机器上打包（默认从 D:\tools 读取各组件，见脚本参数 -ToolsRoot）
powershell -ExecutionPolicy Bypass -File scripts\package-deps.ps1

# ② 上传为新 Release 资产（替换旧文件即可，URL 保持不变）
#    标签 deps-mingw-ucrt64，资产名 uife-deps-mingw-ucrt64.zip

# ③ 更新本文件的「大小 / SHA256 / 版本」与 docs/DEPENDENCIES.md
# ④ 若换了编译器或库版本，同步修改 .github/workflows/ci.yml 里的
#    windows-mingw-full：UIFE_DEPS_URL、镜像缓存 key、以及（如需要）SHA256 校验值
```

### 共享库形态（商业版）怎么重编

静态 SDK 与共享 SDK 用的是同一批头文件，区别只在链接目标。四个 libyal 库都用
`--enable-shared --disable-static --with-libbfio=<prefix>`（libbfio 自己用
`--enable-shared` 单独编一份），zlib 用**只含 `libz.a` 的前缀**（否则 libtool 会挑
`libz.dll.a`，发布包里就多出一个 `libz.dll`）。完整命令与踩坑记录见
[`docs/DEPENDENCIES.md`](../docs/DEPENDENCIES.md)。

编好之后把 `bin/*.dll` 与 `lib/*.dll.a` 并入上面表格里对应的 `deps/sdk/<组件>` 目录，
再跑一次 `package-deps.ps1`——两种形态就都在同一个包里了。日常构建不用手工指定这一长串：
`scripts\fetch-deps.ps1 -Shared` 会一次性把三个 libyal 组件和 libbfio 都钉成导入库。

局部重编（只换某个库）时可以用：

```bash
cmake -S . -B build-dll -G Ninja -DUIFE_USE_PKG_CONFIG=OFF \
  -DUIFE_LIBBFIO_ROOT=<sdk>/libbfio \
  -DUIFE_LIBBFIO_LIBRARY=<sdk>/libbfio/lib/libbfio.dll.a \
  -DLIBEWF_LIBRARY=<sdk>/libewf/lib/libewf.dll.a    -DLIBEWF_INCLUDE_DIR=<sdk>/libewf/include \
  -DLIBFSAPFS_LIBRARY=<sdk>/libfsapfs/lib/libfsapfs.dll.a -DLIBFSAPFS_INCLUDE_DIR=<sdk>/libfsapfs/include \
  -DLIBFSREFS_LIBRARY=<sdk>/libfsrefs/lib/libfsrefs.dll.a -DLIBFSREFS_INCLUDE_DIR=<sdk>/libfsrefs/include
```

三个 `LIB*_LIBRARY` 必须同时给成 `.dll.a`（或同时给成 `.a`）；只给其中一部分，剩下的会由
`find_library` 挑成另一种形态，就是前面说的两份 libbfio。

## 维护约定

1. **改包必改清单**：版本、大小、SHA256 三样必须同步，否则 CI 的校验会失败（这是故意的）。
2. **本文件与 CI 里的钉值要一起改**：`.github/workflows/ci.yml` 的
   `Fetch prebuilt dependency bundle` 步骤硬编码了 SHA256（当前为 `fa24590a…`）。
   换包时必须先上传资产、再同步改那里的期望值，顺序反了会让中间那次 CI 因为
   校验和不符直接失败。
3. **`package-deps.ps1` 保持纯 ASCII**：Windows PowerShell 5.1 会把无 BOM 的脚本
   按 ANSI 解码，非 ASCII 注释会让后面的 `[System.IO.Compression.*]` 类型解析失败
   （实测报 `找不到类型 [System.IO.Compression.ZipArchiveMode]`，与压缩逻辑毫无关系）。
   同一个坑也解释了为什么这个脚本的注释全是英文。
4. **不要用别的工具链凑**：包内静态库的 C++ ABI 与 gcc 16.2.0 UCRT 绑定，混用会在链接期
   才暴露，且报错信息毫无指向性。
5. **备份**：目前这份资产的两个副本是 `D:\tools`（构建机）与 GitHub Release；
   构建机若重装，按 `docs/DEPENDENCIES.md` 的上游地址与步骤重建。
