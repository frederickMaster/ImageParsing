#!/usr/bin/env python3
"""校验 `uife capabilities --json` 的结构与自洽性。

这个命令存在的意义是"让二进制自己申报"，所以它本身也会腐烂：字段漏了、
重名了、把"仅识别"的格式也算进"可读"，编译器不会报错，但依赖它的版本
兼容表就成了假的。这里逐条把能自动查的都查掉。

用法：
    python tests/capabilities_test.py --exe build/uife.exe
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

EXIT_CODES = {
    "success": 0,
    "runtime_error": 1,
    "usage_error": 2,
    "driver_missing": 3,
    "fs_parser_missing": 4,
    "resource_limit": 5,
}

problems: list[str] = []


def fail(message: str) -> None:
    problems.append(message)


def check(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def run(exe: str, args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run([exe, *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def validate_names(key: str, values: object, allow_empty: bool = False) -> None:
    check(isinstance(values, list), f"{key} 必须是数组")
    if not isinstance(values, list):
        return
    check(all(isinstance(item, str) and item for item in values),
          f"{key} 里必须都是非空字符串")
    if not allow_empty:
        check(bool(values), f"{key} 不能为空")
    check(len(values) == len(set(values)), f"{key} 里有重复项")


def validate_booleans(key: str, values: object, required: set[str]) -> None:
    check(isinstance(values, dict), f"{key} 必须是对象")
    if not isinstance(values, dict):
        return
    for name, value in values.items():
        check(isinstance(value, bool), f"{key}.{name} 必须是布尔值")
    missing = required - set(values)
    check(not missing, f"{key} 缺少字段: {sorted(missing)}")


def validate(exe: str) -> dict[str, object] | None:
    result = run(exe, ["capabilities", "--json"])
    check(result.returncode == 0,
          f"capabilities --json 退出码 {result.returncode}: {result.stderr[:200]}")
    if result.returncode != 0:
        return None

    stdout = result.stdout.strip()
    # batch 逐行消费 JSON，多一行日志就会破坏解析；这里要求整个 stdout 就是一个对象。
    check(stdout.count("\n") == 0, "capabilities --json 必须只输出一行 JSON")
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as error:
        fail(f"输出不是合法 JSON: {error}")
        return None
    if not isinstance(payload, dict):
        fail("capabilities --json 的顶层必须是对象")
        return None

    check(payload.get("tool") == "uife", "tool 字段必须是 uife")
    version = payload.get("version")
    check(isinstance(version, str) and
          (version == "unknown" or re.fullmatch(r"\d+\.\d+\.\d+", version) is not None),
          f"version 格式不对: {version!r}")
    build = payload.get("build")
    check(isinstance(build, dict) and isinstance(build.get("compiler"), str) and
          isinstance(build.get("type"), str), "build 必须包含 compiler 与 type 字符串")

    formats = payload.get("image_formats")
    check(isinstance(formats, dict), "image_formats 必须是对象")
    if isinstance(formats, dict):
        validate_names("image_formats.supported", formats.get("supported"))
        validate_names("image_formats.recognized_only",
                       formats.get("recognized_only"), allow_empty=True)
        supported = set(formats.get("supported") or [])
        recognized = set(formats.get("recognized_only") or [])
        overlap = supported & recognized
        # 同一个格式既"可读"又"仅识别"说明注册表里有两条同名登记，
        # 或者这里的口径写错了——两种都要在 CI 上拦下来。
        check(not overlap, f"格式同时出现在 supported 与 recognized_only: {sorted(overlap)}")

    validate_names("filesystems", payload.get("filesystems"))

    features = payload.get("features")
    validate_booleans("features", features, {"resource_limits", "operation_log"})
    notes = payload.get("feature_notes")
    check(isinstance(notes, dict), "feature_notes 必须是对象")
    if isinstance(features, dict) and isinstance(notes, dict):
        check(set(features) == set(notes),
              "features 与 feature_notes 的键必须一一对应")
        for name, note in notes.items():
            check(isinstance(note, str) and note.strip(),
                  f"feature_notes.{name} 必须是非空字符串")

    backends = payload.get("backends")
    validate_booleans("backends", backends, {"raw", "zlib"})
    if isinstance(backends, dict):
        # raw 与 zlib 是核心依赖，恒真；否则这个字段本身就不可信了。
        for name in ("raw", "zlib"):
            check(backends.get(name) is True, f"backends.{name} 必须为 true")

    check(payload.get("exit_codes") == EXIT_CODES,
          f"exit_codes 与文档约定不一致: {payload.get('exit_codes')}")

    # 文本模式也要能跑通：人手排查时看的是它。
    text = run(exe, ["capabilities"])
    check(text.returncode == 0, "capabilities（文本模式）退出码非 0")
    check("能力:" in text.stdout and isinstance(version, str) and version in text.stdout,
          "文本模式没有输出版本号或能力段")

    # 多余参数必须被拒绝：capabilities 不读镜像，静默忽略参数会掩盖脚本写错。
    extra = run(exe, ["capabilities", "unexpected"])
    check(extra.returncode == 2,
          f"capabilities 带多余参数时应以退出码 2 失败，实际 {extra.returncode}")

    validate_about(exe, payload)

    return payload


def validate_about(exe: str, capabilities: dict[str, object]) -> None:
    """`uife about` 是 LGPL §4(c)/§6(e) 的落地，不能悄悄退化成空壳。

    这里查三件事：退出码/参数校验、每个真正启用的后端都出现在声明里、
    每条声明的许可证与许可文件名都不为空。少了哪一条，用户手里那份二进制
    就变成"用了 LGPL 库但查不到出处"。
    """
    result = run(exe, ["about"])
    check(result.returncode == 0, "about 退出码非 0")
    text = result.stdout
    check("uife " in text, "about 没有输出版本行")
    check("许可全文" in text, "about 没有指向许可证全文的位置")

    payload_text = run(exe, ["about", "--json"])
    check(payload_text.returncode == 0, "about --json 退出码非 0")
    try:
        about = json.loads(payload_text.stdout)
    except json.JSONDecodeError as error:
        fail(f"about --json 不是合法 JSON: {error}")
        return

    components = about.get("components")
    check(isinstance(components, list) and bool(components),
          "about 没有列出任何第三方组件")

    expected = {
        "ewf": "libewf",
        "wim": "wimlib",
        "apfs": "libfsapfs",
        "refs": "libfsrefs",
        "tsk": "The Sleuth Kit",
    }
    backends = capabilities.get("backends") or {}
    names = {c.get("name") for c in components if isinstance(c, dict)}
    for backend, component in expected.items():
        if backends.get(backend) is True:
            check(component in names,
                  f"后端 {backend} 已启用，但 about 里没有声明 {component}")

    for component in components:
        if not isinstance(component, dict):
            fail("about 的组件项不是对象")
            continue
        for field in ("name", "license", "copyright", "shipped_as", "license_file"):
            check(bool(component.get(field)),
                  f"about 组件 {component.get('name')} 缺少字段 {field}")

    extra = run(exe, ["about", "unexpected"])
    check(extra.returncode == 2,
          f"about 带多余参数时应以退出码 2 失败，实际 {extra.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, help="uife 可执行文件")
    arguments = parser.parse_args()

    payload = validate(arguments.exe)
    if problems:
        for problem in problems:
            print(f"[FAIL] {problem}", file=sys.stderr)
        return 1
    if payload is None:
        return 1
    supported = len(payload["image_formats"]["supported"])  # type: ignore[index]
    features = len(payload["features"])  # type: ignore[arg-type]
    print(f"capabilities OK: {supported} 种镜像格式, {features} 项能力申报")
    return 0


if __name__ == "__main__":
    sys.exit(main())
