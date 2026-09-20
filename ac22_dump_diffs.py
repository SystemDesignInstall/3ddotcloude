import io, re, subprocess

repo = r"C:\Code\ClaudeDOT\spatial-platform"
files = [
    "docs/architecture/P3-milestone-status.md",
    "docs/architecture/P3.1-production-sparse-correction-verification-report.md",
]
for f in files:
    d = subprocess.run(["git", "diff", "--", f], cwd=repo,
                       capture_output=True, text=True, errors="replace").stdout
    print("=" * 20, f, "=" * 20)
    hunks = re.split(r"(?m)^(?=@@ )", d)
    for h in hunks:
        if not h.startswith("@@"):
            continue
        lines = h.splitlines()
        head = lines[0]
        body = "\n".join(lines[1:])
        has_ac22 = "AC-22" in body
        has_step12 = "Step 12" in body
        has_step11 = "Step 11" in body
        has_cli = "CLI" in body
        added = [l for l in lines[1:] if l.startswith("+")]
        removed = [l for l in lines[1:] if l.startswith("-")]
        print("  HUNK:", head)
        print("    tokens: AC22=%s Step12=%s Step11=%s CLI=%s" % (
            has_ac22, has_step12, has_step11, has_cli))
        for l in added[:6]:
            print("      + " + l[1:][:150])
        for l in removed[:4]:
            print("      - " + l[1:][:150])
