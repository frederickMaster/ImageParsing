# 可选依赖的获取与构建（Windows / MinGW-w64）

UIFE 的可选后端都是按 `UIFE_ENABLE_*` + API 能力探测启用的：库不存在或 API 不完整时，
对应格式只会"识别但不可浏览"，构建不会失败。下面记录本机（Windows Server 2016 +
MinGW-w64 UCRT 16.2.0 + CMake 4.4.5 + Ninja）实际验证过的获取方式与坑。

## 换一台机器：先考虑复用已编好的依赖包

下文所有源码构建步骤（尤其 libfsapfs / libfsrefs）都是重复劳动。**这些库不随仓库分发**
（仓库只包含 `libewf.h` 等头文件，其余四个库的库文件全在外部 SDK 目录），因此换机器时
优先复用一份已经编好的快照：

```powershell
# ① 在已经装好 SDK 的机器上（默认读取 D:\tools\{zlib,tsk-*-ucrt64,libewf-sdk,
#    wimlib-sdk,libfsapfs-sdk,libfsrefs-sdk}）打一个包，约 14 MB
powershell -ExecutionPolicy Bypass -File scripts\package-deps.ps1

# ② 把 deps\uife-deps-mingw-ucrt64.zip 拷到新机器，解包并生成 CMake 初始缓存
powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1

# ③ 直接用生成的缓存文件配置，无需再手写一堆 -DZLIB_LIBRARY ...
cmake -S . -B build -G Ninja -C deps/uife-deps.cmake `
  -DCMAKE_CXX_COMPILER=D:/tools/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=D:/tools/ninja/ninja.exe
