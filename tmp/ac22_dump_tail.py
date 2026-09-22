import subprocess, sys
repo = r'C:\Code\ClaudeDot\spatial-platform'
m = 'docs/architecture/P3-milestone-status.md'
rev = sys.argv[1]
label = sys.argv[2] if len(sys.argv) > 2 else rev
rng = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (130, 174)
out = subprocess.run(['git', 'show', rev + ':' + m], cwd=repo,
                     capture_output=True, encoding='utf-8')
lines = out.stdout.splitlines()
print('===== %s @%s milestone lines %d..%d (160-char trunc) =====' % (label, rev, rng[0], rng[1]))
for i in range(rng[0], min(rng[1], len(lines)) + 1):
    if i <= len(lines):
        l = lines[i-1]
        s = l if len(l) <= 160 else l[:160] + '…'
        print('%d|%s' % (i, s))
print('')
# AC-22 / step12 / Remaining hits
print('--- hits (AC-22 / Step 12 / Remaining P3.1) ---')
for i, l in enumerate(lines, 1):
    if ('AC-22' in l or 'Step 12' in l or 'Remaining P3.1' in l):
        s = l if len(l) <= 180 else l[:180] + '…'
        print('%d|%s' % (i, s))
