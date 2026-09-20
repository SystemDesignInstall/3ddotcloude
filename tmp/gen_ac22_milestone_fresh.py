# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "tmp", "ac22_milestone_full_fresh.txt")

def run(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True)

buf = []
def w(s=""):
    buf.append(s)

w("### MILESTONE diff HEAD (full, UTF-8) ###")
r = run(["git", "diff", "HEAD", "--", MIL])
w(r.stdout.decode("utf-8", "replace"))
w("")
w("### MILESTONE working doc lines 130-175 (numbered) ###")
mp = os.path.join(REPO, "docs", "architecture", "P3-milestone-status.md")
for i, ln in enumerate(io.open(mp, encoding="utf-8", errors="replace").read().splitlines(), 1):
    if 130 <= i <= 175:
        w("%4d | %s" % (i, ln))
w("")
w("### MILESTONE HEAD doc lines 130-150 (numbered) ###")
h = run(["git", "show", "HEAD:" + MIL]).stdout.decode("utf-8", "replace")
for i, ln in enumerate(h.splitlines(), 1):
    if 130 <= i <= 150:
        w("%4d | %s" % (i, ln))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", OUT, "bytes=", os.path.getsize(OUT))
