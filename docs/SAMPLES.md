# 自建测试样本与真实语料

本项目不随仓库携带证据镜像。测试样本有两条来源：**本地自建**（可复现、无版权问题）
与**外部真实样本**（覆盖面更广，但体积大、含敏感数据，需放在受控目录）。

## 1. 本地自建样本

自建样本依赖系统自身能力，不需要第三方取证工具：

| 步骤 | 依赖 | 产物 |
| --- | --- | --- |
| `scripts\make_samples.ps1` | 管理员权限、diskpart、支持 ReFS 的系统（可选） | `ntfs.vhd`、`fat32.vhd`、`refs.vhd` |
| `scripts\tools\make_e01.c` | libewf（`UIFE_LIBEWF_*`） | `<基础名>.E01` |
| `scripts\tools\make_wim.c` | wimlib（`UIFE_LIBWIM_*`） | 压缩 WIM |

### NTFS 样本内容

- `hello.txt`：普通文件 + 两个命名流（`secret`、`Zone.Identifier`）
- `onlyads.txt`：只有命名流、默认流为 0 字节
- `sparse.dat`：4 MB，前 1 MB 标记为未分配（用于验证稀疏 extents）
- `random.bin`：4 MB 确定性随机内容，NTFS 压缩
- `docs\`：普通子目录；`docslink`：目录联接（reparse tag `0xA0000003`）
- `symlink.txt`：文件符号链接（`0xA000000C`）；`hardlink.txt`：与 `hello.txt` 同 inode
- `secret.txt`：EFS 加密尝试（`cipher /e` 在 VHD 上可能静默失败；生成后用
  `uife --json list <镜像> /secret.txt` 检查 `metadata.encrypted`，为 `false` 说明未加密）

### 用法

```powershell
# 1) 生成 VHD 样本（会临时挂载 X:/Y:/Z: 并在结束时分离）
powershell -ExecutionPolicy Bypass -File scripts\make_samples.ps1 -OutputDir D:\tmp\samples

# 2) 编译 E01/WIM 生成器（MinGW + 自建 SDK）
gcc scripts\tools\make_e01.c -o make_e01.exe `
    -I<libewf>/include <libewf>/lib/libewf.a <zlib>/lib/libzs.a -static
gcc scripts\tools\make_wim.c -o make_wim.exe `
    -I<wimlib>/include -L<wimlib>/lib -lwim

