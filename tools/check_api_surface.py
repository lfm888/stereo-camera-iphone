#!/usr/bin/env python3
"""check_api_surface.py — 锁定 C API 的公开符号面（名字 + 参数个数）。

为什么需要它：`stereo_c_api.h` 是 Swift / CLI / 测试唯一的契约。拆头文件、改返回码、
加重载时最容易发生的事是"悄悄多一个符号 / 少一个参数"。这个脚本把契约变成基线文件，
任何变化都必须显式更新 `core/tests/api_surface.txt` 并在 diff 里出现。

用法：
  python3 tools/check_api_surface.py                # 与基线对比，不一致就退出 1
  python3 tools/check_api_surface.py --update       # 重新生成基线（评审时必须看到 diff）
  python3 tools/check_api_surface.py --list         # 打印当前抽取结果

抽取规则（故意简单、可预测）：
  * 只看公开 C 头（默认 core/include/stereo/*.h），不看 .hpp；
  * 先去掉注释、预处理行和 `{...}` 块（头文件里不该有函数体）；
  * 按 `;` 切声明，跳过 `typedef`；
  * 取声明里第一个 `stereo_xxx(` 作为函数名，参数个数 = 最外层括号里的顶层逗号数 + 1；
    `(void)` / `()` 记为 0。

它不校验返回类型和参数类型 —— 那是"名字 + 数量"之外的事，靠 C 编译器与
`c_api_smoke.c` 的调用点兜底。
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

DEFAULT_INCLUDE = "core/include/stereo"
DEFAULT_BASELINE = "core/tests/api_surface.txt"

FUNC_RE = re.compile(r"\b(stereo_[A-Za-z0-9_]+)\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def strip_preprocessor(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def strip_decl_blocks(text: str) -> str:
    """去掉 struct / enum / union 的花括号内容，但**不碰** extern "C" { }。

    公开头里没有函数体，所以删掉类型定义的块是安全的；`extern "C"` 的块必须保留，
    否则所有函数声明都跟着消失（第一版就踩了这个坑）。
    """
    pattern = re.compile(r"\b(?:struct|enum|union)\b[^;{}]*\{[^{}]*\}")
    previous = None
    while previous != text:
        previous = text
        text = pattern.sub(" ", text)
    return text


def param_count(decl: str, open_paren: int) -> int:
    depth = 0
    commas = 0
    body = []
    for ch in decl[open_paren:]:
        if ch == "(":
            depth += 1
            if depth == 1:
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                break
        if depth == 1:
            if ch == ",":
                commas += 1
            body.append(ch)
    params = "".join(body).strip()
    if not params or params == "void":
        return 0
    return commas + 1


def extract_symbols(paths: list[str]) -> dict[str, int]:
    found: dict[str, int] = {}
    for path in paths:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        text = strip_decl_blocks(strip_preprocessor(strip_comments(text)))
        for stmt in text.split(";"):
            stmt = " ".join(stmt.split())
            if not stmt or stmt.startswith("typedef"):
                continue
            match = FUNC_RE.search(stmt)
            if match is None:
                continue
            # `stereo_xxx` 前面必须还有返回类型（排除宏/表达式/函数指针成员）。
            prefix = stmt[: match.start()].strip()
            if not prefix or "(" in prefix:
                continue
            name = match.group(1)
            count = param_count(stmt, match.end() - 1)
            previous = found.get(name)
            if previous is not None and previous != count:
                raise SystemExit(f"头文件里 {name} 的声明参数个数不一致：{previous} vs {count}")
            found[name] = count
    return dict(sorted(found.items()))


def render(symbols: dict[str, int]) -> str:
    return "".join(f"{name} {count}\n" for name, count in symbols.items())


def parse_baseline(path: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 2 or not parts[1].isdigit():
                raise SystemExit(f"{path}:{lineno}: 期望 '<name> <param_count>'，得到 {line!r}")
            symbols[parts[0]] = int(parts[1])
    return dict(sorted(symbols.items()))


def diff(current: dict[str, int], baseline: dict[str, int]) -> list[str]:
    problems: list[str] = []
    for name in sorted(current.keys() - baseline.keys()):
        problems.append(f"新增符号: {name}({current[name]} 参数) —— 确认后跑 --update")
    for name in sorted(baseline.keys() - current.keys()):
        problems.append(f"删除符号: {name}({baseline[name]} 参数) —— 确认后跑 --update")
    for name in sorted(current.keys() & baseline.keys()):
        if current[name] != baseline[name]:
            problems.append(
                f"参数个数变化: {name} {baseline[name]} -> {current[name]} —— 确认后跑 --update"
            )
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--include", default=DEFAULT_INCLUDE,
                        help=f"公开 C 头所在目录（默认 {DEFAULT_INCLUDE}）")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE,
                        help=f"基线文件（默认 {DEFAULT_BASELINE}）")
    parser.add_argument("--update", action="store_true", help="用当前抽取结果覆盖基线")
    parser.add_argument("--list", action="store_true", help="打印当前抽取结果")
    args = parser.parse_args(argv)

    headers = sorted(glob.glob(os.path.join(args.include, "*.h")))
    if not headers:
        print(f"FAIL: {args.include} 下没有 .h 文件", file=sys.stderr)
        return 1

    symbols = extract_symbols(headers)
    if not symbols:
        print(f"FAIL: 从 {len(headers)} 个头里没抽到任何 stereo_* 函数", file=sys.stderr)
        return 1

    if args.list:
        sys.stdout.write(render(symbols))
    if args.update:
        with open(args.baseline, "w", encoding="utf-8") as fh:
            fh.write("# C API 公开符号基线：<函数名> <参数个数>。由 tools/check_api_surface.py --update 生成。\n")
            fh.write(render(symbols))
        print(f"已更新 {args.baseline}（{len(symbols)} 个符号）")
        return 0
    if args.list:
        return 0

    if not os.path.exists(args.baseline):
        print(f"FAIL: 没有基线 {args.baseline}，先跑 --update 并提交", file=sys.stderr)
        return 1

    baseline = parse_baseline(args.baseline)
    problems = diff(symbols, baseline)
    if problems:
        print("FAIL: C API 表面与基线不一致：", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(f"OK: {len(symbols)} 个公开符号与 {args.baseline} 一致（{len(headers)} 个头）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