```

打包产物布局固定为 `<组件>/{include,lib}`，外加记录工具链的 `manifest.json`。
`fetch-deps.ps1` 会把 `manifest.json` 里的编译器与当前 `g++ --version` 比对，不一致就告警。

**ABI 是硬约束**：MinGW-w64 UCRT 编出来的静态库不能被 MSVC 链接，反之亦然。依赖包只对
同一套工具链有效；换编译器需要重新打一份包（`package-deps.ps1` 的参数可指定不同的
`-ToolsRoot` / `-CompilerPath`）。

**CI 里的取法分两类**：Linux 用发行版包（`apt install libtsk-dev libewf-dev libwim-dev`），
Windows 的 MSVC 任务用 `vcpkg install zlib:x64-windows`。这五个可选库在 vcpkg 和 MSYS2 里
都没有端口（`sleuthkit` / `libewf` / `wimlib` / `libfsapfs` / `libfsrefs` 全部查无此包），
而且它们的上游构建系统都不是 CMake，MSVC 版需要先移植各自的构建系统。

因此 Windows 上的"全后端"由 `windows-mingw-full` 任务覆盖：它下载 Release 资产
`uife-deps-mingw-ucrt64.zip`（就是上面这份依赖包）、用固定版本的 WinLibs gcc 16.2.0
UCRT 解包配置，然后要求 `uife-backends.txt` 里五个后端**全部为 ON**，缺一个就失败。
换依赖包（重新打包并上传）时要同步改 CI 里 `winlibs-16.2.0-...` 的缓存 key 与下载 URL，
否则 ABI 对不上会在链接阶段才炸。

**发布新依赖包的步骤**：`scripts/package-deps.ps1` → 把 `deps/uife-deps-mingw-ucrt64.zip`
挂到 Release 资产（当前标签 `deps-mingw-ucrt64`）→ CI 自动拉取。

## 需要的工具

| 工具 | 用途 | 备注 |
| --- | --- | --- |
| MinGW-w64 UCRT x86_64 | 编译器 | 本机在 `D:\tools\mingw64` |
| CMake ≥ 3.20 + Ninja | 构建 | `D:\tools\cmake`、`D:\tools\ninja` |
| pkg-config（可选） | libyal 的 configure | 无真身时可用脚本 shim，需支持 `--atleast-pkgconfig-version` |
| autotools（仅 libfsrefs 快照需要） | 生成 configure | 见下节"没有 MSYS2 安装器时" |

## 各依赖的获取

### Sleuth Kit 4.15.0（NTFS/FAT/exFAT/HFS+/ext2-4）

官方源码包，`./configure --without-afflib --without-libewf --disable-java` 后
`mingw32-make`。注意：

- 官方 `tsk_fs.h` 声明了 `tsk_fs_open_pool()` / `tsk_fs_open_pool_decrypt()`，但**没有任何
  实现**（`develop` 分支同样没有），因此 TSK 的 APFS 路线不可用，CMake 探测失败是正确结果。
- `TSK_FS_TYPE_XFS` 在官方代码中不存在（XFS 从未合并），XFS 只能保持"识别"。
- 静态库链接时需要 zlib；`check_cxx_source_compiles` 会编译**并链接**，所以探测必须把
  zlib 加进 `CMAKE_REQUIRED_LIBRARIES`，否则会把能力误判为缺失。

### libewf（E01/EWF）

官方 release 包自带 `configure`，`--enable-wide-character-type --disable-shared` 静态构建。
已知坑：

- 源码版 `libcpath_path_get_full_path_wide` 对绝对路径会拼错工作目录（出现
  `\\?\D:\D:\...`）→ UIFE 在 `EwfImageDriver` 里用 RAII 临时切换工作目录规避。
- `po/Makevars` 生成与 `LOCALEDIR` 宏会让 libtool 崩溃：删掉 `libclocale/Makefile` 中
  `-DLOCALEDIR=...` 那一行即可（`HAVE_BINDTEXTDOMAIN` 未定义，该宏是死代码）。

### wimlib（WIM/ESD）

`--enable-shared` 更容易：静态库需要额外的 `ntdll` 依赖，导入库 + 运行时 DLL 更省心，
CMake 里用 `UIFE_LIBWIM_RUNTIME` 指定 DLL 以便 copy 到输出目录。

### libfsapfs（APFS，含密码解密）

源码：`libfsapfs-experimental-20261030.tar.gz`（libyal）。本机已按下列步骤成功构建出
`D:\tools\libfsapfs-sdk`（`include\libfsapfs.h` + `lib\libfsapfs.a`，6.4 MB），
`nm` 检查确认库内同时包含 `libfsapfs_container_open_file_io_handle`、
`libbfio_handle_initialize`、`libbfio_handle_free` 三个符号。

配置与编译（在自建 msys64 环境里）：

```bash
SRC=/d/tmp/src/libfsapfs-20261030
BUILD=/d/tmp/build-fsapfs
CPPFLAGS="-I/d/tools/zlib/include" LDFLAGS="-L/d/tools/zlib/lib" \
  "$SRC/configure" --prefix=/d/tools/libfsapfs-sdk \
  --enable-wide-character-type --disable-dependency-tracking \
  --enable-static --disable-shared --without-openssl --disable-nls
cd "$BUILD" && make -j8 && make install
```

踩过的坑（按出现顺序，均已验证）：

1. **必须写 `/etc/fstab`**。自建 msys64 缺少 `etc/fstab` 时，盘符被映射成
   `/cygdrive/d`，configure/config.status 里记录的 `/d/...` 绝对路径全部失效，
   表现为 `cannot find input file: 'Makefile.in'`。写入一行即可：
   `none / cygdrive binary,posix=0,noacl,user 0 0`。
2. **补齐 msys 运行时 DLL**。工具包本身不带 `msys-*.dll`，DLL 在 `lib*` 包里：
   `gmp`、`mpfr`、`libreadline`、`zlib`、`libpcre`、`liblzma`、`libzstd`、`libbz2`、
   `gcc-libs`。缺 `msys-gmp-10.dll` 时 `expr.exe` 起不来，configure 会在
   `no acceptable grep could be found` 这类地方莫名失败（grep 依赖 pcre）。
3. **用 msys 的 `make`，不要用 `mingw32-make`**。后者是原生 Windows 程序，无法
   `stat` POSIX 路径 `/d/...`，会报
   `No rule to make target '/d/tmp/src/.../Makefile.am'`。
4. **删掉 release 包里预生成的 `common/config.h`**。tarball 里带着在 Unix 上生成的
   config.h（`#define HAVE_STRERROR_R 1`），而 mingw-w64 UCRT 不声明 `strerror_r`，
   于是 `libcerror_system.c` 编译报 implicit declaration；更隐蔽的是它排在
   `-I../common` 之后仍会被选中，单看 include 顺序找不出原因。
