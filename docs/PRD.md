# UIFE 产品需求文档（PRD）

- **产品名称**：UIFE — Universal Image Forensics Engine
- **文档版本**：0.1
- **状态**：持续开发；本文同时记录目标需求和当前实现状态
- **最后更新**：2026-11-03
- **产品形态**：跨平台命令行工具（CLI）；不包含 GUI

## 1. 产品概述

UIFE 面向电子数据取证、IT 运维和嵌入式开发人员，目标是无需挂载、以只读方式检查磁盘、文件系统、虚拟化及取证镜像，并支持浏览和提取镜像中的文件。

UIFE 当前是一个持续开发中的 MVP，不是对所有格式变体均兼容的成品，也未经过取证认证。本文中的“目标”是产品需求，不代表相应能力已经交付。格式识别、格式容器读取、文件系统解析和完整提取是不同能力，不能相互等同。

### 1.1 产品目标

1. 提供统一的镜像识别、只读访问、分区/文件系统探测、文件浏览和提取 CLI。
2. 复用成熟开源解析库，避免重复实现已有协议解析能力。
3. 对不支持或无法验证的格式变体明确失败，不使用静默降级或错误数据继续处理。
4. 逐步扩大格式覆盖，并以可复现测试、源镜像哈希检查和真实样本验证质量。

### 1.2 非目标与约束

- 当前版本只提供 CLI；不开发 Qt GUI。
- 不挂载镜像，不调用系统文件系统驱动解析镜像内容。
- 不以修改原始镜像、修复镜像或重放虚拟磁盘日志作为解析方式。
- 不宣称当前版本已达到 99% 识别率、100 MB/s 吞吐、10 GB 镜像 10 秒加载或取证认证标准。

## 2. 用户与使用场景

| 用户 | 主要目标 |
|---|---|
| 电子数据取证人员 | 在不改写证据镜像的前提下识别格式、浏览文件并导出目标文件；保留可获得的元数据并校验导出结果。 |
| IT 运维人员 | 离线检查系统备份、虚拟磁盘和安装镜像，定位并提取配置或日志文件。 |
| 嵌入式开发人员 | 检查 RAW/IMG、ISO、BIN+CUE 等设备或介质镜像中的文件系统内容。 |

### 2.1 典型流程

1. 使用 `uife info <image>` 检查镜像格式、几何信息、分区和可用解析能力。
2. 使用 `uife list <image> [VFS-path]` 浏览根目录或指定路径。
3. 使用 `uife extract <image> <VFS-path> -o <directory>` 提取文件/目录。
4. 使用 `uife restore <image> -o <directory>` 提取所选文件系统的可访问内容。
5. 对重要证据由用户在 UIFE 之外记录源镜像哈希、工具版本及操作日志。

## 3. 功能需求及当前状态

状态说明：

- **已实现（受限）**：代码支持明确列出的变体；不表示格式全覆盖。
- **条件支持**：依赖可选库、特定构建 API 或平台工具。
- **仅识别**：可以报告格式线索，但不能浏览其文件内容。
- **未实现**：当前没有相应解析能力。
- **未验证**：代码或后端存在，但缺少目标样本或平台验证。

### 3.1 镜像格式

