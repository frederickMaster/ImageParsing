# 第三方组件与许可证清单

UIFE 本体（`src/`、`include/`、`tests/`、`scripts/`）由本项目自行开发；下面列的是
**随二进制一起分发**的第三方组件。`deps/MANIFEST.md` 记的是这些组件的版本与
SHA-256，这份文件记的是它们的许可证与再分发义务——两者互补，缺一不可。

矩阵（2026-11-06）：

| 组件 | 版本 | 许可证 | 链接方式 | 再分发义务 |
| --- | --- | --- | --- | --- |
| zlib | 1.3.2.1（Chromium motley 分支） | Zlib | 静态（`libz.a`） | 保留版权与许可声明，不得声称是原版；不得用作者名义背书 |
| The Sleuth Kit | 4.15.0 | Common Public License 1.0（见 `tsk/libtsk.h`） | 静态（`libtsk.a`） | 附许可证全文；对库的修改需以源码形式提供；专利条款含防御性终止 |
| libewf | 20260924 | LGPL-3.0-or-later（另可选商业许可） | 静态（`libewf.a`） | 附许可证全文；**静态链接需让接收方能够重新链接**（见下） |
| libfsapfs | 20261030 | LGPL-3.0-or-later | 静态（`libfsapfs.a`） | 同上 |
| libfsrefs | 20261011 | LGPL-3.0-or-later | 静态（`libfsrefs.a`） | 同上 |
| wimlib | 1.14.5 | LGPL-3.0-or-later（另含 libdivsufsort-lite 的 MIT 声明） | 动态（`libwim-15.dll`） | 附 `COPYING.txt`、`COPYING.LGPL.txt`、`COPYING.GPLv3.txt`、`COPYING.libdivsufsort-lite.txt`；DLL 可替换即满足重新链接要求 |
| 开发期依赖 | MinGW-w64 16.2.0 / CMake / Ninja | 各自的自由软件许可 | 不随二进制分发 | 仅构建期使用，不进发布包 |

说明：

- 表里的许可证以**本地 SDK 的头文件声明**为准（`deps/sdk/*/include`、仓库根目录
  的 `libewf/*.h`）。上游仓库的 `COPYING` 是最终依据，升级组件时应重新核对一遍。
- 所有后端都是**可选**的：`-DUIFE_ENABLE_LIBTSK=OFF` 等开关可以关掉任意一个，
  关掉后该组件及其许可证义务一起消失。只发核心功能时只剩 zlib。
- Python 脚本（`tests/*.py`、`scripts/*.py`）只用标准库，不引入 pip 依赖。

## 已落实

- `scripts/package.ps1` 在打包时会从 `UIFE_LIBWIM_ROOT`（或 CMake 缓存里的
  `LIBWIM_INCLUDE_DIR`）定位 `COPYING*.txt` 并复制进 ZIP；缺失则打包直接失败，
  不允许"静默漏掉许可证"。
- 同一脚本把本仓库的 `docs/THIRD-PARTY-NOTICES.txt` 复制进 ZIP，静态组件的
  许可证与来源声明因此随包分发。

## 未落实（合规待办）

1. **LGPL 静态链接的重新链接权**：libewf / libfsapfs / libfsrefs 目前是 `.a` 静态
   链接，LGPL-3.0 第 4 条要求让接收方能够替换该库并重新链接。当前发布包只提供
   `uife.exe`，没有提供目标文件或等价机制，**这一项尚未满足**。可选的收尾方式：
   - 把这三个库改成 DLL 构建（与 wimlib 一致，最干净）；
   - 或随包提供 `uife` 的目标文件（`.o`/`.obj`）与链接脚本，让接收方能自行重链；
   - 或随包提供库源码归档（与发布包同一版本、同一编译器）。
2. **Sleuth Kit 的修改披露**：目前未修改其源码，因此只需附许可证全文；一旦打补丁，
   必须以源码形式公开该补丁。
3. **许可证全文的来源**：静态组件的 `COPYING` 不在本地 SDK 里，需要从上游获取后
   随包分发。`docs/THIRD-PARTY-NOTICES.txt` 先给出组件、版本、许可证标识与上游
   地址，全文补齐属于发布前动作（与上面第 1 条一起做）。

## 重新生成/核对

```sh
# 版本与来源（哪来的、哈希多少）
cat deps/MANIFEST.md

# 头文件里声明的许可证
head -25 deps/sdk/tsk/include/tsk/libtsk.h
head -25 deps/sdk/libewf/include/libewf.h
head -25 deps/sdk/libfsapfs/include/libfsapfs.h
head -25 deps/sdk/libfsrefs/include/libfsrefs.h
head -20 deps/sdk/zlib/include/zlib.h
```

升级任一组件后，同步更新本文件、`deps/MANIFEST.md` 与
`docs/THIRD-PARTY-NOTICES.txt` 三处。
