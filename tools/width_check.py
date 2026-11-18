import sys,unicodedata
def w(s): return sum(2 if unicodedata.east_asian_width(c) in "WF" else 0 if unicodedata.combining(c) else 1 for c in s)
for f in sys.argv[1:]:
    seen=set()
    for l in open(f,encoding="utf-8",errors="replace").read().splitlines():
        if w(l)>80 and l not in seen:
            seen.add(l); print(w(l), l[:150])