| 格式/需求 | 目标行为 | 当前状态 |
|---|---|---|
| RAW、IMG | 只读随机访问；扫描分区并探测文件系统 | RAW/IMG 只读访问已实现。文件系统浏览取决于对应解析器；没有文件系统的裸数据不提供文件树。 |
| ISO | 只读浏览 ISO9660/Joliet/Rock Ridge 内容 | ISO9660、Joliet、部分 Rock Ridge 已实现；不支持的扩展明确拒绝。 |
| QCOW2 | 离线读取虚拟磁盘数据 | 仅支持 standalone v2/v3、无加密/父盘/快照/特性标志的受限变体；不支持压缩簇。 |
| VMDK | 离线读取虚拟磁盘数据 | 支持单文件未压缩 sparse v1，以及受限 descriptor/FLAT extent；不支持快照和压缩 sparse grains。 |
| VDI | 离线读取虚拟磁盘数据 | 支持 VDI 1.1 dynamic/fixed；不支持差分盘和 block-extra。 |
| QED | 离线读取虚拟磁盘数据 | 支持无 backing file、无 feature flags 的 standalone 镜像。 |
| VHDX | 离线读取虚拟磁盘数据 | 支持 clean-log、standalone dynamic/fixed；不重放日志、不解析父盘，不支持部分 present block。 |
| WIM/ESD | 浏览镜像索引并提取镜像文件 | 条件支持 wimlib；按 `/images/N` 浏览；`.swm` 从首卷打开并引用同目录编号分卷。加密 ESD、部分 Windows 元数据未支持。 |
| GHO | 读取 Ghost 分区镜像及关联分卷 | 支持 1–4 个未加密分区、限定压缩类型；自动发现匹配 `.ghs`。加密和不支持的压缩拒绝处理。 |
| E01/EWF | 读取多段取证镜像并自动使用关联分段 | 条件支持 libewf；已有合成两段样本验证，真实样本兼容矩阵未完成。 |
| BIN+CUE | 读取关联 BIN 数据轨道 | 支持同目录 BINARY 与受限 MODE1 轨道；不支持音轨、MODE2、混合格式等情况。 |
| DMG | 只读解析 Apple UDIF 容器 | 支持单段 UDIF，XML/binary plist blkx 中 raw、zero/ignore、zlib runs；其他压缩和加密变体拒绝。 |
| APFS `.dmg` | 解析 APFS 容器文件系统 | DMG 容器驱动与 APFS 文件系统解析是不同层；APFS 依赖兼容 libtsk APFS pool API。当前构建能力受限，真实加密样本尚未验证；Fusion 未支持。 |
| XFS | 浏览 XFS 元数据/文件树 | 部分 superblock 可检测；是否能浏览取决于具备 XFS API 的 libtsk。当前本地 SDK 不支持浏览。 |
| ReFS / VHDX | 浏览 VHDX 中的 ReFS 文件系统 | 当前仅能识别 ReFS 签名；没有 ReFS 文件树解析能力。 |
| Btrfs send | 从 send stream 重建文件树并读取文件 | 内置解析器支持独立 v1-v3 流及已实现命令；增量快照、clone、encoded write 等未支持。send stream 不是完整 Btrfs 块设备镜像。 |

### 3.2 通用功能

| ID | 需求 | 当前状态 |
|---|---|---|
| FR-01 | 自动识别受支持及已知但不支持的镜像格式，并报告能力状态 | 驱动注册表提供识别/支持状态；识别成功率尚无代表性语料统计。 |
| FR-02 | 展示分区及可识别文件系统信息 | MBR/GPT 扫描已实现；文件系统识别和浏览取决于后端与构建配置。 |
| FR-03 | 浏览目录、查询文件元数据、读取文件内容 | 由对应文件系统解析器提供；未实现解析器的格式不能展示完整文件树。 |
| FR-04 | 提取选中的文件/目录 | 已实现基本提取；受解析器提供的信息、平台权限和文件系统语义限制。 |
| FR-05 | 保留源文件可获得的权限、时间戳、扩展属性及链接信息 | 部分支持：libtsk 后端已导出命名数据流（侧车文件 + SHA-256 校验）、报告文件属性标志（稀疏/压缩/加密/重解析点）、`security_id`、重解析目标，以及未压缩/未加密默认数据流的分配映射（含稀疏洞），并覆盖所有能给出 run 链的后端（NTFS、ext2/3/4 的 extent 树、UFS/FFS）；WIM 目录项报告完整 `FILE_ATTRIBUTE_*` 字与安全描述符（owner/group SID + DACL/SACL ACE）。导出时已重建语义：重解析点重建为真实链接（目录联接 `FSCTL_SET_REPARSE_POINT`、符号链接 `CreateSymbolicLinkW`、POSIX `symlink`），稀疏文件按 extents 写成稀疏文件且逻辑内容哈希不变，POSIX 扩展属性在 chmod 之后回写，`--restore-acl` 时把 DACL 逐条重建（`SE_DACL_PROTECTED`）并按需启用 `SeRestorePrivilege` 还原所有者/组；无法忠实还原的属性写入 `uife-metadata.json` 而不是静默丢弃。SID→账户名解析仍未实现，CLI Sleuth Kit 后备路径仍只有名称/大小/UID-GID，跨格式完整性目标未达成。 |
| FR-06 | 对导出常规文件进行哈希校验 | 当前提取器实现 SHA-256 导出后校验；覆盖范围受可读取文件内容限制。 |
| FR-07 | 接受正确密码后读取加密 APFS/WIM；错误密码给出明确错误 | APFS 仅在后端有对应 API 时尝试解密，真实样本验证未完成；加密 ESD 未实现。 |
| FR-08 | 自动发现并关联多卷/多段镜像 | 已支持 EWF、GHO spans、split WIM 的限定命名/布局；不保证任意厂商命名规则。BIN+CUE 通过描述文件关联 BIN。 |
| FR-09 | 默认只读，不写入原始镜像 | 设计要求；读写 API、临时文件与输出路径须分开。临时 staging 仅用于解析/提取流程，不应修改源镜像。仍需对所有驱动执行源哈希回归验证。 |
| FR-10 | CLI 支持单文件及目录批量自动化 | 已实现 `info`、`inspect`、`probe`、`list`、`extract`、`restore`、`bench` 与 `batch` 清单批处理（逐行执行、失败不中断、`--json` 输出 JSONL、统一退出码）。 |

