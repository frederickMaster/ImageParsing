#!/usr/bin/env python3
"""顺序读取基准：把 `uife bench` 的数字变成可判定的回归门。

共享 CI runner 的绝对吞吐每天都在抖（邻居负载、CPU 型号、页缓存状态），
所以这里不拿绝对 MiB/s 当判据：

  1. 同一次运行里先测一个 256 MiB RAW 文件作为参照，再测各容器格式，算出
     **相对比值**。比值只反映驱动自身的开销（解封装、L2 查表、解压缩），
     与机器快慢基本无关。
  2. 绝对吞吐只做"灾难性退化"下限（默认 5 MiB/s），防止把顺序读退化成按
     字节读、或每读一块就重开一次文件。
  3. 基线文件存的是每个用例的**比值下限**，不是某台机器的绝对值。

输入默认由脚本自己生成（256 MiB RAW + 同内容的 QCOW2），因为几 MB 的样本
全在页缓存里，测出来的只是 memcpy 速度。`--corpus` 可以再叠加真实样本。

用法：
  python3 tests/bench.py --exe build/uife \
      --baseline tests/bench/baseline.json \
      --report build/bench.md --json build/bench.json

阈值没达标时退出码为 1（`--no-strict` 可只记录不判定）。
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_MIN_REFERENCE_MIB = 5.0    # 绝对下限：低于此值说明读路径本身出了问题
DEFAULT_MIN_RATIO = 0.40           # 相对参照的默认下限
DEFAULT_SIZE_MIB = 256             # 生成样本的容量：要大到能看出每簇开销
GATING_MIN_BYTES = 1024 * 1024     # 小于 1 MiB 的样本只报数，不参与判定


def _load_corpus_builders():
    """复用 scripts/gen-corpus.py 里的 QCOW2 封装，避免两处各写一份格式代码。"""
    path = Path(__file__).resolve().parent.parent / "scripts" / "gen-corpus.py"
    spec = importlib.util.spec_from_file_location("uife_gen_corpus", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_generated(work_dir: Path, size_mib: int) -> list[tuple[str, Path]]:
    builders = _load_corpus_builders()
    size = size_mib * 1024 * 1024

    raw_path = work_dir / f"raw-{size_mib}m.bin"
    if not raw_path.exists() or raw_path.stat().st_size != size:
        # 写零而不是随机数：随机数生成会成为脚本自身的瓶颈，且对读路径没有区别。
        with raw_path.open("wb") as handle:
            chunk = bytes(4 * 1024 * 1024)
            for _ in range(size // len(chunk)):
                handle.write(chunk)

    qcow2_path = work_dir / f"qcow2-{size_mib}m.qcow2"
    if not qcow2_path.exists():
        # 64 KiB 簇 → 每张 L2 覆盖 512 MiB，256 MiB 只用一张表。
        qcow2_path.write_bytes(bytes(builders._qcow2(raw_path.read_bytes(), 16)))
    return [(f"generated/raw-{size_mib}m", raw_path),
            (f"generated/qcow2-{size_mib}m", qcow2_path)]


def find_samples(corpus: Path) -> list[tuple[str, Path]]:
    """语料目录下每个用例目录取第一个非 expect.json 的文件。"""
    samples: list[tuple[str, Path]] = []
    for case_dir in sorted(path for path in corpus.rglob("*") if path.is_dir()):
        files = sorted(entry for entry in case_dir.iterdir()
                       if entry.is_file() and entry.name != "expect.json")
        if not files:
            continue
        samples.append((case_dir.relative_to(corpus).as_posix(), files[0]))
    return samples


def run_bench(exe: Path, sample: Path, size: int | None) -> dict:
    command = [str(exe), "bench", str(sample), "--json"]
    if size is not None:
        command += ["--size", str(size)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"uife bench 失败（退出码 {result.returncode}）: {result.stderr.strip()}")
    payload = json.loads(result.stdout)
    payload["sample"] = str(sample)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--json", required=True, type=Path)
    parser.add_argument("--baseline", type=Path,
                        help="基线 JSON（记录每个用例的比值下限）")
    parser.add_argument("--corpus", type=Path,
                        help="额外测量一份兼容性语料（小样本只报数不判定）")
    parser.add_argument("--work-dir", type=Path,
                        help="生成样本的落盘位置（默认临时目录，结束时保留）")
    parser.add_argument("--size-mib", type=int, default=DEFAULT_SIZE_MIB)
    parser.add_argument("--size", type=int,
                        help="每个样本只读前 N 字节（默认整卷）")
    parser.add_argument("--reference", help="参照用例名；默认生成的 RAW 样本")
    parser.add_argument("--no-strict", action="store_true",
                        help="阈值不达标也只记录，不返回失败")
    args = parser.parse_args()

    baseline: dict = {}
    if args.baseline and args.baseline.exists():
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    min_ratio_default = float(baseline.get("default_min_ratio", DEFAULT_MIN_RATIO))
    floors: dict[str, dict] = baseline.get("cases", {})
    min_reference = float(baseline.get("min_reference_mib_per_second",
                                       DEFAULT_MIN_REFERENCE_MIB))
    gating_min_bytes = int(baseline.get("gating_min_bytes", GATING_MIN_BYTES))

    work_dir = args.work_dir or Path(tempfile.mkdtemp(prefix="uife-bench-"))
    work_dir.mkdir(parents=True, exist_ok=True)
    samples = build_generated(work_dir, args.size_mib)
    if args.corpus:
        # 显式给了语料却一个样本都没找到，通常是路径写错或语料生成失败；
        # 静默退化成"只测两个自生成样本"会让基准看起来仍然通过。
        extra = find_samples(args.corpus)
        if not extra:
            print(f"语料目录 {args.corpus} 下没有找到任何样本；"
                  f"请检查生成步骤是否成功", file=sys.stderr)
            return 1
        samples += extra
    if not samples:
        print("没有可测量的样本", file=sys.stderr)
        return 1

    reference = args.reference or baseline.get("reference") or samples[0][0]
    measurements: dict[str, dict] = {}
    reference_mib = None
    ordered = sorted(samples, key=lambda item: item[0] != reference)
    for name, path in ordered:
        try:
            payload = run_bench(args.exe, path, args.size)
        except (RuntimeError, json.JSONDecodeError) as error:
            print(f"[失败] {name}: {error}", file=sys.stderr)
            return 1
        measurements[name] = payload
        if name == reference:
            reference_mib = payload["mib_per_second"]
        print(f"[测量] {name}: {payload['mib_per_second']:.1f} MiB/s "
              f"({payload['bytes']} bytes / {payload['seconds']:.3f}s)")

    if not reference_mib:
        print("参照样本吞吐为 0，无法计算比值", file=sys.stderr)
        return 1

    rows = []
    failures = []
    for name, _ in samples:
        payload = measurements[name]
        ratio = payload["mib_per_second"] / reference_mib
        floor = floors.get(name, {})
        min_ratio = float(floor.get("min_ratio", min_ratio_default))
        gated = name == reference or payload["bytes"] >= gating_min_bytes
        reasons = []
        if gated:
            if ratio < min_ratio:
                reasons.append(f"相对参照比值 {ratio:.2f} < 下限 {min_ratio:.2f}")
            if payload["mib_per_second"] < min_reference:
                reasons.append(
                    f"绝对吞吐 {payload['mib_per_second']:.1f} MiB/s "
                    f"< 下限 {min_reference:.1f}")
        if not gated:
            verdict = "仅记录"
        elif reasons:
            verdict = "退化: " + "; ".join(reasons)
        else:
            verdict = "通过"
        if reasons:
            failures.append((name, verdict))
        rows.append({
            "case": name,
            "format": payload.get("format"),
            "bytes": payload["bytes"],
            "seconds": payload["seconds"],
            "mib_per_second": payload["mib_per_second"],
            "ratio": ratio,
            "min_ratio": min_ratio,
            "gated": gated,
            "verdict": verdict,
        })

    args.report.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# UIFE 顺序读取基准",
        "",
        f"- 参照用例: {reference}（{reference_mib:.1f} MiB/s）",
        f"- 绝对下限: {min_reference:.1f} MiB/s；默认比值下限: {min_ratio_default:.2f}",
        f"- 判定门槛: 样本 >= {gating_min_bytes} 字节才参与判定（更小的只报数）",
        "- 比值相对同一次运行的参照样本计算，用来抵消机器与页缓存差异。",
        "",
        "| 用例 | 格式 | 字节 | 秒 | MiB/s | 相对参照 | 比值下限 | 结果 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        lines.append("| " + " | ".join([
            row["case"],
            row["format"] or "—",
            str(row["bytes"]),
            f"{row['seconds']:.3f}",
            f"{row['mib_per_second']:.1f}",
            f"{row['ratio']:.2f}",
            f"{row['min_ratio']:.2f}",
            row["verdict"],
        ]) + " |")
    lines.append("")
    args.report.write_text("\n".join(lines), encoding="utf-8")

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps({
        "reference": reference,
        "reference_mib_per_second": reference_mib,
        "min_reference_mib_per_second": min_reference,
        "rows": rows,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    gated_rows = [row for row in rows if row["gated"]]
    print(f"\n共 {len(rows)} 个样本（判定 {len(gated_rows)} 个），"
          f"{len(failures)} 个低于下限；报告: {args.report}")
    if failures:
        for name, verdict in failures:
            print(f"  - {name}: {verdict}")
        return 0 if args.no_strict else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
