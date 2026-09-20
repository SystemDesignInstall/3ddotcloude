import io, subprocess, os

REPO = r"C:\Code\ClaudeDOT\spatial-platform"
FILES = [
    "docs/architecture/P3-milestone-status.md",
    "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
]
OUT = os.path.join(REPO, "ac22_commit_preview.txt")

buf = []
w = buf.append

w("git rev-parse --short HEAD = %s" % subprocess.run(
    ["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True,
    text=True).stdout.strip())
w("git status --short:")
for line in subprocess.run(["git", "status", "--short"], cwd=REPO,
                           capture_output=True, text=True).stdout.splitlines():
    w("  " + line)
w("")

for f in FILES:
    r = subprocess.run(["git", "diff", "--unified=3", "--", f], cwd=REPO,
                       capture_output=True, text=True, errors="replace")
    w("=" * 80)
    w("FILE: " + f)
    w("=" * 80)
    w(r.stdout)

io.open(OUT, "w", encoding="utf-8").write("\n".join(buf))
print("WROTE", OUT, "bytes=", os.path.getsize(OUT))
