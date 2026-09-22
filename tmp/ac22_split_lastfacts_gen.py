# -*- coding: utf-8 -*-
import subprocess, io, re, os
REPO=r'C:\Code\ClaudeDot\spatial-platform'
PARENT='fbef42b'; HEAD='72b88d2'
MIL=r'docs/architecture/P3-milestone-status.md'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
def blob(rev,p): return git('show',rev+':'+p)
out=io.StringIO(); p=lambda *x: print(*x,file=out)

p('========== A) REPORT diff fbef42b..72b88d2 : full +/- ==========')
d=git('diff',PARENT,HEAD,'--',REP)
for l in d.split('\n'):
    if l.startswith(('+','-')) and not l.startswith(('+++','---')):
        p('  %s' % (l[:250]+('…' if len(l)>250 else '')))
p('')
p('========== B) PARENT milestone: region after Step-11 (L136) ==========')
pm=blob(PARENT,MIL); PL=pm.split('\n')
p('parent total lines=%d' % (len(PL)-1))
for i in range(135, min(len(PL)-1,145)):
    l=PL[i]
    p('  L%d(%d)|%s' % (i+1,len(l),(l[:130]+'…') if len(l)>130 else l))
p('')
p('========== C) HEAD milestone Step-12 paragraph: full exact lines ==========')
hm=blob(HEAD,MIL); HL=hm.split('\n')
p('head total lines=%d' % (len(HL)-1))
# Step-12 paragraph = from first line containing '**Step 12' to before '---'/'## Next'
start=next(i for i,l in enumerate(HL) if 'Step 12' in l)
# find end: next '---' at line start after start, else EOF
end=next((i for i in range(start+1,len(HL)) if HL[i].strip()=='---'), len(HL)-1)
p('Step-12 paragraph spans HEAD lines %d..%d (1-indexed %d..%d)' % (start+1,end,start+1,end))
# I want the INSERT position in the parent: after Step-11 line (parent L136)
# and also need to check whether HEAD Step-11 line == parent Step-11 line
p('HEAD Step-11 line == parent Step-11 line : %s' % (HL[135]==PL[135]))
p('HEAD L137 first chars: %r' % HL[136][:120])
p('HEAD last-4 lines:')
for l in HL[-4:]: p('   |%s' % (l[:120]+'…' if len(l)>120 else l))
p('')
# Step-12 paragraph content (all lines, with lengths)
for i in range(start,end):
    p('  HL%d(%d)%s' % (i+1,len(HL[i]),(' | '+HL[i][:150]+'…') if len(HL[i])>150 else (' | '+HL[i])))
p('')
p('========== D) REPORT: HEAD AC-22 PROVEN row vs parent NOT-PROVEN row ==========')
pr=blob(PARENT,REP).split('\n')
hr=blob(HEAD,REP).split('\n')
for i,(a,b) in enumerate(zip(pr,hr)):
    if 'AC-22' in a or 'AC-22' in b:
        p('  REPORT L%d  PARENT:%r' % (i+1,a[:200]))
        p('  REPORT L%d  HEAD:  %r' % (i+1,b[:200]))
p('')
open(r'tmp\ac22_split_lastfacts.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE bytes=%d' % len(out.getvalue().encode('utf-8')))