## 4. 非功能需求与验收标准

| 类别 | 目标/验收标准 | 当前结论 |
|---|---|---|
| 只读安全 | 打开、识别、浏览、读取前后源镜像哈希相同；异常输入不造成源文件改写 | 合成 fixture 的自动清扫（`tests/test_support.h`）对每张镜像执行打开、读扇区、扫分区、挂载、列目录、读文件、整卷导出，再比对整个工作目录的 SHA-256 快照；同一套清扫还会把源文件置为只读后重跑一遍，用权限位证明"想写也写不进去"；真实语料由 `tests/compat_matrix.py` 逐用例前后拍指纹（清单+大小+mtime+SHA-256），30 个样本零改动；`tests/readonly_audit.py` 静态审计源码中不存在写方式打开源镜像的路径。截断输入检查把镜像截到 0/512/一半/末尾前一字节，要求明确拒绝或读操作被钳制。剩余风险：以 root 运行时权限位失效（有显式告警）；驱动在镜像旁新建临时文件这类写入仍只能靠快照比对发现。 |
| 提取正确性 | 导出文件 SHA-256 与镜像内对应逻辑文件数据一致 | 提取器有导出后 SHA-256 校验；需扩展真实样本和所有文件系统测试。 |
| 多卷完整性 | 从首卷自动加载完整集合；缺卷/错误卷明确失败且无内容静默缺失 | EWF/GHO/WIM 有部分实现和合成测试；真实命名与损坏组合尚未全覆盖。 |
| 元数据 | 保留权限、时间戳、扩展属性及格式相关元数据 | 部分实现，完整性目标未达成。 |
| 兼容率 | 对声明支持的格式识别成功率 ≥99% | 未测量；需定义样本集、版本矩阵和统计方法后验收。 |
| 性能 | 单镜像解析速度 ≥100 MB/s；10 GB 以内镜像加载 ≤10 秒 | 已实测（`docs/PERFORMANCE.md`）：EWF 两段 4.87 GB 介质读到 468 MiB/s，满足 100 MB/s；但 10 GB/10 秒等价于 1 GB/s，实测未达到（外推约 22 秒）。上述数字是页缓存命中状态，冷缓存未测。目录列举复杂度为 O(条目数)，缺大目录样本，没有实测曲线。资源上限（`--max-entries`/`--time-limit`）保证超大规模输入以退出码 5 明确失败。 |
| 跨平台 | Windows/macOS/Linux 可构建并运行 | C++/CMake 设计为跨平台；当前只完成有限平台验证，三平台验收未完成。 |
| 稳定性 | 对畸形/截断镜像进行边界检查并明确失败，不越界读取或产生错误成功结果 | 集成测试内已有 639 例确定性畸形变异；`uife_fuzz` 每次提交再跑 120 例语料变异（固定 seed，含"保留头部"算子，覆盖挂载与读文件），ASan 任务里带 sanitizer 再跑一遍；夜间 `fuzz-long` 用 ASan 构建跑 20 分钟。超时、崩溃输入自动留档并可 `--replay`。测试侧每个用例另有独立看门狗（默认 120 秒，可用 `UIFE_TEST_TIMEOUT_SECONDS` 覆盖）：超预算即点名该用例并以退出码 4 结束，配套自检用例守着"能发现挂死"这条能力本身。局限：不是覆盖率引导，语料决定可达代码（见 `docs/FUZZING.md`）。 |
| 开源和依赖 | C++20、CMake、可选成熟开源依赖；提供依赖说明、测试及打包方法 | 基础工程与 README/打包脚本已有；仍需补齐完整平台构建说明及依赖许可证审查。 |

