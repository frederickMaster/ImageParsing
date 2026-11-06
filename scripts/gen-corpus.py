#!/usr/bin/env python3
"""生成一份可在 CI 里复现的最小兼容性语料。

真实证据镜像体积大、含敏感数据，不能进版本库（见 tests/corpus/README.md）。
但"兼容性矩阵只在手动触发且有外部语料时才跑"等于没有回归保护，所以这里用
纯 Python 构造几个体积以 MB 计、内容完全确定性的样本，让矩阵每次提交都能跑：

  fs/fat16-minimal        最小 FAT16 卷，文件内容可校验
  fs/fat16-overshoot-1    BPB 声明比镜像多一个扇区（真实证据常见形态）
  fs/fat16-overshoot-200  BPB 声明多 200 个扇区（几何不可信，必须拒绝）
  raw/mbr-fat16           MBR 分区表 + FAT16 分区（验证偏移识别）
  unknown/random-4k       随机字节，必须被识别为"未识别"而不是崩溃

后两个 overshoot 用例锁住容量校验的容差边界：只容忍扇区级偏差，不容忍
"伪文件系统"。外部工具可用时（ewfacquire / mke2fs / genisoimage）额外生成
EWF、ext4、ISO 样本；工具缺失只是少几个用例，不影响必需用例。

用法：
  python3 scripts/gen-corpus.py --output build/corpus
  python3 scripts/gen-corpus.py --output build/corpus --clean
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SECTOR = 512
ROOT_ENTRIES = 512
FAT_SECTORS = 47
RESERVED_SECTORS = 1
TOTAL_SECTORS = 12032  # 簇 11905 个，落在 FAT16 区间


def _fat16(total_sectors: int) -> bytearray:
    """构造一个只有 HELLO.TXT 的最小 FAT16 卷。

    total_sectors 只写进 BPB，镜像实际长度固定：用来复现"分区长度与 BPB 声明
    不一致"的真实形态。布局与 tests/uife_tests.cpp 的 fat16_fixture 一致。
    """
    root_sectors = ROOT_ENTRIES * 32 // SECTOR
    data_start = RESERVED_SECTORS + 2 * FAT_SECTORS + root_sectors
    image = bytearray(TOTAL_SECTORS * SECTOR)

    image[0:3] = b"\xeb\x3c\x90"
    image[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", image, 11, SECTOR)
    image[13] = 1  # 每簇 1 个扇区
    struct.pack_into("<H", image, 14, RESERVED_SECTORS)
    image[16] = 2  # 两份 FAT
    struct.pack_into("<H", image, 17, ROOT_ENTRIES)
    struct.pack_into("<H", image, 19, total_sectors)
    image[21] = 0xF8
    struct.pack_into("<H", image, 22, FAT_SECTORS)
    struct.pack_into("<H", image, 24, 32)  # 每磁道扇区
    struct.pack_into("<H", image, 26, 2)   # 磁头
    struct.pack_into("<I", image, 28, 0)   # 隐藏扇区
    struct.pack_into("<I", image, 32, 0)   # 32 位总扇区数
    image[36] = 0x80
    image[38] = 0x29
    struct.pack_into("<I", image, 39, 0x12345678)
    image[43:54] = b"NO NAME    "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xaa"

    # FAT[0]/FAT[1] 保留表项，FAT[2]=EOC 表示 HELLO.TXT 占用簇 2。
    fat_offset = RESERVED_SECTORS * SECTOR
    struct.pack_into("<H", image, fat_offset, 0xFFF8)
    struct.pack_into("<H", image, fat_offset + 2, 0xFFFF)
    struct.pack_into("<H", image, fat_offset + 4, 0xFFFF)
    image[(RESERVED_SECTORS + FAT_SECTORS) * SECTOR:
          (RESERVED_SECTORS + 2 * FAT_SECTORS) * SECTOR] = \
        image[fat_offset:fat_offset + FAT_SECTORS * SECTOR]

    root_offset = (RESERVED_SECTORS + 2 * FAT_SECTORS) * SECTOR
    image[root_offset:root_offset + 11] = b"HELLO   TXT"
    image[root_offset + 11] = 0x20  # archive
    struct.pack_into("<H", image, root_offset + 26, 2)  # 起始簇
    struct.pack_into("<I", image, root_offset + 28, 5)  # 文件大小
    image[data_start * SECTOR:data_start * SECTOR + 5] = b"hello"
    return image


def _mbr_fat16() -> bytearray:
    """把 FAT16 卷放进 MBR 分区表，起始 LBA 2048（1 MiB 对齐）。"""
    start = 2048
    volume = _fat16(TOTAL_SECTORS)
    image = bytearray((start + TOTAL_SECTORS) * SECTOR)
    image[start * SECTOR:] = volume
    entry = 446
    image[entry + 0] = 0x00                 # 非活动分区
    image[entry + 1:entry + 4] = b"\xfe\xff\xff"  # CHS 起始（解析器只信 LBA）
    image[entry + 4] = 0x06                 # FAT16（CHS/LBA）
    image[entry + 5:entry + 8] = b"\xfe\xff\xff"
    struct.pack_into("<I", image, entry + 8, start)
    struct.pack_into("<I", image, entry + 12, TOTAL_SECTORS)
    image[510:512] = b"\x55\xaa"
    return image


def _random_bytes() -> bytearray:
    return bytearray(random.Random(20261106).randbytes(4096))


def _ext_volume(tool: list[str]) -> bytes | None:
    """用 mke2fs -d 生成一个装满目录的 ext4 卷（不需要 root 或 loop 设备）。"""
    try:
        import tempfile

        with tempfile.TemporaryDirectory() as staging:
            root = Path(staging)
            (root / "hello.txt").write_bytes(b"hello\n")
            (root / "sub").mkdir()
            (root / "sub" / "nested.bin").write_bytes(b"\x00\x01\x02\x03")
            image = root / "sample.ext4"
            result = subprocess.run(
                tool + ["-q", "-F", "-t", "ext4", "-b", "1024", "-d", str(root),
                        str(image), "8192"],
                capture_output=True, text=True)
            if result.returncode != 0:
                return None
            return image.read_bytes()
    except OSError:
        return None


def _ewf_wrap(source: bytes) -> bytes | None:
    """用 ewfacquire 把裸卷封装成 E01，覆盖 libewf 解封装路径。"""
    import tempfile

    try:
        with tempfile.TemporaryDirectory() as staging:
            raw = Path(staging) / "source.raw"
            raw.write_bytes(source)
            target = Path(staging) / "sample"
            # 用参数把 case/examiner 等元数据填满，ewfacquire 就不会进入交互提示；
            # 万一是老版本仍要提问，stdin 的空行会接受默认值。
            result = subprocess.run(
                ["ewfacquire", "-q", "-t", str(target), "-f", "encase6",
                 "-c", "none", "-m", "fixed", "-M", "logical",
                 "-C", "1", "-D", "uife-ci", "-e", "uife", "-E", "uife-ci",
                 "-N", "generated by scripts/gen-corpus.py", str(raw)],
                input="\n" * 8, capture_output=True, text=True)
            produced = Path(str(target) + ".E01")
            if result.returncode != 0 or not produced.exists():
                return None
            return produced.read_bytes()
    except OSError:
        return None


def _iso_volume() -> bytes | None:
    import tempfile

    with tempfile.TemporaryDirectory() as staging:
        root = Path(staging)
        (root / "README.TXT").write_bytes(b"uife corpus\n")
        out = Path(staging) / "sample.iso"
        for tool in (["xorriso", "-as", "mkisofs"], ["genisoimage"]):
            try:
                result = subprocess.run(
                    tool + ["-quiet", "-o", str(out), str(root)],
                    capture_output=True, text=True)
            except OSError:
                continue
            if result.returncode == 0 and out.exists():
                return out.read_bytes()
    return None


def _has(tool: str) -> bool:
    return shutil.which(tool) is not None


def cases() -> list[dict]:
    """用例定义：name/group、构造器、期望结果、依赖的外部工具。"""
    fat16 = _fat16(TOTAL_SECTORS)
    return [
        {
            "group": "fs",
            "name": "fat16-minimal",
            "sample": "sample.raw",
            "data": fat16,
            "requires": [],
            "expect": {
                "format_contains": "RAW",
                "filesystem_contains": "FAT",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "extract": "ok",
                "integrity_absent": True,
                "notes": "由 scripts/gen-corpus.py 生成的最小 FAT16 卷（CI 必需用例）",
            },
        },
        {
            "group": "fs",
            "name": "fat16-overshoot-1",
            "sample": "sample.raw",
            "data": _fat16(TOTAL_SECTORS + 1),
            "requires": [],
            "expect": {
                "format_contains": "RAW",
                "filesystem_contains": "FAT",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "notes": "BPB 声明比镜像多一个扇区：扇区级几何偏差必须仍可浏览",
            },
        },
        {
            "group": "fs",
            "name": "fat16-overshoot-200",
            "sample": "sample.raw",
            "data": _fat16(TOTAL_SECTORS + 200),
            "requires": [],
            "expect": {
                # probe 的 readable 是"该位置有可用后端"，几何校验发生在挂载时：
                # 因此这里 readable 仍为 True，而 list 必须失败（拒绝挂载而非越界读）。
                "format_contains": "RAW",
                "filesystem_contains": "FAT",
                "readable": True,
                "probe_exit": 0,
                "list": "error",
                "notes": "BPB 声明多出 200 个扇区：几何不可信，列表必须被拒绝而不是越界读",
            },
        },
        {
            "group": "raw",
            "name": "mbr-fat16",
            "sample": "sample.raw",
            "data": _mbr_fat16(),
            "requires": [],
            "expect": {
                "format_contains": "RAW",
                "filesystem_contains": "FAT",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "extract": "ok",
                "notes": "MBR 分区表 + 偏移 1 MiB 的 FAT16 分区（CI 必需用例）",
            },
        },
        {
            "group": "unknown",
            "name": "random-4k",
            "sample": "sample.bin",
            "data": _random_bytes(),
            "requires": [],
            "expect": {
                "format_contains": "RAW",
                "readable": False,
                "probe_exit": 0,
                "list": "error",
                "notes": "随机字节：识别为未识别，不崩溃、不误判文件系统",
            },
        },
        {
            "group": "ewf",
            "name": "fat16-e01",
            "sample": "sample.E01",
            "factory": lambda: _ewf_wrap(bytes(fat16)),
            "requires": ["ewfacquire"],
            "expect": {
                "format_contains": "EWF",
                "filesystem_contains": "FAT",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "extract": "ok",
                "integrity_absent": True,
                "notes": "ewfacquire 封装的 FAT16 卷：覆盖 libewf 解封装路径",
            },
        },
        {
            "group": "fs",
            "name": "ext4-minimal",
            "sample": "sample.raw",
            "factory": lambda: _ext_volume(["mke2fs"]),
            "requires": ["mke2fs"],
            "expect": {
                "format_contains": "RAW",
                "filesystem_contains": "ext",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "notes": "mke2fs -d 生成的 ext4 卷（无需 root/loop 设备）",
            },
        },
        {
            "group": "iso",
            "name": "iso9660-hello",
            "sample": "sample.iso",
            "factory": _iso_volume,
            "requires": ["xorriso|genisoimage"],
            "expect": {
                "format_contains": "ISO",
                "filesystem_contains": "ISO",
                "readable": True,
                "probe_exit": 0,
                "list": "ok",
                "notes": "ISO9660 卷：覆盖光学介质路径",
            },
        },
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="语料根目录（不存在则创建）")
    parser.add_argument("--clean", action="store_true",
                        help="先清空输出目录")
    args = parser.parse_args()

    if args.clean and args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True, exist_ok=True)

    written = 0
    skipped = 0
    for case in cases():
        missing = [tool for tool in case["requires"]
                   if not any(_has(name) for name in tool.split("|"))]
        if missing:
            print(f"[跳过] {case['group']}/{case['name']}: 缺少 {', '.join(missing)}")
            skipped += 1
            continue
        data = case.get("data")
        if data is None:
            data = case["factory"]()
            if data is None:
                print(f"[跳过] {case['group']}/{case['name']}: 生成工具执行失败")
                skipped += 1
                continue
        directory = args.output / case["group"] / case["name"]
        directory.mkdir(parents=True, exist_ok=True)
        (directory / case["sample"]).write_bytes(bytes(data))
        (directory / "expect.json").write_text(
            json.dumps(case["expect"], ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        print(f"[生成] {case['group']}/{case['name']} "
              f"({len(data)} bytes, {case['sample']})")
        written += 1

    print(f"共生成 {written} 个用例，跳过 {skipped} 个；语料目录: {args.output}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
