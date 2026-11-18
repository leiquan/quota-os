#!/usr/bin/env python3
"""Bounded e4 run. Streams serial to a file. SIGKILL at the deadline.

Exit 0 only when the kernel itself printed all of:
  E4 item9 PASS
  E4 item10 PASS
  QA DONE
A log cut off by the deadline is not a pass.

  python3 tools/e4_bound.py <kernel> <logfile> <seconds> [icount|normal]

QEMU is  the caller's qemu-system-aarch64. Machine line is fixed:
  -machine virt,gic-version=2 -cpu cortex-a72 -m 128M
  -nographic -nic none -append e4
icount adds -icount shift=4,sleep=off
"""
import os
import select
import signal
import subprocess
import sys
import time


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    kernel, out, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3])
    mode = sys.argv[4] if len(sys.argv) > 4 else "icount"
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "128M",
        "-nographic", "-nic", "none",
        "-kernel", kernel,
        "-append", "e4",
    ]
    if mode == "icount":
        cmd += ["-icount", "shift=4,sleep=off"]
    elif mode != "normal":
        sys.exit("mode must be icount or normal")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    log = open(out, "wb")
    meta = out + ".cmd"
    open(meta, "w").write(
        "timeout_s: %d\nkill: SIGKILL\nmode: %s\nkernel: %s\ncommand:\n%s\n"
        % (seconds, mode, kernel, " ".join(cmd))
    )
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, start_new_session=True,
    )
    raw = b""
    t0 = time.time()
    reason = "exited"
    while True:
        elapsed = time.time() - t0
        if elapsed >= seconds:
            os.killpg(proc.pid, signal.SIGKILL)
            reason = "SIGKILL at %ds" % seconds
            break
        r, _, _ = select.select([proc.stdout], [], [], 0.5)
        if r:
            chunk = os.read(proc.stdout.fileno(), 65536)
            if chunk:
                raw += chunk
                log.write(chunk)
                log.flush()
        if b"QA DONE" in raw:
            time.sleep(0.2)
            r, _, _ = select.select([proc.stdout], [], [], 0.2)
            if r:
                chunk = os.read(proc.stdout.fileno(), 65536)
                if chunk:
                    raw += chunk
                    log.write(chunk)
                    log.flush()
            os.killpg(proc.pid, signal.SIGKILL)
            reason = "saw QA DONE; SIGKILL at %.1fs" % elapsed
            break
        if proc.poll() is not None:
            while True:
                r, _, _ = select.select([proc.stdout], [], [], 0.1)
                if not r:
                    break
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    break
                raw += chunk
                log.write(chunk)
                log.flush()
            reason = "qemu exited %s" % proc.returncode
            break
        if int(elapsed) % 15 == 0 and not r:
            tail = raw.decode("utf-8", "replace").splitlines()
            last = tail[-1] if tail else ""
            print("t=%ds bytes=%d %s" % (int(elapsed), len(raw), last[:100]), flush=True)
    proc.wait(timeout=5)
    log.close()
    text = raw.decode("utf-8", "replace")
    ok = ("E4 item9 PASS" in text and "E4 item10 PASS" in text and "QA DONE" in text)
    print("%s %s bytes=%d item9=%s item10=%s qa=%s -> %s" % (
        out, reason, len(raw),
        "PASS" if "E4 item9 PASS" in text else ("FAIL" if "E4 item9 FAIL" in text else "absent"),
        "PASS" if "E4 item10 PASS" in text else ("FAIL" if "E4 item10 FAIL" in text else "absent"),
        "yes" if "QA DONE" in text else "no",
        "COMPLETE" if ok else "NOT-PASS",
    ), flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
