# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
OUT = os.path.join(REPO, "ac22_head_docs_lines.txt")

buf = []
def w(*a):
    buf.append(" ".join(str(x) for x in a))

def head_lines(path):
    r = subprocess.run(["git", "show", "HEAD:" + path], cwd=REPO,
                       capture_output=True, text=True, errors="replace")
    return r.stdout.splitlines()

w("### HEAD milestone: Step 12 / CLI / AC-22 / Remaining ###")
for i, ln in enumerate(head_lines(MIL), 1):
    low = ln.lower()
    if ("step 12" in low or "cli real providers" in low or "ac-22" in low
            or "remaining p3.1" in low or "sole user" in low):
        w("   %5d | %s" % (i, ln.strip()[:200]))

w("")
w("### HEAD report: AC-22 ###")
for i, ln in enumerate(head_lines(REP), 1):
    if "AC-22" in ln:
        w("   %5d | %s" % (i, ln.strip()[:200]))

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("WROTE", os.path.getsize(OUT), "->", OUT)
