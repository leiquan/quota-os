#!/usr/bin/env python3
"""Boot the LVGL image on QEMU virt with ramfb and virtio input, click +1, type ab."""
import json
import os
import socket
import struct
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/quotaos-lvgl.bin"
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/lvgl-trial.log"
APPEND = sys.argv[3] if len(sys.argv) > 3 else ""
QMP = "/tmp/lvgl-qmp.sock"
RAW = "/tmp/lvgl-fb.raw"
PPM = "/tmp/lvgl-fb.ppm"

def qmp_open():
    for _ in range(50):
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
    icount = len(sys.argv) > 4 and sys.argv[4] == "icount"
    if icount:
        cmd += ["-icount", "shift=4,sleep=off"]
    if APPEND:
        cmd[cmd.index("-kernel") + 2:cmd.index("-kernel") + 2] = ["-append", APPEND]
    proc = subprocess.Popen(cmd)
    s = qmp_open()
    greeting = qmp_read(s)
    qmp(s, {"execute": "qmp_capabilities"})
    ready = False
    fb = None
    deadline = time.time() + (60 if icount else 8)
    while time.time() < deadline:
        text = open(LOG, "r", errors="replace").read()
        if "ramfb published" in text and fb is None:
            for line in text.splitlines():
                if "ramfb published" in line and " at 0x" in line:
                    fb = int(line.split(" at 0x", 1)[1].split()[0], 16)
        if "[lvgl] ready" in text:
            ready = True
            break
        time.sleep(0.05)
    print("ready", ready, "fb", hex(fb) if fb else None)
    if not ready or fb is None:
        print(open(LOG, errors="replace").read())
        proc.kill()
        return 1
    def move_to(px, py):
        qmp(s, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": px * 32767 // 639}},
            {"type": "abs", "data": {"axis": "y", "value": py * 32767 // 479}},
        ]}})
        time.sleep(0.4)
    move_to(560, 40)
    qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-cursor-dark.ppm"}})
    print("cursor dark")
    move_to(160, 400)
    qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-cursor-light.ppm"}})
    print("cursor light")
    def send(events):
        reply = qmp(s, {"execute": "input-send-event", "arguments": {"events": events}})
        if "error" in reply:
            print("QMP", reply.strip())
    def click(px, py):
        send([
            {"type": "abs", "data": {"axis": "x", "value": px * 32767 // 639}},
            {"type": "abs", "data": {"axis": "y", "value": py * 32767 // 479}},
        ])
        time.sleep(0.25)
        qmp(s, {"execute": "human-monitor-command",
                "arguments": {"command-line": "mouse_button 1"}})
        time.sleep(0.25)
        qmp(s, {"execute": "human-monitor-command",
                "arguments": {"command-line": "mouse_button 0"}})
        time.sleep(0.3)
    def wait_for(token, seconds):
        end = time.time() + seconds
        text = ""
        while time.time() < end:
            text = open(LOG, "r", errors="replace").read()
            if token in text:
                return text
            time.sleep(0.1)
        return text
    text = open(LOG, "r", errors="replace").read()
    def box(name):
        key = "[lvgl] " + name + " box "
        for line in text.splitlines():
            if line.startswith(key):
                parts = line[len(key):].replace("x", " ").replace(",", " ").split()
                x, y, w, h = [int(p) for p in parts[:4]]
                return x + w // 2, y + h // 2
        return None
    load9 = box("load9")
    inp = box("input")
    cover = box("cover")
    back = box("back")
    plus = box("plus")
    print("boxes", load9, inp, cover, back, plus)
    move_to(500, 200)
    qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-home.ppm"}})
    print("home shot")
    if load9:
        click(*load9)
    done_wait = 90 if icount else 20
    done = "[menu] done 9" in wait_for("[menu] done 9", done_wait)
    print("clicked load", done)
    if not done:
        text = open(LOG, "r", errors="replace").read()
        print("icount blocker" if icount else "host blocker",
              "done 9 not seen", "panic" if "PANIC" in text else "no panic")
    time.sleep(0.4)
    move_to(520, 360)
    qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-screen.ppm"}})
    print("result shot")
    if done and cover:
        click(*cover)
        time.sleep(0.4)
        qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-cover.ppm"}})
        print("cover shot")
        if back:
            click(*back)
            time.sleep(0.3)
    if done and inp:
        click(*inp)
        time.sleep(0.3)
        if plus:
            click(*plus)
        for key in ("a", "b"):
            print(key, qmp(s, {"execute": "human-monitor-command",
                               "arguments": {"command-line": "sendkey " + key}}).strip()[:200])
            time.sleep(0.2)
        time.sleep(0.4)
        qmp(s, {"execute": "screendump", "arguments": {"filename": "/tmp/lvgl-input.ppm"}})
        print("input shot")
    qmp(s, {"execute": "human-monitor-command",
            "arguments": {"command-line": "pmemsave 0x%x 1228800 %s" % (fb, RAW)}})
    text = open(LOG, "r", errors="replace").read()
    proc.kill()
    proc.wait(timeout=5)
    print("--- log ---")
    for line in text.splitlines():
        if any(s in line for s in ("[lvgl]", "[menu]", "结果", "not connected", "=== 4", "=== 5", "Version")):
            print(line)
    if os.path.exists(RAW):
        data = open(RAW, "rb").read()
        print("fb bytes", len(data))
        # XRGB8888 little-endian to PPM
        with open(PPM, "w") as out:
            out.write("P3\n640 480\n255\n")
            for i in range(0, min(len(data), 640 * 480 * 4), 4):
                b, g, r = data[i], data[i + 1], data[i + 2]
                out.write("%d %d %d\n" % (r, g, b))
        print("wrote", PPM)
    return 0

if __name__ == "__main__":
    sys.exit(main())
