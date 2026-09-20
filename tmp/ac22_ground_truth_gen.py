# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
OUT = os.path.join(REPO, "tmp", "ac22_ground_truth.txt")
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
TST = "tests/unit/test_p3_trajectory_optimization.cpp"

def g(*a):
    r = subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

buf = []
def w(*a):
    buf.append(" ".join(str(x) for x in a))

w("=" * 12, "STATUS --short")
for l in g("status", "--short").stdout.splitlines():
    w("  " + l)

w("")
w("=" * 12, "git diff --check  |  rc=%s" % g("diff", "--check").returncode)
w("=" * 12, "git diff --stat  |  rc=%s" % g("diff", "--stat").returncode)
for l in g("diff", "--stat").stdout.splitlines():
    w("  " + l)

w("")
w("=" * 12, "HEAD inclusive cpp test: содержит ли AC-22 PROVEN текст")
hdr = g("show", "HEAD:" + TST).stdout
w("  HEAD-test has AC22-Proven-token:", "AC22_DeterministicSecondRun" in hdr)

w("")
w("=" * 12, "MILESTONE diff — ПОЛНЫЙ")
d = g("diff", "--", MIL)
w("  rc=%s  b'\n'.len=%d" % (d.returncode, len(d.stdout.encode("utf-8"))))
w(d.stdout)

w("")
w("=" * 12, "REPORT diff — ПОЛНЫЙ")
d = g("diff", "--", REP)
w("  rc=%s  len=%d" % (d.returncode, len(d.stdout.encode("utf-8"))))
w(d.stdout)

w("")
w("=" * 12, "MILESTONE @HEAD: строки про AC-22")
for i, ln in enumerate(g("show", "HEAD:" + MIL).stdout.splitlines(True), 1):
    if "AC-22" in ln:
        w("   HEAD %5d | %s" % (i, ln.strip()[:180]))

w("")
w("=" * 12, "REPORT @HEAD: строки про AC-22")
for i, ln in enumerate(g("show", "HEAD:" + REP).stdout.splitlines(True), 1):
    if "AC-22" in ln:
        w("   HEAD %5d | %s" % (i, ln.strip()[:180]))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", len("\n".join(buf).encode("utf-8")), "bytes ->", OUT)
