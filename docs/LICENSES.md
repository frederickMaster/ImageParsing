# 第三方库的授权要求（逐库）

这份文件回答一个问题：**随包分发的每一个第三方组件，法律上要求我们做什么，我们做到了没有。**
版本与 SHA-256 在 `deps/MANIFEST.md`，随包分发的声明在 `docs/THIRD-PARTY-NOTICES.txt`，
许可证全文在 `docs/licenses/`，商业许可条款草稿在 `docs/EULA.md`。

> 工程侧整理，不是法律意见。对外措辞请法务过一遍再定稿。

## 0. 本项目自己的授权

`uife` 本体（`src/`、`include/`、`tests/`、`scripts/`）是**闭源商业产品**，仓库保持私有，
不对外分发源码。将来如果要开一条社区版线，另走 `LICENSE` 与 CLA，**不影响**本文的结论：
LGPL 只约束"库"本身，不传染应用代码，"闭源 + 商用"在 LGPL 下是合法组合。

## 1. 分发形态

商业版随包分发的是：

```
uife.exe
libewf-3.dll  libbfio-1.dll  libfsapfs-1.dll  libfsrefs-1.dll
libwim-15.dll  libwinpthread-1.dll
licenses/  THIRD-PARTY-NOTICES.txt
```

`libtsk` 与 `zlib` 静态链进 `uife.exe`。四个 libyal 库是**独立可替换的 DLL**，
依据 LGPL-3.0 §4(d)(1)，不需要目标文件、不需要库源码、也不需要公开应用代码。

仓库里仍保留一条静态链接的构建路径（`build-local-full`，全部后端 `.a`），
它对应社区/内部版本；走那条路时适用的是 §4(d)(0)（源码公开 + 可重链），
与商业版互不影响。**不要把静态构建的产物当成商业版发布**。

静态路径若要对外分发，重链能力必须能被证明而不是只写在文档里：
`scripts/relink-check.sh` 会分别用「改过的 `libewf.a`」验证能链上、用「损坏的
`libewf.a`」验证链接确实失败（否则说明那个库根本没参与链接），再恢复原库并跑一次
smoke test。商业版不需要这一步——DLL 替换本身就是重链。

| 组件 | 版本 | 许可证 | 链接方式 | 义务 |
| --- | --- | --- | --- | --- |
| zlib | 1.3.2.1（Chromium motley 分支） | Zlib | 静态进 exe | 仅声明义务，已满足 |
| The Sleuth Kit | 4.15.0 | CPL-1.0 | 静态进 exe | 许可证全文 + 源码可得声明 + 免责条款 |
| libewf | 20260924 | LGPL-3.0+（另有商业许可） | DLL | §4(a)(b)(c) + 4(d)(1) |
| libbfio | 20260623 | LGPL-3.0+ | DLL | 同上 |
| libfsapfs | 20261030 | LGPL-3.0+（另有商业许可） | DLL | 同上 |
| libfsrefs | 20261011 | LGPL-3.0+ | DLL | 同上 |
| wimlib | 1.14.5 | GPLv3+ 或 LGPL-**2.1**+（二选一）／内含 MIT 的 libdivsufsort-lite | DLL | LGPL-2.1 §6（可替换 DLL）+ 声明义务 |
| mingw-w64 runtime | gcc 16.2.0 配套 | MIT/BSD 类 | DLL | 仅声明义务 |

## 2. libyal 家族的"隐藏组件"

四个 libyal DLL 各自把同作者的辅助库静态编了进去。四个包的并集是 **23 个辅助库**：

```
libcaes libcdata libcdatetime libcerror libcfile libclocale libcnotify libcpath
libcsplit libcthreads libfcache libfdata libfdatetime libfguid libfmos libfvalue
libfwnt libfusn libhmac libodraw libsmdev libsmraw libuna
```

它们都是 LGPL-3.0+，都有各自的版权人。所以 LGPL 组件一共 **27 个**（4 个主库 + 23 个
辅助库），只写"libewf, LGPL"是不合格的。`docs/THIRD-PARTY-NOTICES.txt` 已按这个口径写，
并把辅助库清单挂在对应的源码归档下面。

## 3. LGPL-3.0 逐条对照

| 条款 | 要求 | 现状 |
| --- | --- | --- |
| §4 前言 | 组合作品的条款不得限制"修改库"和"为调试修改而逆向" | `docs/EULA.md` §4 有明确例外，**已写** |
| §4(a) | 每份副本显著声明"使用了该库且受 LGPL 约束" | `THIRD-PARTY-NOTICES.txt` 第 3–6 条 + 独立声明段，**已写** |
| §4(b) | 随附 LGPL-3.0 与 GPL-3.0 全文 | `docs/licenses/LGPL-3.0.txt`、`GPL-3.0.txt`，`package.ps1` 缺失即失败，**已落地** |
| §4(c) | 程序若显示版权信息，需列出库的版权并指向许可全文 | `uife about` 输出版权人、许可证与 licenses/ 下的全文文件名（2026-11-06 落地），**已写** |
| §4(d)(1) | 用共享库机制，换成接口兼容的改版能正常工作 | 四个 libyal 全部 DLL，**已满足**（wimlib 是 LGPL-2.1，对应 §6，见下） |
| §4(e) | 仅在 GPLv3 §6 的 "User Product" 场景（随硬件出货）才要 Installation Information | 纯软件交付，不触发；**随硬件出货前需重新评估** |

