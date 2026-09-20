# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
MIL = "docs/architecture/P3-milestone-status.md"
REP = "docs/architecture/P3.1-production-sparse-correction-verification-report.md"
FDIR = "ac22_fresh"

def run(args, cwd=REPO):
    return subprocess.run(args, cwd=cwd, capture_output=True)

def hunk_tokens(txt):
    # split into hunks by lines starting with @@
    toks=[]
    cur=set()
    for ln in txt.splitlines():
        if ln.startswith("@@"):
            if cur: toks.append("|".join(sorted(cur)))
            cur=set()
        low=ln.lower()
        if "ac-22" in low: cur.add("AC22")
        if "step 12" in low or "step12" in low: cur.add("Step12")
        if "cli" in low or "sole user" in low: cur.add("CLI")
        if "e2e" in low: cur.add("E2E")
    if cur: toks.append("|".join(sorted(cur)))
    return toks

os.makedirs(os.path.join(REPO, FDIR), exist_ok=True)

# full diffs to separate files
for name, doc in (("milestone", MIL), ("report", REP)):
    r = run(["git", "diff", "HEAD", "--", doc])
    p = os.path.join(REPO, FDIR, "diff_%s.txt" % name)
    io.open(p, "wb").write(r.stdout)
    print("WROTE", p, len(r.stdout))

# summary file
out=[]
out.append("## FRESH ground truth (git) ##")
out.append("")
r = run(["git", "log", "--oneline", "-8"]); out.append("### log -8 ###"); out.append(r.stdout.decode("utf-8","replace").strip()); out.append("")
r = run(["git", "status", "--short"]); out.append("### status --short ###"); out.append(r.stdout.decode("utf-8","replace").strip()); out.append("")
r = run(["git", "diff", "--stat", "HEAD"]); out.append("### diff --stat HEAD ###"); out.append(r.stdout.decode("utf-8","replace").strip()); out.append("")
out.append("### MILESTONE hunk tokens ###")
out.append("\n".join(hunk_tokens(run(["git","diff","HEAD","--",MIL]).stdout.decode("utf-8","replace"))))
out.append("")
out.append("### REPORT hunk tokens ###")
out.append("\n".join(hunk_tokens(run(["git","diff","HEAD","--",REP]).stdout.decode("utf-8","replace"))))
out.append("")
r = run(["git", "diff", "--check"]); out.append("### diff --check rc=%d stderr=%r ###" % (r.returncode, r.stderr.decode("utf-8","replace")))
p = os.path.join(REPO, FDIR, "summary.txt")
io.open(p, "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("WROTE", p, len("\n".join(out)))
