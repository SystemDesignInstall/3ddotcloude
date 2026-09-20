# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
TEST = "tests/unit/test_p3_trajectory_optimization.cpp"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "ac22_purity_report.txt")
buf = []

def w(*a):
    buf.append(" ".join(str(x) for x in a))

def g(*a):
    r = subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

d = g("diff", "--", TEST).stdout

w("########## 1) TEST hunk-split: ищем AC-23..AC-31, Step-12, CLI ##########")
hunks = re.split(r"(?m)^(?=@@)", d)
pat_other = re.compile(r"AC-?2[3-9]|AC-?3[01]|AC-?2[0-9]|Step 1[0-9]|Step 12|CLI", re.I)
pat_ac22 = re.compile(r"AC-?22|AC22|DeterministicSecondRun|RunPipeline", re.I)
for h in hunks:
    if not h.startswith("@@"):
        continue
    hdr = h.splitlines()[0]
    body = h.splitlines()[1:]
    adds = [x for x in body if x.startswith("+")]
    adds_txt = "\n".join(x[1:] for x in adds)
    other = sorted({m.group(0) for m in pat_other.finditer(adds_txt) if not pat_ac22.match(m.group(0))})
    w("  HUNK %s  add=%d del=%d  OTHER_TOKENS=%s  AC22=%s" %
      (hdr[:38], len(adds), len([x for x in body if x.startswith("-")]),
       other[:20], bool(pat_ac22.search(adds_txt))))
    for x in adds:
        t = x[1:]
        if re.search(r"AC-?2[3-9]|AC-?3[01]|Step 12|CLI|sparse_correction_e2e|spatial_cli", t, re.I):
            w("      OTHER?  " + t[:200])
w("")
w("########## 2) MILESTONE @HEAD: строки Remaining/Step 12/AC-22 ##########")
hl = g("show", "HEAD:" + MIL).stdout.splitlines()
for i, ln in enumerate(hl, 1):
    if re.search(r"Remaining P3\.1|Step 12|AC-22|AC22|CLI", ln):
        w("  %4d | %s" % (i, ln[:220]))
w("")
w("########## 3) REPORT (working): diff ##########")
w(g("diff", "--", REP).stdout)
w("")
w("########## 4) git diff --check ##########")
r = g("diff", "--check")
w("  exit=%d  stderr=%r" % (r.returncode, r.stderr[:300]))
w("")
w("########## 5) git status --short ##########")
w(g("status", "--short").stdout)
io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE bytes=", os.path.getsize(OUT), OUT)
