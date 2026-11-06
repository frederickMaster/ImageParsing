# 外部样本来源清单

本文件的样本**不进版本库**。体积、敏感性和许可都不适合入库，按 `tests/corpus/README.md`
的目录约定放在 `UIFE_CORPUS_DIR` 指向的位置，再用 `tests/compat_matrix.py` 跑。

三类来源：

- **Sleuth Kit 上游测试数据**（`tsk-2024/sleuthkit_test_data.zip`，17 MB）：一份压缩包里含 APFS、Btrfs、exFAT、ext2、XFS、GPT、FAT、NTFS 等镜像，是覆盖文件系统解析最省事的一站。
- **NIST CFReDS 归档站**（`cfreds-archive.nist.gov`）：真实采集的 E01/DD/RAW+CUE，含单段与多段 EWF、MODE2 轨道、损坏镜像。
- **qemu iotests sample_images**（`tests/qemu-iotests/sample_images`）：容器格式的边界变体，脏日志、4 KiB 扇区 VMDK、AFL 变异镜像等。GitHub raw 在国内网络常被挡，用 jsDelivr 或 GitLab 镜像取。

## 用例

| 用例 | 来源 | 大小 (bytes) | SHA-256 |
|---|---|---|---|
| `damaged/nist-edu-corrupt1` | Digital Corpora / NPS 2009 edu corrupt1<br>`https://downloads.digitalcorpora.org/corpora/drives/nps-2009-edu-corrupt1/gilbert1.E01` | 17987821 | `1ffd4e820ad6ac6e93add407bf675316bd7418260c9cdc91624e4e7d9c3a592d` |
| `ewf/nist-hacking-case` | NIST CFReDS Hacking Case (Dell CPi, Windows XP)<br>`https://cfreds-archive.nist.gov/images/4Dell%20Latitude%20CPi.E01` + `.E02` | 1090479548 | `sample.E01` 96bebe80f00541bf28fbc2ef0b02b580082ee6ad58837e991852ae66f077ec31<br>`sample.E02` 46bd09821dbb64675e5877d0ad7ec544a571fad5a3fd7fc3f0c3a16278887db5 |
| `ewf/nist-dlc-rm1` | NIST CFReDS Data Leakage Case, RM#1<br>`https://cfreds-archive.nist.gov/data_leakage_case/images/rm%231/cfreds_2015_data_leakage_rm%231.E01` | 78186742 | `a14150a21bc1e3700b51912c2ab20cd9587ad3e27ee67475af64508a7e760121` |
| `fs/apfs-pool` | Sleuth Kit upstream test data<br>`https://digitalcorpora.s3.amazonaws.com/corpora/drives/tsk-2024/sleuthkit_test_data.zip (apfs/apfs_pool.E01)` | 2089838 | `f08a7bb7565ee1778faa95a279215f3aca5fabfbffe0e34b96e0887059548400` |
| `fs/btrfs-50mb` | Sleuth Kit upstream test data<br>`(same zip) btrfs/btrfs_testimage_50MB.E01` | 3515470 | `42c5bda5e887002eaf6292fb0d20e022187837553bcdf4e84bf2c222b1e70b93` |
| `fs/btrfs-zstd` | Sleuth Kit upstream test data<br>`(same zip) btrfs/btrfs_zstd.E01` | 224369 | `c404949c4a72760dc7e394053aec06b3776903c52b442cc686977addc0e31942` |
| `fs/exfat-gpt` | Sleuth Kit upstream test data<br>`(same zip) exfat/exfat1.E01` | 274722 | `9249cbb06fef129cc411b5e1e65a6780cafad4dfa9f3566c583b5968504e5153` |
| `fs/ext2-small` | Sleuth Kit upstream test data<br>`(same zip) ufs/image.E01` | 4423 | `e94dfb49e8a6dc72b952127ebd32eba01396b4dfb62f359a38b89f360cbbbf30` |
| `fs/fat-undel` | Sleuth Kit upstream test data (DFTT 2004)<br>`(same zip) from_brian/6-fat-undel.E01` | 29591 | `a45d1ab4038cfb0d9be21c86e2d224789dff8b4f62671575cb5ba50027818960` |
| `fs/fat32-with-efs` | Sleuth Kit upstream test data<br>`(same zip) from_brian/fat32_with_efs_file.E01` | 491903 | `705109dfc554eceec4dc588bb489a8fc0cecd877eb9cd4a7044549338caa1f71` |
| `fs/gpt-130-parts` | Sleuth Kit upstream test data<br>`(same zip) gpt/gpt_130_partitions.E01` | 393651 | `c2ca9d37af9c8f249a9667b2a319bc25861d3b80ebdbdb7f4c74863e410a490f` |
| `fs/lvm-fuzz-3235` | Sleuth Kit upstream test data<br>`(same zip) fuzzing/lvm_test_issue_3235.E01` | 511222 | `2209ee278d79031079923647283b425e8e425af3c082a6d3dba81abb3d688f97` |
| `fs/ntfs-mmls` | Sleuth Kit upstream test data (DFTT 2004)<br>`(same zip) from_brian/imageformat_mmls_1.E01` | 414941 | `5125bbc40154a6acdf96b7aded0fc28686e9c0882afb5b95ddf2dc8345ccf44e` |
| `fs/xfs-2gb` | Sleuth Kit upstream test data<br>`(same zip) xfs/xfs-raw-2GB.E01` | 3958191 | `48397462ad7af18d4ac74534769fcbbe081712080918a0ca16e7063f62d5d772` |
| `optical/nist-dlc-rm3-bin-cue` | NIST CFReDS Data Leakage Case, RM#3 MODE2<br>`https://cfreds-archive.nist.gov/data_leakage_case/images/rm%233/cfreds_2015_data_leakage_rm%233_type1.7z` | 123513265 | `track.bin` af5e66a2103031dd6142b4a645261454970c9da7985ac435069d1f6bd3e45f10<br>`sample.cue` 7721492a8703c21f21b38fb24c474b87b21e8b9b543166a599c0875b54d64682 |
| `virtual/dmg-hfs` | qemu iotests sample_images<br>`https://cdn.jsdelivr.net/gh/qemu/qemu@master/tests/qemu-iotests/sample_images/simple-dmg.dmg.bz2` | 19479 | `a1b8b5a17fb487dcc77e9aced334ba5e3d6fba7f11c9e0df85b6171ed57e60e7` |
| `virtual/vhdx-dirty-log` | qemu iotests sample_images<br>`https://gitlab.com/qemu-project/qemu/-/raw/master/tests/qemu-iotests/sample_images/iotest-dirtylog-10G-4M.vhdx.bz2` | 31457280 | `511daba998dba208ffc57a7814194d5dd3afb7c314731b904ff1682e3fb4951a` |
| `virtual/vhdx-qemu-metadata` | qemu iotests sample_images<br>`https://cdn.jsdelivr.net/gh/qemu/qemu@master/tests/qemu-iotests/sample_images/iotest-dynamic-1G.vhdx.bz2` | 104857600 | `a4fb24fa51fb4852d5a6bdc2b390a91b0a4e19b47696edc5a00c816067257402` |
| `virtual/vmdk-truncated-afl` | qemu iotests sample_images<br>`https://cdn.jsdelivr.net/gh/qemu/qemu@master/tests/qemu-iotests/sample_images/afl9.vmdk.bz2` | 196608 | `71996b27606bda1c6ae0c3294595b03d8ab896923688632fe254a127b4f651e4` |
| `virtual/vmdk-v3-geometry` | qemu iotests sample_images<br>`https://cdn.jsdelivr.net/gh/qemu/qemu@master/tests/qemu-iotests/sample_images/iotest-version3.vmdk.bz2` | 282624 | `0b67f248809938335a6f9375a8a76b3079f7c7e355df2837d5e05c9b45aaad5c` |

