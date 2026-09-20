# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
OUT = os.path.join(REPO, "ac22_truth_final.txt")

def g(*a):
    return subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                          text=True, errors="replace")

buf = []
def w(*a):
    buf.append("  ".join(str(x) for x in a))

MIL = "docs/architecture/P3-milestone-status.md"
d = g("diff", "--", MIL).stdout

w("### MILESTONE diff hunks (working vs HEAD) — full ###")
for hu in re.split(r"(?m)(?=^@@ )", d):
    if not hu.startswith("@@"):
        continue
    hdr = hu.splitlines()[0]
    body = hu.splitlines()[1:]
    adds = [x for x in body if x.startswith("+")]
    w("  HUNK %s  add=%d" % (hdr, len(adds)))
    for x in adds:
        tag = "AC22" if "AC-22" in x else ("STEP12" if ("Step 12" in x) else "  ")
        w("    %s | %s" % (tag, x[1:]))
w("")
w("### HEAD milestone: все строки с AC-22 ###")
h = g("show", "HEAD:" + MIL).stdout
for i, ln in enumerate(h.splitlines(), 1):
    if "AC-22" in ln:
        w("   %4d | %s" % (i, ln[:400]))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", os.path.getsize(OUT), "bytes ->", OUT)
