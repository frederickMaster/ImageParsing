# 可复现构建

闭源交付时，用户拿到的二进制与"审计过的源码"之间需要一条可验证的关系。
可复现构建就是这条关系：**同一份源码、同一套依赖、同一套参数，两次独立
构建必须产出逐字节相同的文件**。做不到的话，二进制哈希就不能作为任何证明。

## 结论

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| `uife.exe`（Windows/MinGW） | 可复现 | 两个独立构建目录产出相同 SHA-256，已实测 |
| `uife`（Linux/macOS） | 预期可复现 | 同上机制；CI 未每次都验证（见下文"为什么不做成常态任务"） |
| 依赖包（`deps/*.zip`） | 不可复现 | 由第三方源码与打包机状态决定，只能靠 SHA-256 钉住 |
| 发布 ZIP | 不可复现 | `Compress-Archive` 写入本地时间戳与目录枚举顺序 |

## 机制

1. `-ffile-prefix-map=<源码目录>=.`、`-ffile-prefix-map=<构建目录>=build`
   （GCC/Clang）。把嵌进二进制的绝对路径折叠成相对路径，换机器、换目录名
   都不影响字节。
2. MSVC 用 `/Brepro`，去掉链接器写入的时间戳与 PDB 路径。
3. `-Wdate-time` 把 `__DATE__` / `__TIME__` 变成编译期告警——它们是另一种
   常见的"看起来一样、字节不同"的来源。当前代码没有用到。
4. `scripts/repro-check.ps1` 把 `SOURCE_DATE_EPOCH` 固定下来，在两个空目录里
   各构建一次并比对 SHA-256。

需要说明的是：Release 构建本来就不带调试信息，代码里也没有 `__FILE__`
字面量，所以**即使不加这些参数，当前二进制也可能恰好一致**。这些开关防的是
Debug 构建、断言里字符串化路径、以及以后新增的代码——它们保证"以后也不会
退化成不可复现"，而不是修复了一个已经发作的缺陷。

## 怎么跑

```powershell
# 依赖路径随机器而异，用 -CMakeArguments 透传；两次构建拿到的是同一套参数
pwsh -File scripts/repro-check.ps1 `
  -CMakeExecutable D:/tools/cmake/bin/cmake.exe `
  -CMakeArguments @(
    "-DCMAKE_CXX_COMPILER=D:/tools/mingw64/bin/g++.exe",
    "-DZLIB_INCLUDE_DIR=D:/tools/zlib/include",
    "-DZLIB_LIBRARY=D:/tools/zlib/lib/libzs.a",
    "-DLIBWIM_INCLUDE_DIR=D:/tools/wimlib-sdk/include",
    "-DLIBWIM_LIBRARY=D:/tools/wimlib-sdk/lib/libwim.dll.a"
  )
```

退出码 0 表示两次构建一致，1 表示不一致（脚本会打印两个哈希与常见原因），
2 表示环境缺失。

## 为什么不做成每次提交的 CI 任务

一次检查要完整构建两遍，把 CI 时间翻倍，而收益只在发布时体现。释放流程里
在打包前跑一次即可；如果以后要放进 CI，放在 `workflow_dispatch` 或发布
流水线里，不要挂在每次 push 上。

## 已知的非确定来源

- **依赖包**：`deps/UIFE-deps-*.zip` 里是第三方 SDK，重新打包会得到不同字节。
  发布说明里同时给出源码 commit 与依赖包 SHA-256，两者一起才构成完整坐标。
- **发布 ZIP**：`scripts/package.ps1` 用 `Compress-Archive`，条目时间戳来自
  文件系统的最后修改时间。要让它也可复现，需要改成按排序写入并固定时间戳的
  归档器，目前没做——发布物的重要性等级里，二进制 > 依赖包 > 外层压缩包。
