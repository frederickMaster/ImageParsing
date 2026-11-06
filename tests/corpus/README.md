# UIFE 样本兼容性矩阵语料目录

本目录只定义**目录约定**，不存放真实证据镜像。取证样本通常体积大、含敏感数据，
不应进入版本库；把它放在受控的证据目录中，用 `UIFE_CORPUS_DIR` 指向该目录即可。

自建样本的生成方式见 `docs/SAMPLES.md`（`scripts/make_samples.ps1` 生成 NTFS/FAT32/ReFS 的
VHD，`scripts/tools/make_e01.c`、`make_wim.c` 转成 E01/WIM），这些样本不进入版本库。

## 目录结构

```text
<corpus>/
  <group>/                 # 自由分组，例如 ewf、vmdk、ntfs、damaged、encrypted
    <case>/                # 单个用例，目录名即用例名
      sample.E01           # 样本文件（目录内除 expect.json 外的第一个文件）
      expect.json          # 可选；期望结果
```

## expect.json

```json
{
  "format_contains": "EWF",
  "filesystem_contains": "NTFS",
  "readable": true,
  "probe_exit": 0,
  "list": "ok",
  "extract": "ok",
  "notes": "来源、采集工具、已知损坏情况"
}
```

- `format_contains`：`uife probe` 报告的镜像格式需包含该子串。
- `filesystem_contains`：任一位置的识别结果需包含该子串（例如 `NTFS`、`exFAT`、
  `已识别；未构建 libtsk 后端`）。
- `readable`：是否存在可浏览的文件系统（`probe` 的 `readable` 字段）。
- `probe_exit`：`uife probe --json` 的退出码。
- `list`：`ok` 或 `error`（`uife list <样本> /` 的结果）。
- `extract`：`ok` 或 `error`；`ok` 时脚本会执行全量导出，并按 `SHA256SUMS`
  逐项复核哈希。
- `notes`：自由文本，进入报告表格。

**没有 `expect.json` 的用例只记录能力，不判定成败**，适合先摸底再补期望值。
设置了期望值但不匹配的用例计为失败，脚本退出码为 1。

## 运行方式

```sh
python tests/compat_matrix.py \
  --exe build/uife \
  --corpus /evidence/uife-corpus \
  --report build/compat-matrix.md \
  --json build/compat-matrix.json
```

或通过 CMake 配置后交给 CTest：

```sh
cmake -S . -B build -DUIFE_CORPUS_DIR=/evidence/uife-corpus
ctest --test-dir build -R compat_matrix --output-on-failure
```

报告包含每个样本的识别格式、各位置文件系统、可浏览状态、`list`/`extract`
结果与失败原因；`--only <子串>` 可只跑匹配的用例。

## 已实测的外部样本

哪些公开来源可以下载、每个用例的体积与 SHA-256、以及解压整理时的坑，
记在 `SOURCES.md`。样本本身不进库，需要时按该文件的链接自行获取。

## 建议覆盖的样本类别

| 类别 | 目的 |
| --- | --- |
| 各格式合法单卷样本 | 验证正常浏览与导出 |
| 多卷/分卷（EWF 分段、GHO+GHS、WIM SWM） | 验证跨段读取与缺卷报错 |
| 压缩/加密/差分变体（QCOW2 backing、VHDX differencing、加密 ESD） | 验证“明确拒绝”而非误读 |
| 损坏样本（截断、校验和错误、分区表损坏） | 验证错误码与不崩溃 |
| 含 NTFS ADS / 重解析点 / 稀疏文件的样本 | 验证元数据导出与侧车文件 |
| Unicode 路径样本 | 验证 UTF-8 路径与文件名编码 |
