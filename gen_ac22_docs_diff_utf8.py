# -*- coding: utf-8 -*-
import io, os, subprocess, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO = r"C:\Code\ClaudeDot\spatial-platform"
DOCS = [
    "docs/architecture/P3-milestone-status.md",
    "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
]

def git_diff(path):
    r = subprocess.run(["git", "diff", "--", path], cwd=REPO,
                       capture_output=True, text=True, errors="replace")
    return r.stdout

out_path = os.path.join(REPO, "ac22_docs_diff_utf8.txt")
buf = []
for d in DOCS:
    buf.append("##### FILE: " + d)
    buf.append(git_diff(d))
    buf.append("")
with io.open(out_path, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(buf))
print("bytes (utf-8):", os.path.getsize(out_path))
