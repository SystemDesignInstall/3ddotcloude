# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "ac22_fresh_truth.txt")

def run(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True, errors="replace")

buf = []
def w(s=""):
    buf.append(s)

w("### git log --oneline -6 ###")
r = run(["git", "log", "--oneline", "-6"])
w(r.stdout.strip())
w("")

w("### git status --short ###")
r = run(["git", "status", "--short"])
w(r.stdout.strip())
w("")

w("### git diff --stat (working vs HEAD) ###")
r = run(["git", "diff", "--stat"])
w(r.stdout.strip())
w("")

w("### git diff HEAD -- " + MIL + " ###")
r = run(["git", "diff", "HEAD", "--", MIL])
w(r.stdout)
w("")

w("### git diff HEAD -- " + REP + " ###")
r = run(["git", "diff", "HEAD", "--", REP])
w(r.stdout)
w("")

w("### git diff --check rc ###")
r = run(["git", "diff", "--check"])
w("rc=%s stderr=%r" % (r.returncode, r.stderr.strip()))
w("")

w("### wc lineno of docs (working) ###")
for f in (MIL, REP):
    r = run(["cmd", "/c", "findstr", "/c:\"\"", "NUL", f]) if False else None
with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(buf))
print("WROTE", os.path.getsize(OUT), OUT)
