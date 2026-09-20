# -*- coding: utf-8 -*-
import io, os, subprocess

REPO = r"C:\Code\ClaudeDot\spatial-platform"
RES = os.path.join(REPO, "tmp", "ac22_milestone_full.txt")
os.makedirs(os.path.join(REPO, "tmp"), exist_ok=True)

r = subprocess.run(["git", "diff", "--",
                    "docs/architecture/P3-milestone-status.md"],
                   cwd=REPO, capture_output=True, text=True, errors="replace")
data = "STDOUT:\n" + r.stdout + "\nSTDERR:\n" + r.stderr
io.open(RES, "w", encoding="utf-8", newline="\n").write(data)
print("OK", len(data))
