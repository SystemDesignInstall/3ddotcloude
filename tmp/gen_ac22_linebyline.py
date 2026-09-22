# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "tmp", "ac22_milestone_line_by_line.txt")

def run(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True)

buf = []
def w(s=""):
    buf.append(s)

d = run(["git", "diff", "HEAD", "--", MIL]).stdout.decode("utf-8", "replace")
w("### MILESTONE diff (working vs HEAD) — line by line, with ed markers ###")
oldn = newn = 0
i = 0
lines = d.splitlines()
while i < len(lines):
    ln = lines[i]
    if ln.startswith("@@"):
        import re
        m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", ln)
        if m:
            oldn = int(m.group(1)); newn = int(m.group(3))
            w("H " + ln)
            i += 1
            continue
    if ln.startswith("+++") or ln.startswith("---"):
        w("  " + ln)
        i += 1
        continue
    if ln.startswith("+"):
        newn += 1
        w("  + %d | %s" % (newn, ln[1:]))
    elif ln.startswith("-"):
        oldn += 1
        w("  - %d | %s" % (oldn, ln[1:]))
    elif ln.startswith(" "):
        oldn += 1; newn += 1
        w("    %d/%d | %s" % (oldn, newn, ln[1:]))
    else:
        w("  ? " + ln)
    i += 1

w("")
w("### MILESTONE HEAD lines 134-170 (full, numbered) ###")
h = run(["git", "show", "HEAD:" + MIL]).stdout.decode("utf-8", "replace")
for n, l in enumerate(h.splitlines(), 1):
    if 134 <= n <= 170:
        w("   H%3d | %s" % (n, l))

w("")
w("### REPORT diff (working vs HEAD) — line by line ###")
d2 = run(["git", "diff", "HEAD", "--", REP]).stdout.decode("utf-8", "replace")
oldn = newn = 0
i = 0
for ln in d2.splitlines():
    if ln.startswith("@@"):
        import re
        m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", ln)
        if m:
            oldn = int(m.group(1)); newn = int(m.group(3))
        w("H " + ln); continue
    if ln.startswith("+++") or ln.startswith("---"):
        w("  " + ln); continue
    if ln.startswith("+"):
        newn += 1; w("  + %d | %s" % (newn, ln[1:])); continue
    if ln.startswith("-"):
        oldn += 1; w("  - %d | %s" % (oldn, ln[1:])); continue
    if ln.startswith(" "):
        oldn += 1; newn += 1; w("    %d/%d | %s" % (oldn, newn, ln[1:])); continue
    w("  ? " + ln)

w("")
w("### REPORT HEAD lines 218-232 + WORKING 218-232 ###")
rp = os.path.join(REPO, "docs", "architecture", "P3.1-production-sparse-correction-verification-report.md")
hl = h if False else None
# report HEAD
rh = run(["git", "show", "HEAD:" + REP]).stdout.decode("utf-8", "replace")
for n, l in enumerate(rh.splitlines(), 1):
    if 218 <= n <= 232:
        w("   R-H%3d | %s" % (n, l))
w("   --- working ---")
for n, l in enumerate(io.open(rp, encoding="utf-8", errors="replace").read().splitlines(), 1):
    if 218 <= n <= 232:
        w("   R-W%3d | %s" % (n, l))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", OUT, "bytes=", os.path.getsize(OUT))
