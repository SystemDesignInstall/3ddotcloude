# -*- coding: utf-8 -*-
import io, subprocess, os, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
REPO = r"C:\Code\ClaudeDot\spatial-platform"
os.chdir(REPO)
ba = subprocess.run(["git", "diff", "--", "docs/architecture/P3-milestone-status.md"],
                    capture_output=True, text=True, errors="replace").stdout
io.open(r"C:\Code\ClaudeDot\spatial-platform\ac22_milestone_utf8.diff.txt", "w",
        encoding="utf-8", newline="\n").write(ba)
print("bytes:", len(ba.encode("utf-8")))
