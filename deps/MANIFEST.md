# 依赖包清单（Windows / MinGW-w64 UCRT64）

这份文件说明 `uife-deps-mingw-ucrt64.zip` 里装了什么、从哪来、怎么重建。

**二进制不入库**（静态库合计约 42 MB，且与工具链 ABI 强绑定，放进 Git 只会误导），
只有这份清单进版本库；包本体作为 Release 资产分发。

## 包本体

| 项 | 值 |
| --- | --- |
| 文件名 | `uife-deps-mingw-ucrt64.zip` |
| 大小 | 14,861,657 字节（14.2 MiB） |
| SHA256 | `25f6e01fcba37f35997e444383a9d709d8c21b0abd8689d55804852d7d12a538` |
| 生成时间 | 2026-11-05 10:51（`scripts/package-deps.ps1`） |
| 下载地址 | https://github.com/frederickMaster/ImageParsing/releases/download/deps-mingw-ucrt64/uife-deps-mingw-ucrt64.zip |
| 解包命令 | `powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1` |
| 解包结果 | `deps/sdk/<组件>/{include,lib}` + 生成的 `deps/uife-deps.cmake` |

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
| zlib | 1.3.2.1（Chromium motley 分支） | Zlib | `zlib/include` | `libz.a`（另有 `libzs.a` / `libz.dll.a`） | 133 KB | https://chromium.googlesource.com/chromium/src/third_party/zlib |
| Sleuth Kit | 4.15.0 | CPL-1.0 | `tsk/include` | `libtsk.a` | 18.4 MB | https://github.com/sleuthkit/sleuthkit |
| libewf | 20260924（experimental） | LGPL-3.0-or-later | `libewf/include` | `libewf.a` | 8.1 MB | https://github.com/libyal/libewf |
| wimlib | 1.14.5 | LGPL-3.0-or-later | `wimlib/include` | `libwim.dll.a` + `libwim-15.dll` | 2.8 MB + 48 KB | https://wimlib.net/ |
| libfsapfs | 20261030 | LGPL-3.0-or-later | `libfsapfs/include` | `libfsapfs.a` | 6.4 MB | https://github.com/libyal/libfsapfs |
| libfsrefs | 20261011 | LGPL-3.0-or-later | `libfsrefs/include` | `libfsrefs.a` | 4.8 MB | https://github.com/libyal/libfsrefs |

各组件的许可证与再分发义务（含 LGPL 静态链接的重新链接要求）见 [`docs/LICENSES.md`](../docs/LICENSES.md)，随发布包分发的声明在 [`docs/THIRD-PARTY-NOTICES.txt`](../docs/THIRD-PARTY-NOTICES.txt)。

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

## 维护约定

1. **改包必改清单**：版本、大小、SHA256 三样必须同步，否则 CI 的校验会失败（这是故意的）。
2. **不要用别的工具链凑**：包内静态库的 C++ ABI 与 gcc 16.2.0 UCRT 绑定，混用会在链接期
   才暴露，且报错信息毫无指向性。
3. **备份**：目前这份资产的两个副本是 `D:\tools`（构建机）与 GitHub Release；
   构建机若重装，按 `docs/DEPENDENCIES.md` 的上游地址与步骤重建。
