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
- 自建样本无法覆盖：损坏镜像、多卷 E01、加密 APFS、真实 Windows ACL 组合、
  XFS/Btrfs 等 Linux 文件系统。这些仍需外部样本。
- **APFS 没有自建样本**：Windows 无法创建 APFS 容器（`diskpart` 与
  `Format-Volume` 都不支持），因此 libfsapfs 后端只在“伪造 NXSB 头必须被拒绝”
  这类负面用例上验证过，正向浏览与口令解锁需要 macOS 侧镜像或第三方 APFS 样本。
- **ReFS 样本可造但读不了**：`refs.vhd` 是真实的 Windows Server 2016 ReFS 卷，
  ReFS 签名与分区识别正常；但 libfsrefs 只实现早期 ReFS 布局，打开时在 ministore
  容器树处失败（其自带 `fsrefsinfo` 报同样的错），所以该用例的期望是
  `readable=true` + `list=error`，用来锁住这条能力边界而不是假装能读。

## 2. 目录约定与断言

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
