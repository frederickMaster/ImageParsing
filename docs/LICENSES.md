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
| wimlib | 1.14.5 | LGPL-3.0+ / MIT（libdivsufsort-lite） | DLL | §4(a)(b) + 4(d)(1) |
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
| §4(c) | 程序若显示版权信息，需列出库的版权并指向许可全文 | 当前 CLI 无"关于"界面；将来加 GUI 时必须在"关于"里补这一段，**待办** |
| §4(d)(1) | 用共享库机制，换成接口兼容的改版能正常工作 | 四个 libyal + wimlib 全部 DLL，**已满足** |
| §4(e) | 仅在 GPLv3 §6 的 "User Product" 场景（随硬件出货）才要 Installation Information | 纯软件交付，不触发；**随硬件出货前需重新评估** |

§4(d)(1) 的落地细节：DLL 与 `uife.exe` 同目录，Windows 的加载顺序优先取应用目录，
所以把任意一个 DLL 换成自己编译的版本即生效，不需要重新链接、不需要目标文件。

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

1. **wimlib 的 MIT 全文**：`COPYING.libdivsufsort-lite.txt` 需要从 wimlib 发布包取回并
   放进 `docs/licenses/`（当前依赖打包机上有 wimlib 源码，`package.ps1` 会从
   `UIFE_LIBWIM_ROOT` 复制；在拿到源码之前这一项算未闭环）。
2. **§4(c)**：加 GUI 或"关于"输出时，必须列出 libyal/wimlib 的版权声明与许可全文位置。
3. **随硬件出货**：触发 §4(e)，需要提供 Installation Information（或改买商业授权）。
4. **EULA 定稿**：`docs/EULA.md` 是模板，正式对外前过法务。
