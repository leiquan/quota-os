#!/usr/bin/env python3
"""Run the kernel's built-in acceptance sequence (no serial input) and save the log.

  python3 tools/acceptance.py <kernel.bin> <out.txt> <icount|normal> [qa|qa-long] [gic]

The kernel reads -append from /chosen/bootargs:
  qa       menus 1, 3-10 back to back (1000 periods per demo window), then menu 1
  qa-long  menu 9 load (32 tier-3 loops + tier-2 game + audio-mix) for 100000 periods
Both end with "QA DONE"; the script then kills QEMU.

icount adds `-icount shift=4,sleep=off`: one guest instruction = 16 ns = one CNTVCT
tick at 62.5 MHz, and the guest never waits on host time, so the log is a pure
function of the kernel image and the arguments.
"""
import os
import select
import subprocess
import sys
import time


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    kernel, out, mode = sys.argv[1:4]
    args = sys.argv[4] if len(sys.argv) > 4 else "qa"
    gic = sys.argv[5] if len(sys.argv) > 5 else "3"
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=" + gic, "-cpu", "cortex-a72",
           "-m", "128M", "-nographic", "-nic", "none", "-kernel", kernel, "-append", args]
    if mode == "icount":
        cmd += ["-icount", "shift=4,sleep=off"]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL)
    buf = b""
    while b"QA DONE" not in buf:
        r, _, _ = select.select([p.stdout], [], [], 1)
        if r:
            chunk = os.read(p.stdout.fileno(), 65536)
            if not chunk:
                break
            buf += chunk
        if time.time() - t0 > 7200:
            break
    p.kill()
    p.wait()
    done = b"QA DONE" in buf
    if done:
        # 读到 "QA DONE" 就停；它后面的换行可能还没到，统一截在这里。
        buf = buf[:buf.index(b"QA DONE") + len(b"QA DONE")] + b"\n"
    with open(out, "wb") as f:
        f.write(buf.replace(b"\r\n", b"\n"))
    print("%s: %s, %.0f s host time, cmd: %s" % (out, "done" if done else "INCOMPLETE",
                                                  time.time() - t0, " ".join(cmd)))
    sys.exit(0 if done else 1)


if __name__ == "__main__":
    main()
