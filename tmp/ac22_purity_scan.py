# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
files = {
    "test": "tests/unit/test_p3_trajectory_optimization.cpp",
    "milestone": "docs/architecture/P3-milestone-status.md",
    "report": "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
}
OUT = os.path.join(REPO, "tmp", "ac22_purity_scan.txt")
buf = []

def w(*a):
    buf.append(" ".join(str(x) for x in a))

def g(*a):
    r = subprocess.run(["git"] + list(a), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

def readA(path):
    r = g("show", "HEAD:" + path)
    return r.stdout.splitlines()

## 1) TEST file: added hunks, scan for tokens
w("########## 1) TEST hunk purity ##########")
d = g("diff", "--", files["test"]).stdout
hunks = re.split(r"(?m)(?=^@@ )", d)
pat = r"AC-?\s*(\d+)"
for h in hunks:
    if not h.startswith("@@"):
        continue
    hdr = h.splitlines()[0]
    adds = [l for l in h.splitlines()[1:] if l.startswith("+")]
    adds_txt = "\n".join(a[1:] for a in adds)
    acs = set(re.findall(pat, adds_txt))
    has_cli = bool(re.search(r"\bCLI\b|Step 12|RunCommand|spatial run |--input |--config ", adds_txt))
    has_e2e = bool(re.search(r"\bE2E\b|sparse_correction_e2e|cli_sparse", adds_txt))
    w("  HUNK %s | add=%d ac_ids=%s cli=%s e2e=%s" % (hdr[:44], len(adds),
      sorted(acs) if acs else "[]", has_cli, has_e2e))
    for ln in adds:
        t = ln[1:]
        if re.search(r"AC-?\s*22|AC22|DeterministicSecondRun|RunPipeline", t):
            w("     + [ac22] " + t.strip()[:140])
    for ln in adds:
        t = ln[1:]
        if re.search(r"not supported|fails|quarantin|N6|N7|N8|PoseGraph|GlobalPose|Outlier|covariance", t):
            w("     + [misc?] " + t.strip()[:140])
w("")

## 2) MILESTONE HEAD — around "Remaining P3.1 items"
w("########## 2) MILESTONE @HEAD: 'Remaining P3.1 items' + AC-22 ##########")
hl = readA(files["milestone"])
for i, ln in enumerate(hl, 1):
    if "Remaining P3.1" in ln or "AC-22" in ln or "AC22" in ln:
        w("   %4d| %s" % (i, ln.strip()[:180]))
w("")

## 3) REPORT diff
w("########## 3) REPORT diff hunks ##########")
d = g("diff", "--", files["report"]).stdout
for h in re.split(r"(?m)(?=^@@ )", d):
    if not h.startswith("@@"):
        continue
    hdr = h.splitlines()[0]
    body = "\n".join(h.splitlines()[1:])
    ac22 = "AC-22" in body
    cli = bool(re.search(r"\bCLI\b|RunCommand|spatial run", body))
    w("   HUNK %s | AC22=%s CLI=%s add=%d del=%d" %
      (hdr[:44], ac22, cli, body.count(chr(10) + "+"), body.count(chr(10) + "-")))
    for ln in h.splitlines()[1:]:
        if ln[:1] in "+-" and (re.search(r"AC-22|CLI|Step", ln)):
            w("      %s | %s" % (ln[:1], ln[1:].strip()[:170]))
w("")

## 4) diff --check + status + stat
r = g("diff", "--check")
w("########## 4) git diff --check ##########")
w("   exit_stdout=%r  stderr=%r" % (r.stdout[-120:], r.stderr[-120:]))
w("")
w("########## 5) git status --short ##########")
for l in g("status", "--short").stdout.splitlines():
    w("   " + l)
w("")
w("########## 6) git diff --stat ##########")
for l in g("diff", "--stat").stdout.splitlines():
    w("   " + l)

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", os.path.getsize(OUT), "to", OUT)
