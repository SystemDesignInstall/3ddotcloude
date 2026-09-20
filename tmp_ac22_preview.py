import io, os, re, subprocess

REPO = r"C:\Code\ClaudeDOT\spatial-platform"
OUT = os.path.join(REPO, "tmp_ac22_commit_preview.txt")
DOCS = [
    "docs/architecture/P3-milestone-status.md",
    "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
]

buf = []
def w(*parts):
    buf.append("".join(str(p) for p in parts))

def r(args=(), **kw):
    return subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True,
                          text=True, errors="replace", **kw)

w("=== exact AC-XX table rows in working tree: docs ===")
for d in DOCS:
    p = os.path.join(REPO, d)
    lines = io.open(p, encoding="utf-8", errors="replace").read().splitlines()
    w("")
    w("##### " + d)
    for i, ln in enumerate(lines, 1):
        # only show rows that are AC-table rows
        if re.match(r"^\|\s*AC-", ln):
            w("  %4d | %s" % (i, ln[:175]))

w("")
w("=== git diff (docs), full raw, hunk-split ===")
for d in DOCS:
    res = r(["diff", "--", d])
    w("")
    w("##### " + d)
    text = res.stdout
    if not text.strip():
        w("  (no diff)")
        continue
    for chunk in re.split(r"(?m)^(?=diff --git)", text):
        if not chunk.strip():
            continue
        w("  >> " + chunk.splitlines()[0])
        # split hunks
        hunks = re.split(r"(?m)^(?=@@ )", chunk)
        for hk in hunks[1:]:
            hdr = hk.splitlines()[0]
            body = hk.splitlines()[1:]
            tok = ",".join(sorted({t for t in ("AC-22","AC-23","AC-24","AC-28",
                                               "Step 11","Step 12","CLI","PROVEN") 
                                   if any(t in b for b in body)}))
            adds = [b for b in body if b.startswith("+")]
            dels = [b for b in body if b.startswith("-")]
            w("     HUNK %-40s | tokens: %s | +%d -%d" %
              (hdr, tok, len(adds), len(dels)))
            for b in adds[:4]:
                w("         + " + b[1:][:150])
            for b in dels[:2]:
                w("         - " + b[1:][:150])

w("")
w("=== git diff --check (whole repo) ===")
res = r(["diff", "--check"])
w("  exit=%d" % res.returncode)
w("  %s" % res.stdout)

w("")
w("=== git status --short ===")
w("  %s" % r(["status", "--short"]).stdout)

io.open(OUT, "w", encoding="utf-8").write("\n".join(buf))
print("WROTE", OUT, "bytes", os.path.getsize(OUT))
