# 版本兼容表

这张表**不是手写的**，是从二进制自己申报的 `uife capabilities --json` 生成的：

```bash
python scripts/capability-matrix.py \
    --build "0.1.0 windows-x64=dist/uife-0.1.0-win64/uife.exe" \
    --build "0.1.0 linux-x64=dist/uife-0.1.0-linux64/uife" \
    --output docs/COMPATIBILITY.md
```

每发一个版本加一列，表与发行版不会脱节；`tests/capabilities_test.py` 在 CI 上
守住申报本身的自洽性（一行 JSON、字段齐备、"可读"与"仅识别"不重叠、退出码不变）。

## 为什么要机器生成

兼容表最容易腐烂的方式是"发版时漏改一行"。客户拿到表说支持 ReFS，装上的
二进制却没编进 libfsrefs，这类争议靠人复核心证只会越来越贵。改成从二进制读
之后，表里的每一格都能用同一条命令复核：

```bash
uife capabilities --json | python -m json.tool
```

`features` 里为 `false` 的项同样重要：`wim_named_streams` 目前是 `false`，
因为 wimlib 没有单流导出接口（见 `docs/PRD.md` 的验收项）。把它明写出来，
比留空或写"计划支持"更有用——使用者能直接判断要不要绕路。

## 字段含义

| 字段 | 含义 |
| --- | --- |
| `version` / `build` | 版本号、编译器、构建类型，均在建配置时固定，不含时间戳（可复现构建） |
| `image_formats.supported` | 已注册读取驱动的镜像容器格式 |
| `image_formats.recognized_only` | 能识别但当前构建没有读取驱动的格式（退出码 3） |
| `filesystems` | 已注册的文件系统解析器；容器可读不代表内部文件系统一定受支持 |
| `backends` | 第三方后端编译开关，与 `CMakeLists.txt` 的 `UIFE_WITH_*` 同源 |
| `features` / `feature_notes` | 能力开关与含义；`false` 表示这条路径没打通 |
| `exit_codes` | 退出码约定，与 README、`--help` 保持一致（由测试守住） |

## 当前构建

> 尚未发布版本。下表取自本地 Windows x64 Release 构建（MinGW），
> 用于验证生成链路；正式发布时按上面的命令重新生成。

| 项目 | 0.1.0 未发布 windows-x64 |
| --- | --- |
| 版本 | 0.1.0（Release） |
| 编译器 | GNU 16.2.0 |
| 镜像格式数 | 12 |
| 文件系统解析器数 | 10 |

### 镜像格式

| 格式 | 0.1.0 未发布 windows-x64 |
| --- | --- |
| QCOW2 | 有 |
| VMDK | 有 |
| VDI | 有 |
| QED | 有 |
| VHDX | 有 |
| EWF/E01 | 有 |
| WIM/ESD | 有 |
| ISO9660 | 有 |
| Apple DMG | 有 |
| Norton Ghost GHO (multi-partition) | 有 |
| BIN+CUE | 有 |
| RAW | 有 |

### 文件系统解析器

| 解析器 | 0.1.0 未发布 windows-x64 |
| --- | --- |
| Btrfs send stream | 有 |
| ISO9660 | 有 |
| WIM/ESD | 有 |
| ReFS | 有 |
| APFS | 有 |
| NTFS | 有 |
| FAT | 有 |
| exFAT | 有 |
| HFS+ | 有 |
| ext2/3/4 | 有 |

### 后端

| 后端 | 0.1.0 未发布 windows-x64 |
| --- | --- |
| raw | 是 |
| zlib | 是 |
| ewf | 是 |
| wim | 是 |
| apfs | 是 |
| refs | 是 |
| tsk | 是 |

### 能力

| 能力 | 0.1.0 未发布 windows-x64 |
| --- | --- |
| alternate_streams | 是 |
| sparse_files | 是 |
| acl_restore | 是 |
| reparse_points | 是 |
| long_paths | 是 |
| resource_limits | 是 |
| operation_log | 是 |
| encrypted_apfs | 是 |
| wim_named_streams | 否 |

### 能力说明

| 能力 | 含义 |
| --- | --- |
| alternate_streams | NTFS 备用数据流导出为 <文件>__ads__<转义流名> 侧车文件 |
| sparse_files | 导出时保留稀疏区，不把空洞写成实数据 |
| acl_restore | extract/restore 支持 --restore-acl |
| reparse_points | 目录联接与符号链接按目标重建 |
| long_paths | 导出路径超过 MAX_PATH 仍可写入（导出根走扩展长度路径） |
| resource_limits | --max-entries / --time-limit，超限以退出码 5 中止 |
| operation_log | --log-format text\|json 与 --log-file |
| encrypted_apfs | --password-stdin 解锁加密 APFS 卷 |
| wim_named_streams | wimlib 没有单流导出 API，WIM 内的命名流暂不可导出 |

## 已知的平台差异

| 能力 | Windows | Linux/macOS | 说明 |
| --- | --- | --- | --- |
| `acl_restore` | 是 | 否 | 还原 NTFS DACL 依赖 Win32 安全 API；其它平台只做后备元数据报告 |
| `reparse_points` | 是 | 否 | 目录联接/符号链接还原走 `FSCTL_SET_REPARSE_POINT` |
| `long_paths` | 是 | 是 | Windows 需要扩展长度路径前缀；POSIX 本身没有 260 字符上限 |

平台差异由编译期宏决定，因此同一版本在不同平台的 `capabilities` 输出会不同——
这正是表要分列的原因。
