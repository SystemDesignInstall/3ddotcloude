import subprocess, sys, json

REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = 'docs/architecture/P3-milestone-status.md'

def grep(rev, pat, path=M):
    r = subprocess.run(['git', 'grep', '-n', '--break', '--heading', pat, rev, '--', path],
                       cwd=REPO, capture_output=True)
    txt = r.stdout.decode('utf-8', 'replace')
    return txt

def dump(rev, label, lo, hi):
    r = subprocess.run(['git', 'show', rev + ':' + M], cwd=REPO, capture_output=True)
    lines = r.stdout.decode('utf-8', 'replace').split('\n')
    out = []
    out.append('===== {0} @{1} lines {2}..{3} (trunc 150) ====='.format(label, rev, lo, hi))
    for i in range(lo, min(hi, len(lines)) + 1):
        l = lines[i - 1]
        s = l if len(l) <= 150 else l[:150] + '\u2026'
        out.append('{0,3}|{1}'.format(i, s))
    return '\n'.join(out)

print(grep('fbef42b', 'AC-22'))
print(grep('72b88d2', 'AC-22'))
print(dump('fbef42b', 'PARENT', 133, 172))
print(dump('72b88d2', 'HEAD', 133, 172))
