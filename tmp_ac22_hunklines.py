# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
files = [
    "docs/architecture/P3-milestone-status.md",
    "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
]
buf = []

def w(s=""):
    buf.append(s)

def git(*args):
    r = subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

w("MILESTONE-STATUS: AC-22 PROVEN lines")
r = git("diff", "--", files[0])
for hunk in re.split(r"(?m)^(?=@@)", r.stdout):
    if not hunk.startswith("@@"):
        continue
    header = hunk.splitlines()[0]
    relines = [l for l in hunk.splitlines()[1:] if l.startswith(("+", "-")) and "AC-22" in l]
    if relines:
        w("  HUNK " + header)
        for l in relines:
            w("    " + l[:200])
w("")
w("REPORT: AC-22 PROVEN lines")
r = git("diff", "--", files[1])
for hunk in re.split(r"(?m)^(?=@@)", r.stdout):
    if not hunk.startswith("@@"):
        continue
    header = hunk.splitlines()[0]
    relines = [l for l in hunk.splitlines()[1:] if l.startswith(("+", "-")) and "AC-22" in l]
    if relines:
        w("  HUNK " + header)
        for l in relines:
            w("    " + l[:200])
w("")
w("DIFF_CHECK:", ("OK" if git("diff", "--check").returncode == 0 else "FAIL"))
w("STATUS:")
w(git("status", "--short").stdout.strip())

out = os.path.join(REPO, "tmp_ac22_hunklines.txt")
io.open(out, "w", encoding="utf-8").write("\n".join(buf))
print("wrote", out)
