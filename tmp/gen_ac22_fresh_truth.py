# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "tmp", "ac22_fresh_truth.txt")

def run(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True)

buf = []
def w(s=""):
    buf.append(s)

def lines(txt):
    out = []
    for ln in txt.splitlines():
        low = ln.lower()
        if ("ac-22" in low or "step 12" in low or "cli real providers" in low
                or "sole user" in low or "remaining p3.1" in low):
            out.append(ln)
    return out

w("### git log --oneline -8 ###")
w(run(["git", "log", "--oneline", "-8"]).stdout.decode("utf-8", "replace").strip())
w("")
w("### git status --short ###")
w(run(["git", "status", "--short"]).stdout.decode("utf-8", "replace").strip())
w("")
w("### git diff --stat HEAD ###")
w(run(["git", "diff", "--stat", "HEAD"]).stdout.decode("utf-8", "replace").strip())
w("")
w("### MILESTONE: lines of interest in HEAD ###")
for i, ln in enumerate(run(["git", "show", "HEAD:" + MIL]).stdout.decode("utf-8", "replace").splitlines(), 1):
    if any(k in ln.lower() for k in ("ac-22", "step 12", "cli real providers", "sole user", "remaining p3.1", "step 11")):
        w("   HEAD %d | %s" % (i, ln.strip()[:180]))
w("")
w("### MILESTONE: lines of interest in WORKING (file) ###")
mp = os.path.join(REPO, "docs", "architecture", "P3-milestone-status.md")
for i, ln in enumerate(io.open(mp, encoding="utf-8", errors="replace").read().splitlines(), 1):
    if any(k in ln.lower() for k in ("ac-22", "step 12", "cli real providers", "sole user", "remaining p3.1")):
        w("   WORK %d | %s" % (i, ln.strip()[:180]))
w("")
w("### REPORT: lines of interest in WORKING ###")
rp = os.path.join(REPO, "docs", "architecture", "P3.1-production-sparse-correction-verification-report.md")
for i, ln in enumerate(io.open(rp, encoding="utf-8", errors="replace").read().splitlines(), 1):
    if "ac-22" in ln.lower():
        w("   WORK %d | %s" % (i, ln.strip()[:180]))
w("")

# milestone diff hunks split
w("### MILESTONE diff hunks (working vs HEAD): token purity per hunk ###")
d = run(["git", "diff", "HEAD", "--", MIL]).stdout.decode("utf-8", "replace")
cur = None
for ln in d.splitlines():
    if ln.startswith("@@"):
        if cur:
            w("  HUNK %s tokens=%s add=%d del=%d" % (cur[0], sorted(cur[1]), cur[2], cur[3]))
        cur = [ln, set(), 0, 0]
    elif cur:
        cur[1].add("AC22") if "ac-22" in ln.lower() else None
        cur[1].add("Step12") if "step 12" in ln.lower() else None
        cur[1].add("CLI") if ("cli" in ln.lower() and ("real providers" in ln.lower() or "sole" in ln.lower())) else None
        if ln.startswith("+"):
            cur[2] += 1
        elif ln.startswith("-"):
            cur[3] += 1
if cur:
    w("  HUNK %s tokens=%s add=%d del=%d" % (cur[0], sorted(cur[1]), cur[2], cur[3]))
w("")

# report diff hunks split
w("### REPORT diff hunks (working vs HEAD) ###")
d = run(["git", "diff", "HEAD", "--", REP]).stdout.decode("utf-8", "replace")
cur = None
for ln in d.splitlines():
    if ln.startswith("@@"):
        if cur:
            w("  HUNK %s add=%d del=%d" % (cur[0], cur[1], cur[2]))
        cur = [ln, 0, 0]
    elif cur:
        if ln.startswith("+") and not ln.startswith("+++"):
            cur[1] += 1
        elif ln.startswith("-") and not ln.startswith("---"):
            cur[2] += 1
if cur:
    w("  HUNK %s add=%d del=%d" % (cur[0], cur[1], cur[2]))
w("")
w("### git diff --check (rc) ###")
r = run(["git", "diff", "--check"])
w("  rc=%d stderr=%r" % (r.returncode, r.stderr.decode("utf-8", "replace")))
w("")

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", OUT, "bytes=", os.path.getsize(OUT))
