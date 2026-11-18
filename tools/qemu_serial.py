#!/usr/bin/env python3
"""Drive Quota OS over the QEMU serial console.  Shared by the capture scripts."""
import os
import select
import subprocess
import time

BASE = [
    "qemu-system-aarch64",
    "-machine", "virt,gic-version=2",
    "-cpu", "cortex-a72",
    "-nographic", "-nic", "none",
]


class Session:
    def __init__(self, kernel, mem="128M", extra=None, machine=None):
        cmd = list(BASE)
        if machine:
            cmd[cmd.index("virt,gic-version=2")] = machine
        cmd += ["-m", mem, "-kernel", kernel] + list(extra or [])
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.buf = b""
        self.pos = 0

    def _pull(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            r, _, _ = select.select([self.proc.stdout], [], [], 0.1)
            if not r:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            self.buf += chunk
        return True

    def wait(self, token, limit):
        token_b = token.encode()
        deadline = time.time() + limit
        while time.time() < deadline:
            idx = self.buf.find(token_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(token_b)
                return True
            if self.proc.poll() is not None:
                self._pull(0.2)
                return self.buf.find(token_b, self.pos) >= 0
            self._pull(0.2)
        return False

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def run(self, steps):
        """steps: list of (reply, token, limit). Send reply, then wait for token."""
        ok = self.wait("select> ", 30)
        for reply, token, limit in steps:
            if not ok:
                break
            if reply is not None:
                self.send(reply)
            if token:
                ok = self.wait(token, limit)
        return ok

    def close(self):
        try:
            self.send("\x01x")
        except (BrokenPipeError, OSError):
            pass
        self._pull(0.3)
        self.proc.kill()
        self.proc.wait(timeout=5)
        return self.text()

    def text(self):
        return self.buf.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
