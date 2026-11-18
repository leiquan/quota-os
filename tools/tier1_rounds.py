#!/usr/bin/env python3
"""Tier 1 deadline regression: boot N times, run the documented load, collect T1STAT.

  python3 tools/tier1_rounds.py <kernel.bin> <outdir> <rounds> [icount_shift]

Load per round (tiering ON the whole time): menu 1, 4 (8 Tier 3 busy loops),
6 (runaway Tier 1/2 + Tier 3 hog), 10 (32 Tier 2 slices), 4, 6, then menu 1
again.  The final menu 1 prints the counters accumulated since the first task
ran: Tier 1 jobs/misses, latency histogram from the ideal period start
(epoch + k*10000us), skipped periods and clock jumps.  Raw console logs go to
<outdir>/round-NN.txt and the aggregate to <outdir>/summary.txt.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_serial import Session  # noqa: E402

P = "select> "
STEPS = [("1\n", P, 30), ("4\n", P, 60), ("6\n", P, 60), ("10\n", P, 120),
         ("4\n", P, 60), ("6\n", P, 60), ("1\n", P, 30)]


def quantile(hist, q):
    n = sum(hist.values())
    need = max(1, -(-n * q // 1000))
    acc = 0
    for k in sorted(hist):
        acc += hist[k]
        if acc >= need:
            return k
    return 0


def parse(text):
    tail = text[text.rfind("========== 资源表"):]
    stats, hists, glob = {}, {}, {}
    for line in tail.splitlines():
        if line.startswith("T1STAT periods="):
            glob = dict(kv.split("=") for kv in line.split()[1:] if "=" in kv)
        elif line.startswith("T1STAT "):
            parts = line.split()
            stats[parts[1]] = {k: int(v) for k, v in (p.split("=") for p in parts[2:])}
        elif line.startswith("T1HIST "):
            parts = line.split()
            hists[parts[1]] = {int(a): int(b) for a, b in (p.split(":") for p in parts[2:])}
    return stats, hists, glob


def main():
    kernel, outdir, rounds = sys.argv[1], sys.argv[2], int(sys.argv[3])
    shift = sys.argv[4] if len(sys.argv) > 4 else None
    extra = ["-icount", "shift=%s,sleep=off" % shift] if shift is not None else []
    os.makedirs(outdir, exist_ok=True)
    rows, agg_hist, agg = [], {}, {}
    for r in range(1, rounds + 1):
        s = Session(kernel, "128M", extra)
        ok = s.run(STEPS)
        text = s.close()
        open(os.path.join(outdir, "round-%02d.txt" % r), "w").write(
            "# cmd: %s\n%s" % (" ".join(s.cmd), text))
        stats, hists, glob = parse(text) if ok else ({}, {}, {})
        line = "round %02d ok=%s" % (r, ok)
        for name in sorted(stats):
            st = stats[name]
            line += " | %s jobs=%d misses=%d skipped=%d unfinished=%d p50=%d p99=%d max=%d" % (
                name, st["jobs"], st["misses"], st["skipped"], st["unfinished"],
                st["lat_p50"], st["lat_p99"], st["lat_max"])
            a = agg.setdefault(name, {"jobs": 0, "misses": 0, "skipped": 0, "unfinished": 0,
                                      "max": 0, "lag": 0, "deadline": st["deadline"]})
            for k in ("jobs", "misses", "skipped", "unfinished"):
                a[k] += st[k]
            a["max"] = max(a["max"], st["lat_max"])
            a["lag"] = max(a["lag"], st["dispatch_lag_max"])
            h = agg_hist.setdefault(name, {})
            for k, v in hists.get(name, {}).items():
                h[k] = h.get(k, 0) + v
        if glob:
            line += " | periods=%s skips=%s clock_jumps=%s" % (
                glob["periods"], glob["period_skips"], glob["clock_jumps"])
            for k in ("periods", "period_skips", "clock_jumps"):
                agg[k] = agg.get(k, 0) + int(glob[k])
        rows.append(line)
        print(line, flush=True)
    out = ["mode: %s" % ("icount shift=%s,sleep=off" % shift if shift is not None else "normal (no icount)"),
           "kernel: %s" % kernel, "rounds: %d" % rounds, ""] + rows + ["", "TOTAL"]
    for name in sorted(agg_hist):
        a, h = agg[name], agg_hist[name]
        out.append("%s jobs=%d misses=%d (skipped=%d unfinished=%d) lat_from_ideal p50=%d p99=%d "
                   "max=%d us deadline=%d us dispatch_lag_max=%d us" % (
                       name, a["jobs"], a["misses"], a["skipped"], a["unfinished"],
                       quantile(h, 500), quantile(h, 990), a["max"], a["deadline"], a["lag"]))
    out.append("periods=%d period_skips=%d clock_jumps=%d" % (
        agg.get("periods", 0), agg.get("period_skips", 0), agg.get("clock_jumps", 0)))
    out.append("failed_rounds=%d" % sum(1 for r in rows if "ok=False" in r))
    open(os.path.join(outdir, "summary.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out[-5:]))


if __name__ == "__main__":
    main()
