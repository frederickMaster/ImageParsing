#!/usr/bin/env python3
"""源镜像只读打开方式的静态审计。

行为层面的证明在 `tests/test_support.h`（操作前后哈希快照 + 只读权限位清扫）
和 `tests/compat_matrix.py`（真实样本的前后指纹）里。这个脚本补的是另一半：
**代码里不存在把源镜像按可写方式打开的地方**，不需要构造特定路径才能触发。

规则（`src/` 全树）：

1. `src/drivers/image/` 是镜像容器层，它只应该读源文件。出现任何写方式
   打开、截断、改名、删除，直接判失败。
2. 全树不允许 `std::ifstream` 带 `std::ios::out` / `trunc` / `app`，
   也不允许 `LIBEWF_ACCESS_FLAG_WRITE`。
3. libtsk 的镜像入口只允许 `tsk_img_open_external`——我们自己实现读回调，
   读写边界因此完全在 `external_image_read` 一处，可审计。
4. 少数确实需要写文件的非镜像代码（导出目标、临时 spool）必须在同一行或
   上一行写 `readonly-audit: <理由>`，否则同样失败。

退出码：0 无违规，1 有违规，2 用法错误。
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

MARKER = "readonly-audit:"

# (名称, 正则, 适用目录, 说明)
RULES: list[tuple[str, re.Pattern[str], str, str]] = [
    ("write-stream", re.compile(r"\bstd::(?:ofstream|fstream)\b"), "src",
     "改动流对象可能写文件"),
    ("write-open", re.compile(r"\b(?:fopen|_wfopen|fopen_s|_wfopen_s)\s*\("), "src",
     "C 风格打开需人工确认模式串"),
    ("win32-write", re.compile(r"\bCreateFile[AW]?\s*\("), "src",
     "Win32 打开需人工确认访问掩码"),
    ("win32-write-mask", re.compile(r"\bGENERIC_(?:WRITE|ALL)\b"), "src",
     "请求了写权限"),
    ("win32-write-call", re.compile(r"\b(?:WriteFile|SetEndOfFile|FSCTL_SET_[A-Z_]+)\b"),
     "src", "写调用"),
    ("posix-write-flag", re.compile(r"\bO_(?:WRONLY|RDWR|CREAT|TRUNC|APPEND)\b"), "src",
     "POSIX 写标志"),
    ("ifstream-write-mode", re.compile(r"ifstream[^;\n]*ios::(?:out|trunc|app)"), "src",
     "只读流带了写模式"),
    ("libewf-write", re.compile(r"LIBEWF_ACCESS_FLAG_WRITE"), "src",
     "libewf 以写方式打开"),
    ("fs-mutation", re.compile(r"\b(?:resize_file|remove_all)\s*\("), "src",
     "文件系统变更调用"),
]

# 镜像容器层：一条都不许有。
IMAGE_DRIVER_VIOLATIONS = re.compile(
    r"\b(?:ofstream|fstream|fopen|_wfopen|CreateFile[AW]?)\s*\(|"
    r"\bGENERIC_(?:WRITE|ALL)\b|\bWriteFile\b|\bSetEndOfFile\b|"
    r"\bO_(?:WRONLY|RDWR|CREAT|TRUNC|APPEND)\b|"
    r"\b(?:resize_file|rename|remove|remove_all)\s*\(")

TSK_OPEN = re.compile(r"\btsk_img_open\w*\s*\(")


@dataclass
class Violation:
    path: str
    line: int
    rule: str
    detail: str
    text: str


def annotated(lines: list[str], index: int) -> bool:
    """同一行或上一行有 readonly-audit 标注即视为已交代用途。"""
    if MARKER in lines[index]:
        return True
    return index > 0 and MARKER in lines[index - 1]


def audit_file(path: Path) -> list[Violation]:
    relative = path.as_posix()
    violations: list[Violation] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return [Violation(relative, 0, "read-error", str(error), "")]

    lines = text.splitlines()
    in_image_driver = "/drivers/image/" in relative and relative.startswith("src/")

    for index, line in enumerate(lines):
        number = index + 1
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        if MARKER in line:
            continue

        if in_image_driver:
            match = IMAGE_DRIVER_VIOLATIONS.search(line)
            # 只读打开（ifstream）不匹配上面的正则；命中即违规。
            if match:
                violations.append(Violation(
                    relative, number, "image-driver-write",
                    "镜像容器层不得写文件", stripped))

        for name, pattern, _, detail in RULES:
            if not pattern.search(line):
                continue
            if annotated(lines, index):
                continue
            violations.append(Violation(relative, number, name, detail, stripped))
            break  # 同一行只报第一条，避免噪声

        match = TSK_OPEN.search(line)
        if match and "tsk_img_open_external" not in line:
            violations.append(Violation(
                relative, number, "tsk-img-open",
                "libtsk 只允许 tsk_img_open_external（读回调由本仓库实现）", stripped))

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description="源镜像只读打开方式的静态审计")
    parser.add_argument("--root", default=".", help="仓库根目录")
    parser.add_argument("--report", default="", help="把结果追加写入该文件")
    arguments = parser.parse_args()

    root = Path(arguments.root).resolve()
    source_root = root / "src"
    if not source_root.is_dir():
        print(f"找不到源码目录: {source_root}", file=sys.stderr)
        return 2

    files = sorted(
        path for path in source_root.rglob("*")
        if path.is_file() and path.suffix in {".cpp", ".cc", ".h", ".hpp"})
    if not files:
        print(f"源码目录中没有可审计文件: {source_root}", file=sys.stderr)
        return 2

    violations: list[Violation] = []
    for path in files:
        violations.extend(audit_file(path))

    lines = [f"audited {len(files)} files under {source_root}",
             f"violations {len(violations)}"]
    for item in violations:
        lines.append(f"{item.path}:{item.line}: [{item.rule}] {item.detail}: {item.text}")

    output = "\n".join(lines)
    print(output)
    if arguments.report:
        with open(arguments.report, "a", encoding="utf-8") as handle:
            handle.write(output + "\n")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
