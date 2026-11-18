#!/usr/bin/env python3
"""Click tier-3 load on the LVGL home and wait for menu 9.

Same devices and empty boot arguments for every round. Host clock is the
default. Pass icount to add -icount shift=4,sleep=off.

  python3 tools/run-gui-stress.py <kernel> <logfile> [host|icount] [shot.ppm]
"""
import json
import os
import socket
import subprocess
import sys
import time

BIN = sys.argv[1]
LOG = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "host"
SHOT = sys.argv[4] if len(sys.argv) > 4 else ""
QMP = "/tmp/gui-stress-qmp.sock"


def qmp_open():
    for _ in range(80):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(QMP)
            s.settimeout(2)
            return s
        except OSError:
            time.sleep(0.05)
    raise SystemExit("qmp did not open")


def qmp_read(s):
    buf = b""
    while b"\n" not in buf:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    return buf.decode("utf-8", "replace")


def qmp(s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    return qmp_read(s)


def main():
    if os.path.exists(QMP):
        os.unlink(QMP)
    open(LOG, "w").close()
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "128M",
        "-display", "none",
        "-nic", "none",
        "-device", "ramfb",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-kernel", BIN,
        "-serial", "file:" + LOG,
        "-qmp", "unix:" + QMP + ",server,nowait",
    ]
    if MODE == "icount":
        cmd += ["-icount", "shift=4,sleep=off"]
    elif MODE != "host":
        raise SystemExit("mode must be host or icount")
    open(LOG + ".cmd", "w").write(" ".join(cmd) + "\n")
    proc = subprocess.Popen(cmd)
    s = qmp_open()
    qmp_read(s)
    qmp(s, {"execute": "qmp_capabilities"})
    ready_s = 60 if MODE == "icount" else 15
    deadline = time.time() + ready_s
    ready = False
    while time.time() < deadline:
        text = open(LOG, "r", errors="replace").read()
        if "[lvgl] ready" in text and "load9 box" in text:
            ready = True
            break
        time.sleep(0.05)
    if not ready:
        proc.kill()
        print("not ready")
        return 1

    def box(name, text):
        key = "[lvgl] " + name + " box "
        for line in text.splitlines():
            if line.startswith(key):
                parts = line[len(key):].replace("x", " ").replace(",", " ").split()
                x, y, w, h = [int(p) for p in parts[:4]]
                return x + w // 2, y + h // 2
        return None

    text = open(LOG, "r", errors="replace").read()
    load9 = box("load9", text)
    print("load9", load9, "mode", MODE)
    if not load9:
        proc.kill()
        return 1
    px, py = load9

    def send(events):
        qmp(s, {"execute": "input-send-event", "arguments": {"events": events}})

    send([
        {"type": "abs", "data": {"axis": "x", "value": px * 32767 // 639}},
        {"type": "abs", "data": {"axis": "y", "value": py * 32767 // 479}},
    ])
    time.sleep(0.25)
    qmp(s, {"execute": "human-monitor-command",
            "arguments": {"command-line": "mouse_button 1"}})
    time.sleep(0.2)
    qmp(s, {"execute": "human-monitor-command",
            "arguments": {"command-line": "mouse_button 0"}})
    wait_s = 90 if MODE == "icount" else 25
    end = time.time() + wait_s
    seen = False
    while time.time() < end:
        text = open(LOG, "r", errors="replace").read()
        if "[menu] done 9" in text:
            seen = True
            break
        time.sleep(0.1)
    time.sleep(0.5)
    if SHOT:
        qmp(s, {"execute": "screendump", "arguments": {"filename": SHOT}})
    proc.kill()
    proc.wait(timeout=5)
    text = open(LOG, "r", errors="replace").read()
    print("done9", seen, "panic", "PANIC" in text, "bytes", len(text))
    return 0 if seen else 2


if __name__ == "__main__":
    sys.exit(main())
