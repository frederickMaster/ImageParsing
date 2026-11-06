#!/usr/bin/env python3
"""从各发行版的 `uife capabilities --json` 生成版本兼容表。

版本兼容表最容易腐烂的地方是"人抄"：发版时漏改一行，客户拿到的表和手里的
二进制就不是一回事。这里改成从二进制里读——每发一个版本就把它的
capabilities 结果加一列，表就是事实本身。

用法：
    python scripts/capability-matrix.py \\
        --build "0.1.0 windows-x64=<path>/uife.exe" \\
        --output docs/COMPATIBILITY.md

`--build` 可重复；标签里的第一段当作版本号，其余当作平台描述。
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass


@dataclass
class Build:
    label: str
    path: str
    payload: dict


def load_build(label: str, path: str) -> Build:
    result = subprocess.run([path, "capabilities", "--json"],
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    if result.returncode != 0:
        raise SystemExit(f"{label}: capabilities 调用失败（退出码 {result.returncode}）: "
                         f"{result.stderr.strip()[:200]}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(f"{label}: 输出不是合法 JSON: {error}") from error
    return Build(label=label, path=path, payload=payload)


def cell(text: str) -> str:
    return text.replace("|", "\\|")


def table(header: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |",
             "| " + " | ".join("---" for _ in header) + " |"]
    for row in rows:
        lines.append("| " + " | ".join(cell(item) for item in row) + " |")
    return lines


def membership(builds: list[Build], extract) -> list[list[str]]:
    """把所有版本出现过的条目取并集，逐个标出哪些版本里有。"""
    names: list[str] = []
    for build in builds:
        for name in extract(build.payload):
            if name not in names:
                names.append(name)
    rows = []
    for name in names:
        rows.append([name] + ["有" if name in extract(build.payload) else "—"
                              for build in builds])
    return rows


def flag_rows(builds: list[Build], key: str) -> list[list[str]]:
    names: list[str] = []
    for build in builds:
        for name in build.payload.get(key, {}):
            if name not in names:
                names.append(name)
    rows = []
    for name in names:
        values = []
        for build in builds:
            value = build.payload.get(key, {}).get(name)
            values.append("是" if value is True else "否" if value is False else "—")
        rows.append([name] + values)
    return rows


def render(builds: list[Build]) -> str:
    def header(first: str) -> list[str]:
        return [first] + [build.label for build in builds]

    lines: list[str] = []

    lines.append("| 项目 | " + " | ".join(cell(b.label) for b in builds) + " |")
    lines.append("| --- | " + " | ".join("---" for _ in builds) + " |")
    lines.append("| 版本 | " + " | ".join(
        cell(f"{b.payload.get('version', '?')}（{b.payload.get('build', {}).get('type', '?')}）")
        for b in builds) + " |")
    lines.append("| 编译器 | " + " | ".join(
        cell(str(b.payload.get("build", {}).get("compiler", "?"))) for b in builds) + " |")
    lines.append("| 镜像格式数 | " + " | ".join(
        str(len(b.payload.get("image_formats", {}).get("supported", []))) for b in builds) + " |")
    lines.append("| 文件系统解析器数 | " + " | ".join(
        str(len(b.payload.get("filesystems", []))) for b in builds) + " |")
    lines.append("")

    lines.append("### 镜像格式")
    lines.append("")
    lines += table(header("格式"), membership(
        builds, lambda p: p.get("image_formats", {}).get("supported", [])))
    lines.append("")

    recognized = [b for b in builds
                  if b.payload.get("image_formats", {}).get("recognized_only")]
    if recognized:
        lines.append("### 仅识别、没有读取驱动的格式")
        lines.append("")
        lines += table(header("格式"), membership(
            builds, lambda p: p.get("image_formats", {}).get("recognized_only", [])))
        lines.append("")

    lines.append("### 文件系统解析器")
    lines.append("")
    lines += table(header("解析器"), membership(builds, lambda p: p.get("filesystems", [])))
    lines.append("")

    lines.append("### 后端")
    lines.append("")
    lines += table(header("后端"), flag_rows(builds, "backends"))
    lines.append("")

    lines.append("### 能力")
    lines.append("")
    lines += table(header("能力"), flag_rows(builds, "features"))
    lines.append("")

    notes: list[tuple[str, str]] = []
    for build in builds:
        for name, note in build.payload.get("feature_notes", {}).items():
            if all(name != existing for existing, _ in notes):
                notes.append((name, note))
    if notes:
        lines.append("### 能力说明")
        lines.append("")
        lines += table(["能力", "含义"], [[name, note] for name, note in notes])
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="append", default=[],
                        metavar="LABEL=PATH",
                        help="一个构建，可重复：标签=可执行文件路径")
    parser.add_argument("--output", default="", help="写入文件，默认输出到 stdout")
    arguments = parser.parse_args()

    if not arguments.build:
        parser.error("至少需要一个 --build 标签=路径")

    builds: list[Build] = []
    for spec in arguments.build:
        label, separator, path = spec.rpartition("=")
        if not separator or not label or not path:
            parser.error(f"--build 需要 LABEL=PATH 形式: {spec}")
        builds.append(load_build(label, path))

    document = render(builds)
    if arguments.output:
        with open(arguments.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(document)
        print(f"已写入 {arguments.output}（{len(builds)} 个构建）")
    else:
        sys.stdout.write(document)
    return 0


if __name__ == "__main__":
    sys.exit(main())