## 5. CLI 需求

当前命令形态：

```text
uife --help
uife info <image>
uife inspect <image>
uife probe <image>
uife list <image> [VFS-path] [--partition N]
uife extract <image> <VFS-path> -o <output-directory> [--partition N]
uife restore <image> -o <output-directory> [--partition N]
uife bench <image> [--size <bytes>]
uife batch <manifest-file>
uife capabilities [--json]   # 本构建的格式/解析器/后端/能力，JSON 供脚本采集
```

`list`/`extract`/`restore` 另接受资源预算：`--max-entries <N>` 限制节点数（list 按单目录条目计，提取按整棵目录树计），`--time-limit <秒>` 限制墙钟时间。两者默认关闭，超限以退出码 5 中止——遍历阶段就停，单文件复制也会在分块之间被打断，不会把进程拖进 OOM 或无限等待。

当前另有 APFS 密码通过 `--password-stdin` 从标准输入传递的实现路径。密码不应作为明文命令行参数传入。

CLI 产品化（2026-11-04 已实现）：统一 JSON 输出模式（`--json`，含 JSON 错误对象）、批处理清单（`batch`，JSONL 结果）、退出码/错误码表（0/1/2/3/4，见 README）、导出清单（`--manifest`，路径相对导出目录，可直接 `sha256sum -c`）、逐位置能力探测（`probe`）与顺序读取吞吐基准（`bench`）；进度信息固定输出到 stderr，JSON 模式下关闭。`--log-format text|json` + `--log-file` 提供操作日志：UTC 毫秒时间戳、固定事件名（`open`/`integrity`/`list`/`extract_start`/`extract_done`/`command_failed`），默认关闭，写到 stderr 时自动关掉进度条以免互相覆盖；JSON 格式每行一个对象，可直接与 `batch` 的 JSONL 结果拼成时间线。

## 6. 主要待办与依赖

