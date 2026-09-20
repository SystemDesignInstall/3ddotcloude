# -*- coding: utf-8 -*-
import io, os, re, subprocess
REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
TST = "tests/unit/test_p3_trajectory_optimization.cpp"
OUT = os.path.join(REPO, "ac22_ground_truth_dump.txt")

def g(*a):
    return subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                          text=True, errors="replace")

B = []
def w(*a):
    B.append(" ".join(str(x) for x in a))

w("### STATUS --short ###")
for l in g("status", "--short").stdout.splitlines():
    w("  " + l)

w("")
w("### diff --check (rc) ###")
r = g("diff", "--check")
w("  rc=%d stderr=%r" % (r.returncode, r.stderr[:120]))

w("")
w("### diff --stat ###")
for l in g("diff", "--stat").stdout.splitlines():
    w("  " + l)

w("")
w("### HEAD milestone: AC-22 строки ###")
h = g("show", "HEAD:" + MIL).stdout.splitlines()
for i, ln in enumerate(h, 1):
    if "AC-22" in ln:
        w("  HEAD %4d | %s" % (i, ln.strip()[:200]))

w("")
w("### WORKING milestone: AC-22 строки ###")
h = g("show", "HEAD:" + MIL).stdout.splitlines()
wl = io.open(os.path.join(REPO, MIL), encoding="utf-8", errors="replace").read().splitlines()
for i, ln in enumerate(h, 1):
    if "AC-22" in ln:
        w("  HEAD %4d | %s" % (i, ln.strip()[:200]))

w("")
w("### WORKING milestone diff hunks: пометки Step12/CLI/AC22 в каждой hunk-заголовке ###")
d = g("diff", "--", MIL).stdout
hunks = re.split(r"(?m)^(?=@@ )", d)
for hu in hunks:
    if not hu.startswith("@@"):
        continue
    hdr = hu.splitlines()[0]
    body = hu.splitlines()[1:]
    has_s12 = any("Step 12" in x for x in body)
    has_cli = any("CLI" in x for x in body)
    has_ac22 = any("AC-22" in x for x in body)
    adds = [x for x in body if x.startswith("+")]
    dels = [x for x in body if x.startswith("-")]
    w("  HUNK %s | Step12=%s CLI=%s AC22=%s add=%d del=%d" %
      (hdr[:44], has_s12, has_cli, has_ac22, len(adds), len(dels)))
    for x in adds:
        t = x[1:].strip()
        if re.search(r"AC-22|Step 12|CLI|DeterministicSecondRun", t):
            w("       + | %s" % t[:190])

w("")
w("### WORKING report diff hunks ###")
d = g("diff", "--", REP).stdout
hunks = re.split(r"(?m)^(?=@@ )", d)
for hu in hunks:
    if not hu.startswith("@@"):
        continue
    hdr = hu.splitlines()[0]
    body = hu.splitlines()[1:]
    w("  HUNK %s" % hdr[:44])
    for x in body:
        if x.startswith(("+", "-")):
            w("     %s | %s" % (x[0], x[1:].strip()[:200]))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(B))
print("WROTE", os.path.getsize(OUT))