5. **阻止 automake 维护者规则**。若 `Makefile.in` 比 `aclocal.m4`/`configure` 旧，
   make 会调用 `automake-1.18`（本机只有 1.19）并失败。按依赖顺序刷新时间戳：
   `configure.ac` → `*.m4` → `aclocal.m4` → `configure` → `config.h.in` →
   所有 `Makefile.in`（最后一步）。
6. **静态库要自己补 zlib**。`.pc` 只把 `-lz` 写在 `Libs.private`，直接链接静态库会
   报 `undefined reference to 'uncompress'`；CMake 侧已在能力探测和链接命令中都加了
   `ZLIB::ZLIB`。
7. **`.pc` 的 prefix 要改成 Windows 路径**。安装出来是 `prefix=/d/tools/libfsapfs-sdk`，
   pkg-config 会给原生 g++ 传 POSIX 路径；改成 `D:/tools/libfsapfs-sdk` 或直接用
   `-DUIFE_LIBFSAPFS_ROOT=` 定位。

libyal 只安装自己的头文件，捆绑的 libbfio/libcerror 头不导出；而
`libfsapfs_container_open_file_io_handle` 又只在 `LIBFSAPFS_HAVE_BFIO`（链接外部
libbfio 时）才声明。UIFE 的做法是：在 `ApfsFsParser.cpp` 里用不透明类型自行声明
这些入口，并在 CMake 能力探测里一并做链接测试，缺符号就退回“仅识别”。

### libfsrefs（ReFS）

libfsrefs 没有 release，只有 git 快照，且仓库里不含 16 个捆绑依赖。本机已验证可行的
路线是"GitHub tarball + 替身 git"，全程几分钟（原 `synclibs.sh` 直接 `git clone`
在本机 20 分钟只完成 1/16）：

1. 逐个下载 `https://codeload.github.com/libyal/<lib>/tar.gz/refs/heads/main`，
   解压到 `D://tmp//libs//<lib>-main`。依赖列表见 `synclibs.sh` 的 `LOCAL_LIBS`
   （libbfio libcdata libcerror libcfile libclocale libcnotify libcpath libcsplit
   libcthreads libfcache libfdata libfdatetime libfguid libfusn libfwnt libuna）。
2. 放一个 `git` 替身到 PATH 最前：`clone` 时把 tarball 目录复制成目标目录并把
   内层 `<lib>-main` 改名为 `<lib>`；`fetch/tag/checkout` 直接返回成功。
   `tag` 输出为空会让 `synclibs.sh` 走 HEAD 分支，不再需要 tag 信息。
3. `sh ./synclibs.sh` 完成复制与 sed 变换。
4. 手工生成 configure（**不要用 autogen.sh**）：

   ```sh
   mkdir -p intl && echo "gettext-0.22.5" > intl/VERSION
   cp /path/to/libfsapfs-<版>/po/Makefile.in.in po/     # 本机 msys 没有 gettext 骨架
   cp /path/to/libfsapfs-<版>/config.rpath ./            # automake 要求，否则报
                                                         # required file './config.rpath' not found
   aclocal-1.19 --force --install -I m4                  # 默认 aclocal 是 1.9，不支持 --install
   libtoolize --force && autoheader --force && autoconf --force
   automake-1.19 --force --add-missing --copy
   ```

   `autogen.sh` 会走 `autoreconf -> autopoint`，而 autopoint 需要
   `/usr/share/gettext/archive.dir.tar.xz`（gettext 包里没有这个文件），必然失败。
