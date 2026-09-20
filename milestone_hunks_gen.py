# -*- coding: utf-8 -*-
import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDOT\spatial-platform"
DOC = "docs/architecture/P3-milestone-status.md"
OUT = os.path.join(REPO, "milestone_hunks.txt")

d = subprocess.run(["git", "diff", "--", DOC], cwd=REPO,
                   capture_output=True, text=True, errors="replace").stdout

lines = d.splitlines(keepends=True)
hunks = []  # each: (header, body_lines)
cur_header = None
cur = []
for ln in lines:
    if ln.startswith("@@"):
        if cur_header is not None:
            hunks.append((cur_header, cur))
        cur_header = ln.strip()
        cur = []
    elif cur_header is not None:
        cur.append(ln)

buf = []
for hdr, body in hunks:
    bodytxt = "".join(body)
    toks = []
    for t in ["Step 12", "Step 11", "Step 10", "AC-22", "AC22", "CLI", "CMakeLists", "Remaining"]:
        if t in bodytxt:
            toks.append(t)
    adds = [b for b in body if b.startswith("+")]
    dels = [b for b in body if b.startswith("-")]
    buf.append("HUNK %s" % hdr)
    buf.append("  TOKENS: %s" % ", ".join(toks))
    buf.append("  N add=%d del=%d" % (len(adds), len(dels)))
    for a in adds[:60]:
        buf.append("     +|" + a[1:].rstrip("\n")[:200])
    for de in dels[:20]:
        buf.append("     -|" + de[1:].rstrip("\n")[:200])
    buf.append("")

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(buf))
print("wrote", OUT, os.path.getsize(OUT), "bytes; hunks =", len(hunks))
