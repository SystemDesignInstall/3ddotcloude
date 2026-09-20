# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = os.path.join(REPO, "docs", "architecture", "P3-milestone-status.md")
REP = os.path.join(REPO, "docs", "architecture",
                   "P3.1-production-sparse-correction-verification-report.md")
OUT = os.path.join(REPO, "ac22_hunkdump.txt")

buf = []
def w(*a):
    buf.append(" ".join(str(x) for x in a))

def g(*args):
    r = subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True,
                       text=True, errors="replace")
    return r

# --- milestone: split raw git diff into hunks, print hunk header + all +/- lines ---
w("##### P3-milestone-status.md — ПОЛНЫЙ hunk-сплит (рабочее дерево vs HEAD) #####")
d = g("diff", "--", "docs/architecture/P3-milestone-status.md").stdout
parts = re.split(r"(?m)^(?=@@ )", d)
for p in parts:
    if not p.startswith("@@"):
        continue
    lines = p.splitlines()
    head = lines[0]
    body = lines[1:]
    has_ac22 = any("AC-22" in l for l in body)
    has_step12 = any("Step 12" in l or "CLI real providers" in l for l in body)
    w("")
    w("  HUNK %s | AC22=%s Step12=%s" % (head[:60], has_ac22, has_step12))
    for l in body:
        mark = l[0]
        txt = l[1:] if l[:1] in "+-" else l
        if txt.strip():
            w("    %s | %s" % (mark if mark in "+-" else " ", txt[:230]))
w("")
open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("BYTES", os.path.getsize(OUT))
