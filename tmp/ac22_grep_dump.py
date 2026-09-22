import subprocess, sys, io, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'

def git(*a, **k):
    return subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True).stdout.decode('utf-8', 'replace')

def grep(rev, pat):
    out = git('grep', '-n', '-E', pat, rev, '--', M)
    return out

def dump(rev, lo, hi):
    lines = git('show', rev + ':' + M).split('\n')
    out = []
    for i in range(lo, min(hi, len(lines)) + 1):
        l = lines[i-1]
        s = l if len(l) <= 120 else l[:120] + '…'
        out.append('%3d|%s' % (i, s))
    return '\n'.join(out)

for rev,label in [('fbef42b','PARENT'), ('72b88d2','HEAD')]:
    print('===== %s @%s : grep "AC-22|Step 1[12]|Remaining P3.1|NOT started|PROVEN" =====' % (label, rev))
    print(grep(rev, r'AC-22|Step 1[12].*PROVEN|Remaining P3\.1 items|NOT started per GO'))
    print()
print('===== PARENT @fbef42b tail: lines 159-165 =====')
print(dump('fbef42b', 159, 166))
print()
print('===== HEAD @72b88d2 tail: lines 159-170 =====')
print(dump('72b88d2', 159, 171))
