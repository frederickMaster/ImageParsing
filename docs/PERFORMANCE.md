# 性能实测

PRD 里有两条硬的验收口径：**单镜像解析速度 ≥100 MB/s**、**10 GB 以内镜像
加载 ≤10 秒**（后者等价于 ≥1 GB/s）。这份文档记录到目前为止的实际测量，
以及不能算达标的部分。

## 测量环境

| 项目 | 值 |
| --- | --- |
| CPU | 12th Gen Intel Core i7-12700 |
| 内存 | 63.7 GB |
| 系统 | Windows Server 2016 Standard（虚拟机） |
| 构建 | Release，MinGW g++ 16.2.0，`build-local-full`（五后端全开） |
| 命令 | `uife bench <镜像> --size 536870912` |

**重要前提：所有数字都是页缓存命中状态下的结果。** `bench` 顺序读同一批
扇区，第二次起就不碰盘；Windows 上清缓存要管理员工具（RAMMap 等），本轮
没有做。所以下表的吞吐反映的是**解封装/解压缩路径的开销**，不是磁盘带宽。

## 容器读取吞吐

| 样本 | 容器格式 | 介质类型 | 读取量 | 耗时 | 吞吐 |
| --- | --- | --- | --- | --- | --- |
| `ewf/nist-hacking-case` | EWF/E01（两段） | NTFS，4.87 GB 介质 | 512 MiB | 1.094 s | **468 MiB/s** |
| `fs/apfs-pool` | EWF/E01 → APFS | APFS | 512 MiB | 0.350 s | 1465 MiB/s |
| `raw/ntfs-vhd` | RAW | NTFS | 64 MiB | 0.010 s | 6243 MiB/s |
| `virtual/dmg-hfs` | Apple DMG | HFS+ | 1.7 MiB | 0.001 s | 1727 MiB/s |
| `e01/ntfs-ads` | EWF/E01 | NTFS | 64 MiB | 0.032 s | 2029 MiB/s |
| `wim/basic` | WIM/ESD | — | 6 KiB | <1 ms | 356 MiB/s |

读数要点：

- **最大的一档才是可信的**。512 MiB 那两行有意义；64 MiB 与 1.7 MiB 基本
  是在测 `memcpy`（页缓存里），只用来确认没有数量级退化。
- **EWF 是唯一带真实解压开销的容器**：468 MiB/s，是验收线（100 MB/s ≈ 95 MiB/s）
  的 4.9 倍，但离 1 GB/s 还差一半。也就是说 **"10 GB 在 10 秒内"对 EWF 不成立**，
  按实测外推约 22 秒。
- APFS 与 DMG 更快是因为样本在缓存里、且解压比 EWF 轻；不能当成 EWF 的替代证据。

## 目录列举的复杂度

`uife list <dir>` 走 `VFS::list_dir`，结果是 `std::vector<VFSNode>`：
**时间 O(条目数)，内存 O(条目数)**，每个条目带完整元数据（时间戳、权限、
ACE 列表、数据流名、extent 段），因此单条目成本远大于"一个文件名"。
libtsk 后端还要先把整个目录块读进内存再逐条解析。

目前的证据强度：

- 真实的 512 条目上限（FAT16 根目录）与 NTFS 几百条目目录都测过，耗时可忽略
  （毫秒级），无法据此画曲线。
- **尚缺"几十万条目"的真实样本**，所以没有 list 的实测曲线，只有复杂度结论。
- 兜底手段已经就位：`--max-entries` 在遍历阶段就中止，不会让进程被 OOM 杀掉
  （见 `tests/uife_tests.cpp` 的 `resource_limits`：5000 条目 + 上限 100，
  在写任何文件之前就失败）。

结论：**大目录 list 的复杂度是已知的 O(n)，绝对时间未知**。拿未知来源的
证据做批量处理时应设 `--max-entries`，把"跑不动"变成"明确失败"。

## 与验收口径的差距

| 口径 | 结论 | 说明 |
| --- | --- | --- |
| 单镜像解析速度 ≥100 MB/s | **达到**（测过的容器里最低 468 MiB/s） | EWF 解压是最坏情况；未测冷缓存 |
| 10 GB 以内镜像加载 ≤10 秒 | **未达到** | 需要 1 GB/s；EWF 实测 468 MiB/s，外推 22 秒。RAW 缓存命中时可达标，但这不算证据 |
| 大目录 list 复杂度 | 未知 | 缺大目录样本；复杂度随条目数线性 |

## 复现方法

```sh
# 同一个脚本、同一批样本，改 --size 控制读取量
uife bench /evidence/sample.E01 --size 536870912

# 或跑内置的相对基准（与同内容 RAW 比较，抵消机器差异）
python tests/bench.py --exe build/uife --work /tmp/bench
```

`tests/bench.py` 的判定用"相对同内容 RAW 的比值"，绝对吞吐只留 40 MiB/s
的灾难下限——共享 CI runner 上绝对数字没有可比性。要更新本页数字，务必
在同一台机器、同一构建、同一批样本上重跑，并保留 `--size`。
