#!/usr/bin/env python3
"""下载一小组公开的真实镜像样本，用于手工验证与语料建设。

为什么不随仓库携带：证据镜像体积大、且真实数据涉及隐私与版权。这里只挑
"小而合法、能覆盖单一格式特性"的公开样本，全部来自两个长期存在的来源：

  * digitalcorpora.org（NPS 测试镜像，公有领域，E01/raw，含 NTFS 与 ext3 卷）
  * qemu 的 tests/qemu-iotests/sample_images（由原生工具生成、仅供测试的
    极小镜像，覆盖 VHDX 日志、streamOptimized VMDK、DMG 等变体）

每个条目都钉 size 与 sha256，所以下载完整性不依赖 TLS：哈希对不上就报错退出。

用法：
    python scripts/fetch-samples.py --list
    python scripts/fetch-samples.py --out D:/tmp/uife-samples
    python scripts/fetch-samples.py --out D:/tmp/uife-samples --only e01-ntfs,dmg-hfs
"""

from __future__ import annotations

import argparse
import bz2
import hashlib
import ssl
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

S3 = "https://digitalcorpora.s3.amazonaws.com/corpora/drives"
QEMU = ("https://gitlab.com/qemu-project/qemu/-/raw/master/"
        "tests/qemu-iotests/sample_images")


@dataclass(frozen=True)
class Sample:
    key: str
    filename: str
    url: str
    size: int
    sha256: str
    covers: str
    compression: str = ""
    note: str = ""


SAMPLES: tuple[Sample, ...] = (
    Sample(
        key="e01-ntfs",
        filename="ntfs1-gen0.E01",
        url=f"{S3}/nps-2009-ntfs1/ntfs1-gen0.E01",
        size=1089252,
        sha256="96e525f53d50f986461151f8e9c07588633215477a6b8a3f744b2eeebe512460",
        covers="真实 E01 + 单卷 NTFS（含 ADS、稀疏、ACL、压缩目录）",
        note="1 MB 的 E01 解出 516 MB 卷，正好覆盖 EWF 解压路径",
    ),
    Sample(
        key="e01-ext3",
        filename="ubnist1-casper-rw-gen0.E01",
        url=f"{S3}/nps-2009-casper-rw/ubnist1.casper-rw.gen0.E01",
        size=1162991,
        sha256="dd6408ab2ed13b422ab9c4e1f4d2872d20e0b7e6e1cb82732a6bcfdccec25022",
        covers="真实 E01 + ext3 卷",
        note="Ubuntu 8.10 的 casper-rw 分区",
    ),
    Sample(
        key="dmg-hfs",
        filename="simple-dmg.dmg",
        url=f"{QEMU}/simple-dmg.dmg.bz2",
        size=19479,
        sha256="a1b8b5a17fb487dcc77e9aced334ba5e3d6fba7f11c9e0df85b6171ed57e60e7",
        covers="Apple DMG + GPT + HFS+ 分区",
        compression="bz2",
        note="qemu 测试样本，19 KB 解压后 1.7 MB",
    ),
    Sample(
        key="vhdx-dynamic",
        filename="iotest-dynamic-1G.vhdx",
        url=f"{QEMU}/iotest-dynamic-1G.vhdx.bz2",
        size=104857600,
        sha256="a4fb24fa51fb4852d5a6bdc2b390a91b0a4e19b47696edc5a00c816067257402",
        covers="Hyper-V 生成的动态 VHDX（真实 metadata 布局）",
        compression="bz2",
        note=("正是这个样本暴露出 logical sector size 的 GUID 抄错一个字节"
              "（a96f 写成 a996），真实镜像因此被判为"
              "\"含不支持的必需 metadata item\""),
    ),
    Sample(
        key="vhdx-dirtylog",
        filename="iotest-dirtylog-10G-4M.vhdx",
        url=f"{QEMU}/iotest-dirtylog-10G-4M.vhdx.bz2",
        size=31457280,
        sha256="511daba998dba208ffc57a7814194d5dd3afb7c314731b904ff1682e3fb4951a",
        covers="VHDX 日志未提交（LogGuid 非零）",
        compression="bz2",
        note="期望行为：明确报告\"需要日志重放\"并拒绝，而不是当损坏镜像",
    ),
    Sample(
        key="vmdk-stream",
        filename="iotest-version3.vmdk",
        url=f"{QEMU}/iotest-version3.vmdk.bz2",
        size=282624,
        sha256="0b67f248809938335a6f9375a8a76b3079f7c7e355df2837d5e05c9b45aaad5c",
        covers="VMDK streamOptimized（sparse version 3，压缩 grain）",
        compression="bz2",
        note="期望行为：说明是 version 3 变体、当前只支持 hosted sparse",
    ),
)


