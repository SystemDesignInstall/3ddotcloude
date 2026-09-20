# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = os.path.join(REPO, "docs", "architecture", "P3-milestone-status.md")
REP = os.path.join(REPO, "docs", "architecture",
                   "P3.1-production-sparse-correction-verification-report.md")
OUT = os.path.join(REPO, "ac22_lines_dump.txt")
b = []

def w(*a):
    b.append(" ".join(str(x) for x in a))

def g(*a):
    r = subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

def lines_from_stdout(s, keep_ctx=True):
    return s.splitlines()

w("== HEAD milestone: AC-22 / NOT started / Remaining ", "==")
h = g("show", "HEAD:" + MIL + "").stdout
for i, ln in enumerate(h.splitlines(), 1):
    if re.search(r"AC-22|Remaining P3\.1|NOT started", ln):
        w("   %5d [%s]" % (i, ln.strip()[:180]))

w("")
w("== HEAD report: AC-22 row ", "==")
h = g("show", "HEAD:" + REP).stdout
for i, ln in enumerate(h.splitlines(), 1):
    if "AC-22" in ln:
        w("   %5d [%s]" % (i, ln.strip()[:240]))
    if "Deterministic second run" in ln:
        w("   %5d [%s]" % (i, ln.strip()[:240]))

w("")
w("== WORKING milestone: строки 155-175 (read-инструментом читается) ==", )
wl = io.open(MIL, encoding="utf-8", errors="replace").read().splitlines()
for i, ln in enumerate(wl, 1):
    if 158 <= i <= 173:
        w("   %5d [%s]" % (i, ln[:220]))

w("")
w("== WORKING report: строки 215-235 ==")
rl = io.open(REP, encoding="utf-8", errors="replace").read().splitlines()
for i, ln in enumerate(rl, 1):
    if 218 <= i <= 234:
        w("   %5d [%s]" % (i, ln[:240]))

w("")
w("== git diff --check + status ==")
r = g("diff", "--check")
w("   diff-check rc=%s  out=%r err=%r" % (r.returncode, r.stdout[:200], r.stderr[:200]))
for l in g("status", "--short").stdout.splitlines():
    w("   " + l)

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(b))
print("WROTE", os.path.getsize(OUT), "bytes to", OUT)
