# -*- coding: utf-8 -*-
# ONE decisive probe: AC-22 anchor exact bytes at PARENT in BOTH milestone & report,
# and the flipped AC-22 PROVEN row at HEAD in BOTH docs — via git blobs, no git add.
import subprocess, io, os
REPO=r'C:\Code\ClaudeDot\spatial-platform'
MIL=r'docs/architecture/P3-milestone-status.md'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
PARENT='fbef42b'; HEAD='72b88d2'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout
def blob(rev,p): return git('show',rev+':'+p).decode('utf-8','replace')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
pm=blob(PARENT,MIL); hm=blob(HEAD,MIL); pr=blob(PARENT,REP); hr=blob(HEAD,REP)
# Not-started anchors (parent) + PROVEN (head), byte context
for tag,s in (('MIL-PARENT',pm),('MIL-HEAD',hm)):
    lines=s.split('\n')
    p('== %s lines=%d ==' % (tag,len(lines)-1))
    for i,l in enumerate(lines,1):
        if 'Remaining P3.1' in l and 'AC-22' in l:
            p('  L%d: %s' % (i, l[-230:]))
    p('')
p('')
for tag,s in (('REP-PARENT',pr),('REP-HEAD',hr)):
    lines=s.split('\n')
    p('== %s lines=%d ==' % (tag,len(lines)-1))
    for i,l in enumerate(lines,1):
        if 'AC-22' in l and '|' in l:
            p('  L%d: %s' % (i,l[:170]))
    p('')
wb=lambda f,t: open(os.path.join(REPO,f),'w',encoding='utf-8',newline='\n').write(t)
wb('tmp/ac22_anchor_final.txt', out.getvalue())
print('WROTE bytes=%d' % len(out.getvalue().encode('utf-8')))
