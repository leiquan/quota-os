#!/usr/bin/env python3
"""Drive the serial menu on 128MB and 64MB guests and write docs/demo-output.txt."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_serial import Session  # noqa: E402
from menu_regress import BIG, SMALL  # noqa: E402  same steps as the regression

HEADER = """# Quota OS 演示实录

下面两段都是用 tools/capture_demos.py 驱动 qemu-system-aarch64 实际跑出来的串口记录，不是手写的预期输出。
计时来自客户机的通用定时器，不是脚本填的数字。

命令：

    qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M -nographic -nic none -kernel build/quotaos.bin
    qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 64M -nographic -nic none -kernel build/quotaos.bin

"""


def section(title, text):
    bar = "=" * 80
    return "%s\n%s\n%s\n%s\n" % (bar, title, bar, text)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/quotaos.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/demo-output.txt"
    results = []
    for title, mem, steps in (("场景 A：128MB（普通演示机）", "128M", BIG),
                              ("场景 B：64MB（-m 64M，模拟旧设备）", "64M", SMALL)):
        s = Session(kernel, mem)
        ok = s.run(steps)
        results.append((title, ok, s.close()))
        print(mem, "ok" if ok else "FAILED", flush=True)
    with open(out, "w") as f:
        f.write(HEADER)
        for title, _, text in results:
            f.write(section(title, text))
    print("wrote", out)
    if not all(ok for _, ok, _ in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