# 3) 转成 E01 / WIM（基础名不要超过 8 字符，libewf 使用 8.3 命名）
cd D:\tmp\samples
..\make_e01.exe ntfs.vhd ntf
..\make_wim.exe D:\tmp\wimsrc sample
```

### 已知限制

- 固定 VHD 会被当作 RAW 镜像读取（尾部 512 字节元数据不影响 MBR/GPT 解析），
  因此 VHD 样本同时覆盖 "RAW + 分区表 + NTFS/FAT" 路径。
- ReFS 需要 ≥ 1 GB 卷（64 MB 会报 "卷太小"），且只有 Server / Pro for Workstations
  支持 `format fs=refs`；桌面版会跳过该样本。
- 自建样本无法覆盖：损坏镜像、多卷 E01、加密 APFS、XFS/Btrfs 等 Linux 文件系统。
  这些仍需外部样本。
- **ext4 样本由测试代码生成**：`tests/uife_integration_tests.cpp` 的
  `write_ext4_allocation_fixture()` 直接拼一张 512 KiB 的 ext4（1 KiB 块、单块组、
  extent 树），其中 `sparse.dat` 用"未初始化 extent"（`ee_len` 最高位为 1）造出
  [洞][数据][洞] 三段，用来锁住分配图与零填充读取结果一致。它不是通用格式化器，
  只保证 TSK 能挂载；真实工具链（`mke2fs -d`）生成的镜像仍建议放入语料目录。
- **WIM ACL 有两条自建路径**：`tests/uife_integration_tests.cpp` 里 `NO_ACLS` 的
  fixture 锁住"没有安全描述符时不编造"，另一份不带 `NO_ACLS` 的 fixture 在 Windows
  上会采集真实 NTFS 安全描述符，用来验证解码结果（owner SID 以 `S-1-` 开头、DACL
  非空）。POSIX 上 wimlib 不采集 Windows SD，该断言只要求"没报就是没报"。
- **APFS 没有自建样本**：Windows 无法创建 APFS 容器（`diskpart` 与
  `Format-Volume` 都不支持），因此 libfsapfs 后端只在“伪造 NXSB 头必须被拒绝”
  这类负面用例上验证过，正向浏览与口令解锁需要 macOS 侧镜像或第三方 APFS 样本。
- **ReFS 样本可造但读不了**：`refs.vhd` 是真实的 Windows Server 2016 ReFS 卷，
  ReFS 签名与分区识别正常；但 libfsrefs 只实现早期 ReFS 布局，打开时在 ministore
  容器树处失败（其自带 `fsrefsinfo` 报同样的错），所以该用例的期望是
  `readable=true` + `list=error`，用来锁住这条能力边界而不是假装能读。

## 2. 公开可下载的真实样本

有一条可以直接跑的路径：`scripts/fetch-samples.py` 会取回下面 6 个样本（合计约 137 MB，
全部公开、许可清晰），每个都钉了大小与 sha256，哈希不符即失败。

```bash
python scripts/fetch-samples.py --list                  # 看看有什么
python scripts/fetch-samples.py --out D:/tmp/uife-samples
```

| key | 文件 | 覆盖 | 解压后 | 当前结果（2026-11-07） |
|---|---|---|---|---|
| `e01-ntfs` | `ntfs1-gen0.E01` | 真实 E01 + 单卷 NTFS | 1.0 MB（卷 516 MB） | 可读，`list /` 含 ADS/稀疏/ACL/压缩目录 |
| `e01-ext3` | `ubnist1-casper-rw-gen0.E01` | 真实 E01 + ext3 | 1.1 MB（卷 629 MB） | 可读，ext3 解析正常 |
| `dmg-hfs` | `simple-dmg.dmg` | DMG + GPT + HFS+ | 19 KB | 可读，GPT 分区识别正确 |
| `vhdx-dynamic` | `iotest-dynamic-1G.vhdx` | Hyper-V 动态 VHDX | 100 MB | 可读 |
| `vhdx-dirtylog` | `iotest-dirtylog-10G-4M.vhdx` | VHDX 日志未提交 | 30 MB | 明确报告"需要日志重放"并拒绝 |
| `vmdk-stream` | `iotest-version3.vmdk` | VMDK streamOptimized（v3） | 276 KB | 明确说明只支持 hosted sparse（v1） |

来源只有两个，都长期存在：`digitalcorpora.org` 的 NPS 测试镜像（公有领域）与 qemu 的
`tests/qemu-iotests/sample_images`（由原生工具生成、专门用于测试的小镜像）。

`vhdx-dynamic` 这个样本是有价值的：它当初第一次跑就报了"含有当前不支持的必需
metadata item"——真实原因是 `logical_sector_size` 的 GUID 被抄成了 `a996`
（规范是 `a96f`），而测试 fixture 照抄了同一个错值，所以合成用例一路全绿。
现在 fixture 用 `guid_from_string("8141bf1d-a96f-4709-ba47-f233a8faab5f")` 从规范
文本生成，抄错必须先把文本改错。

### B1–B9 的样本可得性

| # | 缺口 | 能不能下载 | 具体来源与代价 |
|---|---|---|---|
| B1 | XFS 浏览 | 能，但不是小巧的"XFS 测试镜像" | `AlmaLinux-9-GenericCloud-latest.x86_64.qcow2`（586 MB，国内镜像 `mirrors.aliyun.com/almalinux/9/cloud/x86_64/images/`，需带浏览器 UA；根文件系统就是 XFS）。2026-11-07 实测：**XFS 识别已端到端验证**（info 正确认出两个 XFS 分区），浏览仍卡在上游——TSK 4.13 的实验性 XFS 在 4.14 被回退，4.15 无 XFS，`UIFE_TSK_HAS_XFS` 探测为假是正确行为 |
| B2 | ReFS 真实卷 | 上游只有结构片段 | `libyal/libfsrefs` 的 `tests/data/superblock.1` 等是 4 KiB 片段，不是整卷；整卷靠 `make_samples.ps1` 自建（需要 Server/Pro for Workstations） |
| B3 | APFS 真实镜像 | 能，但没有官方小样本 | 厂商 DMG（Zed 的 aarch64 包、BBEdit 16 都是 APFS 卷封在 DMG 里，100–200 MB），或 Josh Hickman 公开的 macOS/iOS 镜像（数 GB）。Windows 无法自建 APFS |
| B4 | 加密 ESD | 能 | 微软官方 Windows ISO 里的 `install.esd`（LZMS 压缩与加密两条路径都在里面） |
| B5 | Btrfs send stream | 基本不能 | 没有公开语料；`btrfs send` 需要在 Linux 上真实挂载 btrfs 才有输出，只能到 Linux 机器上生成 |
| B6 | QCOW2 压缩簇/backing/快照 | 压缩簇已实现，backing/快照仍拒 | **压缩簇读取已实现（2026-11-07）**：compressed cluster descriptor（`1<<62` + 字节粒度 host_offset + additional_sectors，raw deflate -15 解压 + 单 cluster 缓存），合成 fixture 锁行为，真实 AlmaLinux 10 GB 镜像（GPT+FAT+XFS）端到端读通。backing/snapshot/加密仍按既有策略明确拒绝；要造新样本才需要 `qemu-img`（`qemu.weilnetz.de/w64/`） |
| B7 | VHDX 日志/差分 | 部分 | 日志样本已在 `fetch-samples.py` 里；差分盘的读取仍需 Hyper-V 造样本，但驱动已能识别并明确拒绝（Has Parent 标志、Parent Locator item、Virtual Disk Type=4 三条路径均有合成 fixture 锁定，2026-11-07） |
| B8 | VMDK 快照/压缩 grain、VDI 差分、DMG 分卷 | 部分 | qemu `sample_images` 有 streamOptimized VMDK 与 DMG；VDI 用 `qemu-img`/VirtualBox 生成；VMDK 快照与 DMG 分卷没有现成公开样本 |
| B9 | E01/GHO/BIN+CUE 真实样本 | E01 与 BIN+CUE 能，GHO 能但只剩网盘渠道 | E01：digitalcorpora 的 NPS 系列（1 MB–161 MB/卷）、NIST CFReDS `Hacking Case`（`cfreds-archive.nist.gov/images/4Dell Latitude CPi.E01`，640 MB/卷，共 8 卷）；BIN+CUE：CFReDS `Data Leakage Case` 的 `rm#3-type1.7z`（92.8 MB）；**GHO**：本地测试不要求许可干净（2026-11-07 确认），但实测所有装机站 HTTP 直链已全部失效（`down.zhuangjiyuan.com:953` 超时、`down.newxitong.net:8001` 网关 502、archive.org 被网络屏蔽），现存分发全部走百度/移动/天翼网盘分享（需登录客户端，文件 3–9 GB，如 bbs.x6x8.com 帖 `W7x86_2606.GHO` 5.81 GB，MD5 已给）。拿到后放任意目录跑 `uife info` 即可验证解析层；合成 fixture 已覆盖未压缩/zlib/Fast LZ/多分区/拒绝路径，真实样本补的是"格式理解与真 Ghost 输出一致"这层 |

