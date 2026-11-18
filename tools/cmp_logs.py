#!/usr/bin/env python3
"""两份日志比两次：(1) 逐字比较（unified diff）；(2) 把所有数字换成 # 再比。

用法: python3 tools/cmp_logs.py OLD NEW
两部分分开写，各自给出改动行数；不做其他归一化（不合并空白、不跳过任何行）。
"""
import difflib
import re
import sys


def section(title, a, b, na, nb):
    d = list(difflib.unified_diff(a, b, na, nb, lineterm="", n=0))
    minus = sum(1 for l in d if l.startswith("-") and not l.startswith("---"))
    plus = sum(1 for l in d if l.startswith("+") and not l.startswith("+++"))
    print("=== %s: %s ===" % (title, "IDENTICAL" if not d else
                               "%d lines removed, %d lines added" % (minus, plus)))
    for l in d:
        print(l)
    print()


def main(old, new):
    a = open(old, errors="replace").read().splitlines()
    b = open(new, errors="replace").read().splitlines()
    section("1 verbatim diff", a, b, old, new)
    m = lambda xs: [re.sub(r"\d+", "#", x) for x in xs]
    section("2 numbers-masked diff (every digit run -> #)", m(a), m(b), old, new)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