5. `../configure --prefix=/d/tools/libfsrefs-sdk --enable-wide-character-type
   --enable-static --disable-shared --without-openssl --disable-nls`，然后 `make`。

`make` 最后会在 `po` 子目录停下（`No rule to make target '/config.status'`），因为
复制来的 gettext 骨架与本次 configure 的 `po/Makefile.in` 变量不匹配。翻译与库功能
无关，直接分目录安装即可：

```sh
make -C include install && make -C libfsrefs install
```

**能力限制（务必记录）**：

- 库里**没有**读取文件内容的接口，`libfsrefs_file_entry_get_size` 也只有声明没有实现
  （`nm` 里查不到），所以 UIFE 的 ReFS 后端只能列出目录与元数据，文件大小标为不可用。
- 该库只实现早期 ReFS 布局。用 Windows Server 2016 格式化的 1 GB ReFS 卷测试，
  `libfsrefs` 自带的 `fsrefsinfo` 与本项目的后端报同样的错：
  `invalid metadata block signature ... unable to read ministore tree`。
  即"适配层没问题，是库读不了现代 ReFS"。

### 没有 MSYS2 安装器时（例如大文件下载被代理阻断）

`repo.msys2.org` 通常可直连，逐个下载 `.pkg.tar.zst` 解压即可得到 autotools：

```text
msys2-runtime-3.6.9-2      # 必须与 bash 匹配，3.5.7 会 0xC0000139（入口点缺失）
bash-5.3.20-2
coreutils sed grep gawk findutils diffutils
m4 perl autoconf automake1.19 automake-wrapper libtool make
gzip xz zstd tar patch gettext libintl libiconv ncurses tzcode libxcrypt
```

解压到 `D:\tools\msys64` 后，注意 msys bash 的根目录会被 PATH 里的其他
`msys-2.0.dll`（例如 PortableGit）抢走，必须用干净环境启动：

```bash
env -i PATH="/d/tools/msys64/usr/bin" HOME=/d/tools/msys64/home \
    /d/tools/msys64/usr/bin/bash.exe -lc "autoconf --version"
```

## 构建开关

手工指定各依赖路径的方式（等价于 `scripts/fetch-deps.ps1` 生成的缓存文件内容，保留作参考）：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=D:/tools/mingw64/bin/g++.exe \
  -DZLIB_LIBRARY=D:/tools/zlib/lib/libzs.a -DZLIB_INCLUDE_DIR=D:/tools/zlib/include \
  -DLIBTSK_INCLUDE_DIR=D:/tools/tsk-4.15.0-ucrt64/include \
  -DLIBTSK_LIBRARY=D:/tools/tsk-4.15.0-ucrt64/lib/libtsk.a \
  -DLIBEWF_INCLUDE_DIR=D:/tools/libewf-sdk/include \
  -DLIBEWF_LIBRARY=D:/tools/libewf-sdk/lib/libewf.a \
  -DLIBWIM_INCLUDE_DIR=D:/tools/wimlib-sdk/include \
  -DLIBWIM_LIBRARY=D:/tools/wimlib-sdk/lib/libwim.dll.a \
  -DUIFE_LIBWIM_RUNTIME=D:/tools/wimlib-sdk/lib/libwim-15.dll \
  -DUIFE_LIBFSAPFS_ROOT=D:/tools/libfsapfs-sdk \
  -DUIFE_LIBFSREFS_ROOT=D:/tools/libfsrefs-sdk
```

`UIFE_LIBFSAPFS_ROOT` 指向 libfsapfs SDK 根目录；找不到时 APFS 保持“仅识别”。
修改探测相关代码后如遇缓存未刷新，追加 `-UUIFE_LIBFSAPFS_HAS_REQUIRED_API` 重跑 configure。

`uife info` 会打印"已启用文件系统解析器"，可直接确认哪些后端真的接上了。