def pick(names: str) -> list[Sample]:
    if not names:
        return list(SAMPLES)
    wanted = {item.strip() for item in names.split(",") if item.strip()}
    known = {sample.key for sample in SAMPLES}
    unknown = wanted - known
    if unknown:
        raise SystemExit(f"未知样本: {', '.join(sorted(unknown))}；用 --list 查看")
    return [sample for sample in SAMPLES if sample.key in wanted]


def download(sample: Sample) -> bytes:
    request = urllib.request.Request(sample.url, headers={"User-Agent": "uife-samples"})
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            return response.read()
    except urllib.error.URLError as error:
        # 本机某些环境的根证书链不完整（企业代理/精简 CA 存储）。样本本身用
        # sha256 钉死，所以这里退化成"不校验证书"不会降低完整性——但要说出来，
        # 因为它同时意味着别的 HTTPS 访问也不可信。
        if "CERTIFICATE_VERIFY_FAILED" not in str(error):
            raise
        print(f"    [warn] TLS 校验失败，改用不校验的连接（内容仍按 sha256 校验）：{error}")
        context = ssl._create_unverified_context()
        with urllib.request.urlopen(request, timeout=180, context=context) as response:
            return response.read()


def fetch(sample: Sample, out_dir: Path, force: bool) -> bool:
    target = out_dir / sample.filename
    if target.exists() and not force:
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        if digest == sample.sha256:
            print(f"[skip] {sample.filename}（已存在且哈希一致）")
            return True
        print(f"[stale] {sample.filename} 哈希不符，重新下载")

    print(f"[get ] {sample.filename} <- {sample.url}")
    payload = download(sample)
    if sample.compression == "bz2":
        payload = bz2.decompress(payload)
    digest = hashlib.sha256(payload).hexdigest()
    if len(payload) != sample.size or digest != sample.sha256:
        print(f"    [FAIL] 期望 {sample.size} 字节 / {sample.sha256}")
        print(f"           实际 {len(payload)} 字节 / {digest}")
        return False
    target.write_bytes(payload)
    print(f"    ok {len(payload)} 字节")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="uife-samples", help="输出目录（默认 ./uife-samples）")
    parser.add_argument("--only", default="", help="只下载指定 key，逗号分隔")
    parser.add_argument("--list", action="store_true", help="只列出样本，不下载")
    parser.add_argument("--force", action="store_true", help="即使已存在也重新下载")
    args = parser.parse_args()

    if args.list:
        for sample in SAMPLES:
            print(f"{sample.key:14s} {sample.size:>9d}  {sample.covers}")
            if sample.note:
                print(f"{'':14s} {'':>9s}  └ {sample.note}")
        return 0

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    failed = [sample.key for sample in pick(args.only)
              if not fetch(sample, out_dir, args.force)]
    if failed:
        print(f"\n失败: {', '.join(failed)}")
        return 1
    print(f"\n样本目录: {out_dir.resolve()}")
    print("下一步: uife info <样本>；批量比对用 tests/compat_matrix.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
