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
      "notes": "真实样本来源说明"
    }

未提供 expect.json 的样本按“仅记录”处理，不影响退出码；提供期望值但
不匹配的样本计入失败，脚本以退出码 1 结束，可直接作为 CTest 用例。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

EXPECT_NAME = "expect.json"


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


def evaluate_case(exe: str, group: str, case_dir: Path) -> CaseResult:
    sample = find_sample(case_dir)
    if sample is None:
        result = CaseResult(group, case_dir.name, "")
        result.notes = "目录中没有样本文件"
        result.failures.append("样本缺失")
        result.has_expectations = True
        return result

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
        "| 分组 | 用例 | 镜像格式 | 文件系统（位置=识别结果） | 可浏览 | list | extract | 结果 | 备注 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
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

    results = [evaluate_case(exe, group, directory) for group, directory in case_dirs]
    write_reports(results, Path(arguments.report), Path(arguments.json_report))

    for item in results:
        status = "未设期望"
        if item.has_expectations:
            status = "失败" if item.failures else "通过"
        print(f"[{status}] {item.group}/{item.name}: {item.image_format} | "
              f"{item.filesystem or '未识别'} | readable={item.readable} | "
              f"list={item.list_status}")
        for failure in item.failures:
            print(f"    - {failure}")

    failed = [item for item in results if item.has_expectations and item.failures]
    print(f"\n共 {len(results)} 个样本，{len(failed)} 个失败；报告: "
          f"{arguments.report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