§4(d)(1) 的落地细节：DLL 与 `uife.exe` 同目录，Windows 的加载顺序优先取应用目录，
所以把任意一个 DLL 换成自己编译的版本即生效，不需要重新链接、不需要目标文件。

### wimlib 走的是 LGPL-2.1，不是 3.0

这一点以前写错了，2026-11-06 按上游原文更正。wimlib 1.14.5 的 `COPYING` 说得很明确：
整个 wimlib 是 **GPLv3+**；而 **libwim（库部分）在"第三方许可不禁止"的前提下可以按
LGPL-2.1+ 分发**，二选一。我们按 LGPL-2.1+ 走，对应义务是 §6 而不是 §4：

| 条款 | 要求 | 现状 |
| --- | --- | --- |
| §6(a) | 显著声明使用了库、注明版权并附许可全文 | `THIRD-PARTY-NOTICES.txt` 第 7 条 + `docs/licenses/wimlib-COPYING.LGPL.txt`，**已落地** |
| §6(b)(c)(d) | 允许用户替换库版本，并提供必要信息 | `libwim-15.dll` 与 `uife.exe` 同目录，按接口兼容替换即生效，**已满足** |
| §6(e) | 程序若显示版权信息，需一并声明库的版权 | 同 §4(c)：`uife about` 已列出 wimlib 与 libdivsufsort-lite，**已写** |

前提是"第三方许可不禁止"：若某次 wimlib 打包链上了 GPL 的 libntfs-3g，LGPL 选项就失效，
只剩 GPLv3 一条路，那时必须换构建或取商业授权。当前这份 `libwim-15.dll` 只依赖系统 DLL
（ADVAPI32 / KERNEL32 / USER32 / ntdll + UCRT），**没有 libntfs-3g**，所以 LGPL 成立。
**换 wimlib 构建后要复查这一条**（`objdump -p libwim-15.dll | grep "DLL Name"`）。

libdivsufsort-lite 是 wimlib 内部带的 MIT 代码，上游没有单独的许可文件，通知里那句话
（"随附 COPYING.libdivsufsort-lite.txt"）与事实不符。现在改成：MIT 正文按源码文件头
逐字收录在 `docs/licenses/MIT-libdivsufsort-lite.txt`，并注明出处与来源包的 SHA256。

## 4. CPL-1.0（The Sleuth Kit）

CPL 是 IBM 的弱传染许可：

1. 随附许可证全文 → `docs/licenses/CPL-1.0.txt`；
2. 目标码分发时，分发者须在**自己的许可协议**里声明"Program 源码可从分发者处获得并说明
   获取方式"→ `docs/EULA.md` §5 给出上游地址与版本；
3. 只有修改 TSK 本身才需公开那部分修改 → 我们未改，不触发；
4. 专利条款带防御性终止；
5. 分发目标码的许可协议里需含免责与责任限制 → `docs/EULA.md` §6。

## 5. Zlib

只要求：不歪曲来源、修改版需标注、不得删除声明、只发二进制时声明要出现在随附材料里。
`THIRD-PARTY-NOTICES.txt` 第 1 条已满足。

## 6. 构建侧为什么要多一个 `libbfio-1.dll`

libyal 的静态归档把 `libbfio` 打包进去却不安装它的头文件，而 `ApfsFsParser.cpp` 需要按
handle 打开 APFS，直接引用 `libbfio_handle_initialize` / `libbfio_handle_free`。
一旦把 `libfsapfs` 改成 DLL，libtool 的导出规则（`^libfsapfs_`）不会导出这两个符号，
链接就会失败。

正确解法不是给库打补丁（那会触发"公开对库的修改"的义务），而是按上游设计用
`--with-libbfio=<prefix>` 把 `libbfio` 作为**独立的共享库**编出来：这样
`LIBFSAPFS_HAVE_BFIO` 打开，两个符号由 `libbfio-1.dll` 自己导出。**没有任何第三方源码
被修改，因此不存在补丁公开义务。**

## 7. 仍然要做的事

1. ~~**wimlib 的 MIT 全文**~~ —— **已完成（2026-11-06）**。上游 wimlib 1.14.5 根本
   没有 `COPYING.libdivsufsort-lite.txt`（那句"随附该文件"是照抄假设写下的），
   libdivsufsort-lite 的 MIT 声明只在 `src/divsufsort.c` 的文件头里。现在按原文逐字
   收录为 `docs/licenses/MIT-libdivsufsort-lite.txt`，wimlib 自己的三份 COPYING 也
   一并入库（`wimlib-COPYING{,.LGPL,.GPLv3}.txt`），`package.ps1` 直接从仓库取，
   不再去 SDK 目录里找。顺带更正了 wimlib 的许可版本：libwim 是 **LGPL-2.1+**。
2. ~~**§4(c) / §6(e)**~~ —— **已完成（2026-11-06）**：`uife about` 列出组件、许可证、
   版权人与许可全文文件名，`tests/capabilities_test.py` 守着"启用的后端必须有声明"。
   将来加 GUI 时，把 `about` 的输出原样放进"关于"即可，不要另抄一份表。
3. **随硬件出货**：触发 §4(e)，需要提供 Installation Information（或改买商业授权）。
4. **EULA 定稿**：`docs/EULA.md` 是模板，正式对外前过法务（2026-11-06 已按 LGPL-2.1
   更正 wimlib 那一段，其余条款未动）。
