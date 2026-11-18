#!/usr/bin/env python3
"""Boot the kernel on several QEMU interrupt-controller setups and check it reaches select>.

  python3 tools/gic_boot_check.py <kernel.bin> <outdir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_serial import Session  # noqa: E402

CASES = [
    ("gicv2", "virt,gic-version=2", []),
    ("gicv3", "virt,gic-version=3", []),
    ("gicv2-el2", "virt,gic-version=2,virtualization=on", []),
    ("gicv3-el2", "virt,gic-version=3,virtualization=on", []),
    ("gic-max", "virt,gic-version=max", []),
    ("virt-default", "virt", []),
    ("gicv3-smp2", "virt,gic-version=3", ["-smp", "2"]),
]


def main():
    kernel, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    lines = []
    for name, machine, extra in CASES:
        s = Session(kernel, "128M", extra, machine=machine)
        ok = s.run([("1\n", "select> ", 20)])
        text = s.close()
        open(os.path.join(outdir, "boot-%s.txt" % name), "w").write(
            "# cmd: %s\n%s" % (" ".join(s.cmd), text))
        gic = next((l for l in text.splitlines() if "GIC" in l and "[boot]" in l), "-")
        panic = "PANIC" in text or "ERROR" in text
        line = "%-13s %-40s select>=%s panic/error=%s  %s" % (
            name, machine + (" " + " ".join(extra) if extra else ""), ok, panic, gic.strip())
        lines.append(line)
        print(line, flush=True)
    open(os.path.join(outdir, "summary.txt"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
