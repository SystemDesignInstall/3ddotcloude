# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
TST = os.path.join(REPO, "tests", "unit", "test_p3_trajectory_optimization.cpp")
MIL = os.path.join(REPO, "docs", "architecture", "P3-milestone-status.md")
REP = os.path.join(REPO, "docs", "architecture",
                   "P3.1-production-sparse-correction-verification-report.md")
OUT = os.path.join(REPO, "ac22_final_truth.txt")

buf = []
def w(*a):
    buf.append(" ".join(str(x) for x in a))

def g(*a):
    r = subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

# ---------- 1) git status --short (+junk top) ----------
w("#### 1) git status --short ####")
for l in g("status", "--short").stdout.splitlines():
    w("   " + l)

# ---------- 2) очистка: списки junk-файлов в корне ----------
w("")
w("#### 2) junk в корне (мой temp) — должно удалять НЕ трогая Step12/CLI/CMake ####")
for f in sorted(os.listdir(REPO)):
    if os.path.isfile(os.path.join(REPO, f)):
        if f.lower().startswith(("ac22", "tmp_", "temp_", "cli_preview", "ms_diff",
                                  "rep_diff", "gen_", "milestone_hunk")):
            w("   JUNK? " + f)

# ---------- 3) MILESTONE: hunk-сплит рабочего diff ----------
w("")
w("#### 3) MILESTONE diff hunk-сплит (рабочее дерево vs HEAD) ####")
d = g("diff", "--", "docs/architecture/P3-milestone-status.md").stdout
hunktxt = re.split(r"(?m)(?=^@@ )", d)
for hu in hunktxt:
    if not hu.startswith("@@"):
        continue
    hdr = hu.splitlines()[0]
    lines = hu.splitlines()[1:]
    adds = [x for x in lines if x.startswith("+")]
    has_ac22 = any("AC-22" in x for x in adds)
    has_step12 = any("Step 12" in x and "CLI" in x for x in adds)
    has_cli = any(re.search(r"CLI real providers|spatial_cli_sparse|CLI E2E", x) for x in adds)
    w("   HUNK %s | ac22=%s step12cli=%s clie2e=%s add=%d" % (
        hdr[:40], has_ac22, has_step12, has_cli, len(adds)))
    for x in adds:
        if "AC-22" in x or "Step 12" in x:
            w("      + " + x[1:][:170])

# ---------- 4) REPORT: hunk-сплит + AC-22 ----------
w("")
w("#### 4) REPORT diff hunk-сплит (рабочее дерево vs HEAD) ####")
d = g("diff", "--",
      "docs/architecture/P3.1-production-sparse-correction-verification-report.md").stdout
hunktxt = re.split(r"(?m)(?=^@@ )", d)
for hu in hunktxt:
    if not hu.startswith("@@"):
        continue
    hdr = hu.splitlines()[0]
    lines = hu.splitlines()[1:]
    adds = [x for x in lines if x.startswith("+")]
    dels = [x for x in lines if x.startswith("-")]
    w("   HUNK %s | add=%d del=%d ac22add=%s" % (
        hdr[:40], len(adds), len(dels),
        any("AC-22" in x for x in adds)))
    for x in adds + dels:
        if "AC-22" in x:
            w("      %s " % x[0] + x[1:][:170])

# ---------- 5) TEST purity ----------
w("")
w("#### 5) TEST diff: hunk+/- только AC-22? (без CLI/Step12) ####")
d = g("diff", "--", "tests/unit/test_p3_trajectory_optimization.cpp").stdout
hunktxt = re.split(r"(?m)(?=^@@ )", d)
for hu in hunktxt:
    if not hu.startswith("@@"):
        continue
    adds = [x[1:] for x in hu.splitlines()[1:] if x.startswith("+")]
    tok = set()
    for a in adds:
        if re.search(r"AC-?2[0-9]", a):
            tok.add(re.search(r"AC-?2[0-9]", a).group(0))
    cli = any(re.search(r"CLI|Step 1[24]|spatial_cli|RunCommand|exit code", a) for a in adds)
    w("   HUNK %s | add=%d tokens=%s cli=%s" % (
        hu.splitlines()[0][:40], len(adds), sorted(tok), cli))

# ---------- 6) HEAD строки доков про AC-22 ----------
w("")
w("#### 6) HEAD milestone: строки AC-22 / Remaining P3.1 ####")
ml = g("show", "HEAD:docs/architecture/P3-milestone-status.md").stdout.splitlines()
for i, ln in enumerate(ml, 1):
    if re.search(r"AC-22|Remaining P3\.1", ln):
        w("   HEAD %5d | %s" % (i, ln.strip()[:150]))

w("")
w("#### 6b) HEAD report: строка AC-22 ####")
rp = g("show", "HEAD:docs/architecture/P3.1-production-sparse-correction-verification-report.md").stdout
for i, ln in enumerate(list(rp.splitlines()), 1):
    if "AC-22" in ln:
        w("   HEAD %5d | %s" % (i, ln.strip()[:150]))

# ---------- 7) diff --check ----------
w("")
w("#### 7) git diff --check ####")
r = g("diff", "--check")
w("   rc=%s stderr=%s" % (r.returncode, r.stderr[:120]))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("OK bytes=", os.path.getsize(OUT))