## 解压与整理

```sh
# TSK 测试数据
mkdir -p $CORPUS/fs
unzip sleuthkit_test_data.zip -d /tmp/tsk
# 逐个文件系统复制成 <corpus>/fs/<case>/sample.E01，.xml/.xml2 侧车放 metadata/ 子目录
# （侧车名字排在 sample 之前时会被当成样本，务必放进子目录）

# CFReDS RM#3：7z 里是 RAW 轨道 + CUE
7z x cfreds_2015_data_leakage_rm#3_type1.7z
# 原始包内 CUE 引用的文件名与实际文件不一致，需改名后再喂给 BIN+CUE 驱动

# qemu 样本
bzip2 -d simple-dmg.dmg.bz2 iotest-dynamic-1G.vhdx.bz2
```

## 已知边界

- `optical/nist-dlc-rm3-bin-cue` 是 **MODE2/2352** 轨道，底层 UDF 5.13。两者都不支持，用例锁定的是“明确拒绝”而不是静默降级。
- `virtual/*` 四个用例是负例：脏日志、QEMU 私有 metadata item、4 KiB 扇区 VMDK、AFL 变异镜像，全部要求 probe 以退出码 1 失败。
- `fs/xfs-2gb` 能识别 XFS 但本地 libtsk 无 XFS API，用例锁定“识别但不可浏览”。
- `ewf/nist-hacking-case` 缺 `sample.E02` 时，读取会在缺段偏移处报 `missing segment file for offset`，不会静默短读；但分段完整性没有在 probe 阶段校验，属于待改进项。
- `fs/fat-undel` 的 BPB 声明 12033 个扇区而镜像只有 12032 个（差 1 个扇区），当前容量校验因此整盘拒绝。真实镜像常见这种 ±1 扇区偏差，容忍一个块的差额更合适。
- `fs/fat-undel`、`fs/btrfs-*`、`fs/gpt-130-parts`、`fs/lvm-fuzz-3235` 暂无 `expect.json`，只记录能力、不判定成败，等能力补齐后再定期望。
