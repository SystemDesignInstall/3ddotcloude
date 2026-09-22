import subprocess, re, sys
REPO=r'C:\Code\ClaudeDot\spatial-platform'
M=r'docs/architecture/P3-milestone-status.md'
def show(rev):
    return subprocess.run(['git','show',rev+':'+M],cwd=REPO,capture_output=True).stdout.decode('utf-8')
def analyze(rev,label):
    t=show(rev)
    print('===== %s @ %s : AC-22/status occurrences =====' % (label,rev))
    for pat in [r'AC-22 \(', r'NOT started per GO', r'PROVEN 2026', r'Remaining P3\.1 items:']:
        cnt=t.count(pat)
        print('  count %-22s = %d' % (pat,cnt))
    # line 136 tail for both
    ls=t.split('\n')
    print('  total lines=%d' % len(ls))
    l=ls[135]
    print('  L136 len=%d last120=%r' % (len(l), l[-120:]))
    # where in L136 does AC-22 appear?
    for m in re.finditer(r'AC-22', l):
        print('    AC-22 @L136 offset %d.. context: ...%s...' % (m.start(), l[max(0,m.start()-40):m.start()+90]))
    print()
analyze('fbef42b','PARENT')
analyze('72b88d2','HEAD')
