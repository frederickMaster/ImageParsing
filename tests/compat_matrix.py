#!/usr/bin/env python3
"""UIFE 样本兼容性矩阵。

按目录约定扫描真实/合成样本，运行 `uife probe`、`uife list`、可选的
`uife extract`，把“识别到什么 / 能否浏览 / 能否导出并通过哈希校验”写成
Markdown 与 JSON 报告。样本与期望值分离，便于把外部取证语料加入版本库之外
的目录后重复执行。

目录约定（--corpus 指向的根目录）::

    <corpus>/<group>/<case>/
        sample.E01            # 任意一个非 expect.json 的文件即为样本
        expect.json           # 可选；缺省时只记录能力，不判定成败

expect.json 支持的键::

    {
      "format_contains": "EWF",          # probe 报告的镜像格式包含该子串
      "filesystem_contains": "NTFS",     # 任一位置的识别结果包含该子串
      "readable": true,                  # 是否存在可浏览的文件系统
      "probe_exit": 0,                   # probe 退出码
      "list": "ok",                      # "ok" | "error"
      "extract": "ok",                   # "ok" | "error"（ok 时校验 SHA-256 清单）
      "integrity_contains": "分卷不完整",  # probe 的完整性提示包含该子串
      "integrity_absent": true,          # 无完整性提示（完整镜像）
      "notes": "真实样本来源说明"
    }

未提供 expect.json 的样本按“仅记录”处理，不影响退出码；提供期望值但
不匹配的样本计入失败，脚本以退出码 1 结束，可直接作为 CTest 用例。

每个用例在跑之前和跑完之后都会给整个用例目录拍快照（文件清单、大小、
修改时间，默认还含 SHA-256），因此“工具改动了源镜像”或“在源目录里留下
临时文件”会直接判失败——这是只读属性在真实样本上的证明，而不是靠约定。
另外默认执行两项深检：把镜像头部 8 MiB 复制出来单独探测（截断输入必须
明确失败或正常返回，不能崩），以及多分卷镜像只给首卷时要求 probe 阶段
就报出缺卷。`--no-source-hash` / `--no-deep-checks` 分别关掉这两类检查。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

EXPECT_NAME = "expect.json"

# 截断预检只复制镜像头部这么多字节：足够走到格式头/分区表解析，又不至于
# 为 5 GB 的真实镜像再复制一遍整盘。
TRUNCATED_PROBE_BYTES = 8 * 1024 * 1024

# 缺分卷预检需要把首卷单独放进空目录；超过这个体积改用硬链接，避免重复占用磁盘。
MISSING_SEGMENT_COPY_LIMIT = 2 * 1024 * 1024 * 1024

SEGMENT_NAME = re.compile(r"(?i)\.e\d{2}$")

# 合法退出码见 README 的退出码表；其它值说明进程崩了（Windows 是 0xC0000005
# 这类巨大正数，POSIX 是负的信号号）。
VALID_EXIT_CODES = range(0, 5)


@dataclass
class CaseResult:
    group: str
    name: str
    sample: str
    image_format: str = ""
    filesystem: str = ""
    readable: bool = False
    probe_exit: int = -1
    list_status: str = ""
    list_entries: int = 0
    list_detail: str = ""
    extract_status: str = ""
    extract_detail: str = ""
    integrity_note: str = ""
    source_unchanged: bool = True
    source_changes: list[str] = field(default_factory=list)
    truncated_probe: str = ""
    missing_segment: str = ""
    failures: list[str] = field(default_factory=list)
    notes: str = ""
    has_expectations: bool = False


def run(exe: str, args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
    completed = subprocess.run(
        [exe] + args,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=str(cwd) if cwd else None,
        check=False,
    )
    return completed.returncode, completed.stdout, completed.stderr


def parse_json(text: str):
    text = text.strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_manifest(manifest: Path, output_root: Path) -> list[str]:
    problems: list[str] = []
    if not manifest.is_file():
        return ["清单文件缺失: " + str(manifest)]
    for line_number, line in enumerate(
            manifest.read_text(encoding="utf-8").splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        parts = line.split("  ", 1)
        if len(parts) != 2:
            problems.append(f"清单第 {line_number} 行格式错误")
            continue
        expected_hash, relative = parts
        target = output_root / relative
        if not target.is_file():
            problems.append(f"清单引用的文件不存在: {relative}")
            continue
        if sha256_of(target) != expected_hash:
            problems.append(f"清单哈希不匹配: {relative}")
    return problems


def find_sample(case_dir: Path) -> Path | None:
    for entry in sorted(case_dir.iterdir()):
        if entry.is_file() and entry.name != EXPECT_NAME:
            return entry
    return None


def fingerprint(path: Path, hash_contents: bool) -> dict:
    stat = path.stat()
    return {
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": sha256_of(path) if hash_contents else "",
    }


def snapshot_tree(root: Path, hash_contents: bool) -> dict[str, dict]:
    """给用例目录拍快照：文件清单 + 大小 + 修改时间（可选内容哈希）。

    直接调用取证工具的上下文里，"源镜像被改写"是致命缺陷，所以快照要能同时
    发现三种情况：内容变了、大小变了、目录里多出了工具自己写的临时文件。
    """
    snapshot: dict[str, dict] = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            snapshot[path.relative_to(root).as_posix()] = fingerprint(path, hash_contents)
    return snapshot


def diff_snapshot(before: dict[str, dict], after: dict[str, dict]) -> list[str]:
    changes: list[str] = []
    for name, old in before.items():
        new = after.get(name)
        if new is None:
            changes.append(f"{name}: 源文件消失")
            continue
        if new["size"] != old["size"]:
            changes.append(f"{name}: 大小 {old['size']} → {new['size']}")
            continue
        if old["sha256"] and new["sha256"] and new["sha256"] != old["sha256"]:
            changes.append(f"{name}: 内容哈希变化")
            continue
        if new["mtime_ns"] != old["mtime_ns"]:
            changes.append(f"{name}: 修改时间变化")
    for name in after.keys() - before.keys():
        changes.append(f"{name}: 源目录新增文件")
    return changes


def probe_truncated(exe: str, sample: Path) -> str:
    """把镜像头部复制成一个短文件再探测，要求"正常返回或明确报错"。

    覆盖真实样本的截断行为：不能崩溃、不能既非 0 又给不出 JSON 错误对象。
    取 8 MiB 与一半体积中较小的那个——小镜像一刀切一半，大镜像只截头部。
    """
    size = sample.stat().st_size
    if size < 64 * 1024:
        return "skipped"  # 样本本身就是几百字节的碎片，截断没有额外信息量
    limit = min(TRUNCATED_PROBE_BYTES, size // 2)
    with tempfile.TemporaryDirectory(prefix="uife-trunc-") as temporary:
        target = Path(temporary) / ("truncated" + "".join(sample.suffixes))
        remaining = limit
        with sample.open("rb") as source, target.open("wb") as sink:
            while remaining > 0:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    break
                sink.write(chunk)
                remaining -= len(chunk)
        code, stdout, _ = run(exe, ["--json", "probe", str(target)])
        if code not in VALID_EXIT_CODES:
            return f"崩溃（退出码 {code}）"
        if parse_json(stdout) is None:
            return "无 JSON 输出"
        return "ok"


def probe_missing_segment(exe: str, case_dir: Path) -> str:
    """多分卷镜像只给首卷时，必须在探测阶段就报出缺卷，而不是挂载时才失败。"""
    segments = sorted(path for path in case_dir.iterdir()
                      if path.is_file() and SEGMENT_NAME.search(path.name))
    if len(segments) < 2:
        return ""
    first = segments[0]
    with tempfile.TemporaryDirectory(prefix="uife-missing-seg-") as temporary:
        target = Path(temporary) / first.name
        if first.stat().st_size <= MISSING_SEGMENT_COPY_LIMIT:
            shutil.copyfile(first, target)
        else:
            # 硬链接与源文件同 inode；只读实现下不会写穿，源指纹随后还会再核对一次。
            os.link(first, target)
        code, stdout, _ = run(exe, ["--json", "probe", str(target)])
        if code not in VALID_EXIT_CODES:
            return f"崩溃（退出码 {code}）"
        probe = parse_json(stdout)
        if probe is None:
            return "无 JSON 输出"
        note = str(probe.get("integrity_note", ""))
        if not note:
            return "缺卷未报"
        return "ok"


def evaluate_case(exe: str, group: str, case_dir: Path, hash_contents: bool = True,
                  deep_checks: bool = True) -> CaseResult:
    sample = find_sample(case_dir)
    if sample is None:
        result = CaseResult(group, case_dir.name, "")
        result.notes = "目录中没有样本文件"
        result.failures.append("样本缺失")
        result.has_expectations = True
        return result

    # 快照先于一切读写：工具链任意环节碰了源目录都会被 diff 抓到。
    before = snapshot_tree(case_dir, hash_contents)

    result = CaseResult(group, case_dir.name, str(sample))
    expect_path = case_dir / EXPECT_NAME
    expect = {}
    if expect_path.is_file():
        try:
            expect = json.loads(expect_path.read_text(encoding="utf-8-sig"))
        except json.JSONDecodeError as error:
            result.failures.append(f"expect.json 解析失败: {error}")
            expect = {}
        result.has_expectations = True
        result.notes = str(expect.get("notes", ""))

    code, stdout, stderr = run(exe, ["--json", "probe", str(sample)])
    result.probe_exit = code
    probe = parse_json(stdout)
    if probe is None:
        result.failures.append(
            "probe 未输出 JSON（退出码 %d）: %s" % (code, (stderr or stdout).strip()[:200]))
    else:
        result.image_format = str(probe.get("format", ""))
        result.integrity_note = str(probe.get("integrity_note", ""))
        locations = probe.get("locations") or []
        labels = []
        readable_any = False
        for location in locations:
            filesystem = str(location.get("filesystem", ""))
            label = str(location.get("label", "?"))
            labels.append(f"{label}={filesystem}")
            if location.get("readable"):
                readable_any = True
        result.filesystem = "; ".join(labels)
        result.readable = readable_any

    code, stdout, stderr = run(exe, ["--json", "list", str(sample), "/"])
    listing = parse_json(stdout)
    if code == 0 and listing is not None and "entries" in listing:
        result.list_status = "ok"
        result.list_entries = len(listing.get("entries") or [])
    else:
        result.list_status = "error"
        message = ""
        if isinstance(listing, dict):
            message = str(listing.get("error", {}).get("message", ""))
        result.list_detail = (message or (stderr or stdout).strip())[:180]

    if "extract" in expect:
        with tempfile.TemporaryDirectory(prefix="uife-compat-") as temporary:
            output_root = Path(temporary) / "out"
            manifest = output_root / "SHA256SUMS"
            code, stdout, stderr = run(exe, [
                "--json", "extract", str(sample), "/",
                "-o", str(output_root), "--manifest", str(manifest)])
            listing = parse_json(stdout)
            if code == 0:
                result.extract_status = "ok"
                for problem in verify_manifest(manifest, output_root):
                    result.failures.append(problem)
            else:
                result.extract_status = "error"
                if isinstance(listing, dict):
                    result.extract_detail = str(
                        listing.get("error", {}).get("message", ""))[:180]

    if deep_checks:
        result.truncated_probe = probe_truncated(exe, sample)
        result.missing_segment = probe_missing_segment(exe, case_dir)

    after = snapshot_tree(case_dir, hash_contents)
    result.source_changes = diff_snapshot(before, after)
    result.source_unchanged = not result.source_changes
    for change in result.source_changes:
        result.failures.append(f"源文件被改动: {change}")
    if result.truncated_probe not in ("", "ok", "skipped"):
        result.failures.append(f"截断镜像探测异常: {result.truncated_probe}")
    if result.missing_segment not in ("", "ok"):
        result.failures.append(f"缺分卷探测异常: {result.missing_segment}")

    check_expectations(result, expect)
    return result


def check_expectations(result: CaseResult, expect: dict) -> None:
    if "format_contains" in expect:
        wanted = str(expect["format_contains"])
        if wanted not in result.image_format:
            result.failures.append(
                f"镜像格式不匹配：期望包含 {wanted!r}，实际 {result.image_format!r}")
    if "filesystem_contains" in expect:
        wanted = str(expect["filesystem_contains"])
        if wanted not in result.filesystem:
            result.failures.append(
                f"文件系统不匹配：期望包含 {wanted!r}，实际 {result.filesystem!r}")
    if "readable" in expect and bool(expect["readable"]) != result.readable:
        result.failures.append(
            f"可浏览状态不符：期望 {bool(expect['readable'])}，实际 {result.readable}")
    if "probe_exit" in expect and int(expect["probe_exit"]) != result.probe_exit:
        result.failures.append(
            f"probe 退出码不符：期望 {expect['probe_exit']}，实际 {result.probe_exit}")
    if "integrity_contains" in expect:
        wanted = str(expect["integrity_contains"])
        if wanted not in result.integrity_note:
            result.failures.append(
                f"完整性提示不符：期望包含 {wanted!r}，实际 {result.integrity_note!r}")
    if "integrity_absent" in expect and bool(expect["integrity_absent"]):
        if result.integrity_note:
            result.failures.append(
                f"完整镜像不应有完整性提示：{result.integrity_note!r}")
    if "list" in expect and str(expect["list"]) != result.list_status:
        result.failures.append(
            f"list 结果不符：期望 {expect['list']}，实际 {result.list_status}"
            + (f"（{result.list_detail}）" if result.list_detail else ""))
    if "extract" in expect and str(expect["extract"]) != result.extract_status:
        result.failures.append(
            f"extract 结果不符：期望 {expect['extract']}，实际 {result.extract_status}"
            + (f"（{result.extract_detail}）" if result.extract_detail else ""))


def cell(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ") if value else "—"


def write_reports(results: list[CaseResult], report: Path, json_path: Path) -> None:
    expected_cases = [item for item in results if item.has_expectations]
    failed = [item for item in expected_cases if item.failures]
    lines = [
        "# UIFE 样本兼容性矩阵",
        "",
        f"- 样本总数: {len(results)}",
        f"- 含期望值: {len(expected_cases)}",
        f"- 通过: {len(expected_cases) - len(failed)}",
        f"- 失败: {len(failed)}",
        "",
        "| 分组 | 用例 | 镜像格式 | 文件系统（位置=识别结果） | 可浏览 | list | extract | 源文件 | 截断 | 缺卷 | 结果 | 备注 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for item in results:
        verdict = "未设期望"
        if item.has_expectations:
            verdict = "失败: " + "; ".join(item.failures) if item.failures else "通过"
        lines.append("| " + " | ".join([
            cell(item.group),
            cell(item.name),
            cell(item.image_format),
            cell(item.filesystem),
            "是" if item.readable else "否",
            cell(item.list_status or "—"),
            cell(item.extract_status or "—"),
            "未变" if item.source_unchanged else "已改",
            cell(item.truncated_probe or "—"),
            cell(item.missing_segment or "—"),
            cell(verdict),
            cell(item.notes),
        ]) + " |")
    lines.append("")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(lines), encoding="utf-8")

    payload = {
        "cases": len(results),
        "expected": len(expected_cases),
        "failed": len(failed),
        "sources_modified": sum(1 for item in results if not item.source_unchanged),
        "results": [
            {
                "group": item.group,
                "case": item.name,
                "sample": item.sample,
                "format": item.image_format,
                "filesystem": item.filesystem,
                "readable": item.readable,
                "probe_exit": item.probe_exit,
                "list": item.list_status,
                "list_entries": item.list_entries,
                "extract": item.extract_status,
                "integrity_note": item.integrity_note,
                "source_unchanged": item.source_unchanged,
                "source_changes": item.source_changes,
                "truncated_probe": item.truncated_probe,
                "missing_segment": item.missing_segment,
                "expectations": item.has_expectations,
                "failures": item.failures,
                "notes": item.notes,
            }
            for item in results
        ],
    }
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="UIFE 样本兼容性矩阵")
    parser.add_argument("--exe", required=True, help="uife 可执行文件")
    parser.add_argument("--corpus", required=True, help="样本语料根目录")
    parser.add_argument("--report", required=True, help="Markdown 报告输出路径")
    parser.add_argument("--json", required=True, dest="json_report",
                        help="JSON 报告输出路径")
    parser.add_argument("--only", default="", help="只运行名称包含该子串的用例")
    parser.add_argument("--no-source-hash", action="store_true",
                        help="只比对文件清单/大小/修改时间，不逐字节哈希（超大语料用）")
    parser.add_argument("--no-deep-checks", action="store_true",
                        help="跳过截断镜像与缺分卷预检")
    arguments = parser.parse_args()

    exe = str(Path(arguments.exe).resolve())
    corpus = Path(arguments.corpus)
    if not corpus.is_dir():
        print(f"语料目录不存在: {corpus}", file=sys.stderr)
        return 2

    case_dirs: list[tuple[str, Path]] = []
    for group_dir in sorted(path for path in corpus.iterdir() if path.is_dir()):
        for case_dir in sorted(path for path in group_dir.iterdir() if path.is_dir()):
            if arguments.only and arguments.only not in str(case_dir):
                continue
            case_dirs.append((group_dir.name, case_dir))
    if not case_dirs:
        print(f"语料目录中没有 <group>/<case>/ 结构: {corpus}", file=sys.stderr)
        return 2

    results = [evaluate_case(exe, group, directory,
                             hash_contents=not arguments.no_source_hash,
                             deep_checks=not arguments.no_deep_checks)
               for group, directory in case_dirs]
    write_reports(results, Path(arguments.report), Path(arguments.json_report))

    for item in results:
        status = "未设期望"
        if item.has_expectations:
            status = "失败" if item.failures else "通过"
        print(f"[{status}] {item.group}/{item.name}: {item.image_format} | "
              f"{item.filesystem or '未识别'} | readable={item.readable} | "
              f"list={item.list_status} | 源文件={'未变' if item.source_unchanged else '已改'}")
        for failure in item.failures:
            print(f"    - {failure}")

    failed = [item for item in results if item.has_expectations and item.failures]
    modified = [item for item in results if not item.source_unchanged]
    print(f"\n共 {len(results)} 个样本，{len(failed)} 个失败，"
          f"{len(modified)} 个源文件被改动；报告: {arguments.report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