1. **定义兼容性测试集**：逐格式收集合法、变体、损坏、加密及多卷样本；记录来源、许可证/使用权限、哈希和预期解析结果。目录约定与断言脚本已就绪（`tests/corpus/README.md`、`tests/compat_matrix.py`），缺样本。
2. **补齐文件系统后端**：评估可维护的用户态 ReFS、XFS、APFS 解析库；核实许可证、平台支持、只读接口和长期维护情况。
3. **加密支持**：先明确 APFS/WIM 的算法、依赖 API、密钥输入与内存清理策略，再用真实加密/错误密码样本验收。
4. **元数据模型**：定义跨平台元数据映射与无法恢复时的显式报告。已完成 NTFS ADS 导出、文件属性标志、security_id、重解析目标与 `$Secure:$SDS` 安全描述符解析（owner/group/DACL/SACL，经 `--json` 输出）；分配映射已从 NTFS 扩展到 ext2/3/4（含未初始化 extent 表示的空洞）与 UFS/FFS，WIM 目录项的安全描述符与文件属性也已解码。已实现 ACL 重建到导出文件（`--restore-acl`）、reparse point 原生重建、稀疏文件稀疏写盘、POSIX 扩展属性回写，以及后备元数据报告（`uife-metadata.json`）；待完成 SID→账户名解析与 CLI Sleuth Kit 后备路径的时间戳/权限位。
5. **只读证明**：现在是三层证据，而不是一条约定。(a) 合成 fixture 的自动清扫（`tests/test_support.h`：打开→读扇区→扫分区→挂载→列目录→读文件→整卷导出，操作前后比对目录树字节快照）。(b) 同一套清扫在**源文件被置为只读**后再跑一遍（Windows 只读属性 / POSIX `0444`），任何"按可写方式打开"都会当场 `ACCESS_DENIED`；集成测试中 17 张镜像、17 份导出全部通过。(c) 真实语料由 `tests/compat_matrix.py` 在每个用例前后拍指纹（文件清单+大小+mtime+SHA-256），30 个外部样本零改动，并系统性检查了截断镜像与多分卷缺卷。另有 `tests/readonly_audit.py` 静态审计（镜像容器层不得出现写方式打开、libewf 不得用写标志、libtsk 只允许 `tsk_img_open_external`），已作为 CI 第一步运行；所有合法写出点必须带 `readonly-audit:` 标注说明用途。仍待完成：以 root 运行时权限位失效，脚本会显式告警但无法阻止写入，需要在容器内以非 root 跑。
6. **跨平台 CI 与性能**：`.github/workflows/ci.yml` 现有六个任务（Linux 核心、Linux 真后端、macOS、Windows/MSVC、Windows/MinGW 五后端、Linux ASan+UBSan）全部通过，并对后端开关做断言；`uife bench` 与 `tests/compat_matrix.py` 已就绪；`docs/PERFORMANCE.md` 记录了实测吞吐（含硬件与缓存前提）与未达标的验收口径。待完成：冷缓存测量与大目录样本。
7. **安全与稳定性**：已实现结构化畸形输入清扫（`tests/test_support.h`：对每张 fixture 做单字段极大值/清零、512 字节整块清零、深处结构破坏共数百个变异，要求"正常返回或明确报错"，不允许崩溃/越界），并在 CI 增设 `linux-sanitizers` 任务用 ASan+UBSan 重跑同一套用例；待完成超大目录/超长路径/稀疏范围的压力测试、资源上限（内存/时间）与长跑 fuzzing。
8. **发布流程**：依赖许可证与随包声明已完成（`docs/LICENSES.md`、`docs/EULA.md`）；可复现构建已验证并记录口径（`docs/REPRODUCIBLE.md`，`scripts/repro-check.ps1` 两次独立构建哈希一致），依赖包与外层 ZIP 仍不可复现，靠 SHA-256 钉住。平台打包只有 Windows x64 ZIP。版本兼容表改为从二进制生成（`uife capabilities --json` → `scripts/capability-matrix.py` → `docs/COMPATIBILITY.md`），每发一版加一列，不再手抄；`tests/capabilities_test.py` 在 CI 上校验申报自洽（一行 JSON、字段齐备、「可读」与「仅识别」不重叠、退出码不变）。

## 7. 版本规划建议

- **MVP（当前阶段）**：保持明确只读、CLI 基本操作和已列支持变体；优先修复真实样本发现的问题。
- **兼容性阶段**：建立格式/变体样本矩阵，拓展后端并验证分卷、损坏和加密行为。
- **取证增强阶段**：完善元数据与审计记录，完成只读不变性回归、跨平台 CI 与独立验证。
- **性能与发布阶段**：基于公开的可复现基准优化，逐平台生成包并提供签名/校验信息。

任何阶段均不得将“目标格式清单中的格式名称”直接宣传为“该格式所有版本均已支持”。发布说明必须列出可读变体、依赖条件和已知限制。

## 8. 相关文档

- [README：构建、使用、逐格式能力和当前项目状态](../README.md)
- [编码规范：C++ 风格、只读解析安全、模块职责和测试要求](./CODING_STANDARDS.md)
- [可复现构建：机制、验证方法与已知的非确定来源](./REPRODUCIBLE.md)
- [性能实测：容器吞吐、目录列举复杂度与未达标的验收口径](./PERFORMANCE.md)
- [版本兼容表：由二进制自报生成，含平台差异](./COMPATIBILITY.md)
- [变异测试：覆盖范围、算子、CI 节奏与已知边界](./FUZZING.md)