注意版权：CFReDS 与 digitalcorpora 的镜像里含微软可执行文件的"已破坏"副本，只授权用于
取证教学与研究；不要把它们打进发布包或 CI 缓存。

## 3. 目录约定与断言

`tests/corpus/README.md` 定义了 `<group>/<case>/sample.* + expect.json` 约定。
把样本放进去即可跑：

```bash
python tests/compat_matrix.py --exe build-local-full/uife.exe \
    --corpus D:/tmp/uife-corpus \
    --report D:/tmp/uife-corpus/report.md \
    --json   D:/tmp/uife-corpus/report.json
```

配置 `-DUIFE_CORPUS_DIR=<目录>` 时，`ctest` 会自动注册 `compat_matrix` 用例。

当前自建语料的期望结果（2026-11-04，`build-local-full`）：

| 用例 | 覆盖点 | 结果 |
| --- | --- | --- |
| `e01/ntfs-ads` | E01 读取、MBR 分区、ADS 导出、稀疏/压缩属性 | 通过 |
| `e01/fat32` | E01 + FAT32、目录层级 | 通过 |
| `raw/ntfs-vhd` | 固定 VHD 按 RAW 解析、硬链接去重 | 通过 |
| `raw/fat32-vhd` | FAT32 时间戳还原 | 通过 |
| `raw/refs-vhd` | ReFS 检测缺口（当前不可浏览） | 通过（按预期失败） |
| `wim/basic` | WIM 虚拟路径 `/images/N`、子目录导出 | 通过 |
| `iso/hello` | ISO9660 基础读取 | 通过 |
| `unknown/random` | 非镜像输入的拒绝路径 | 通过 |
