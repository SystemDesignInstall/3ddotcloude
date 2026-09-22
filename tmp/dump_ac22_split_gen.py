# -*- coding: utf-8 -*-
# DEFINITIVE dump of the exact lines I must flip/split, written to tmp/dump_ac22_split.txt
import subprocess, io
REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL = r'docs/architecture/P3-milestone-status.md'
REP = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
def show(rev, p):
    return subprocess.run(['git','show', rev+':'+p], cwd=REPO, capture_output=True).stdout.decode('utf-8','replace')
def git(*a):
    return subprocess.run(['git']+list(a), cwd=REPO, capture_output=True).stdout.decode('utf-8','replace')

out = io.StringIO()
def p(*a): print(*a, file=out)

p('===== MILESTONE @PARENT(fbef42b): full Step-11 line L136 =====')
P = show('fbef42b', MIL)
PL = P.split('\n')
p('parent milestone full lines=%d' % (len(PL)-1) if not PL[-1] else 'parent lines=%d' % len(PL))
ln = PL[135]
p('L136 len=%d' % len(ln))
p('L136 FULL:')
p(ln)
p('')
p('--- parent: does Step-11 tail mention AC-22 NOT started? ---')
p('   "AC-22 (NOT started per GO)" in L136:', 'AC-22 (NOT started per GO)' in ln)
p('   Step 12 anywhere in parent milestone :', 'Step 12' in P)
p('')

p('===== MILESTONE @HEAD(72b88d2): Step-11 L136 + where AC-22 PROVEN lives =====')
H = show('72b88d2', MIL)
HL = H.split('\n')
p('head milestone full lines=%d' % (len(HL)-1 if not HL[-1] else len(HL)))
h136 = HL[135]
p('H136 len=%d' % len(h136))
p('H136 FULL:')
p(h136)
p('   does H136 keep "AC-22 (NOT started per GO)"? ', 'AC-22 (NOT started per GO)' in h136)
p('   Step 12 present? ', 'Step 12' in H)
p('')
# Where is AC-22 PROVEN in the head milestone:
import re
for i, l in enumerate(HL, 1):
    if 'AC-22 (PROVEN' in l or 'AC-22 (PROVEN' in l or 'PROVEN 2026-09-12' in l:
        p('  AC-22-PROVEN candidate @ L%d (partial 200): %s' % (i, l[:200]))
p('')

p('===== REPORT @PARENT vs @HEAD: AC-22 row =====')
RR = show('fbef42b', REP); RH = show('72b88d2', REP)
rpl = RR.split('\n'); rhl = RH.split('\n')
p('report parent lines=%d  head lines=%d' % (len(rpl), len(rhl)))
for i,(a,b) in enumerate(zip(rpl, rhl), 1):
    if 'AC-22' in a or 'AC-22' in b:
        p('  @%d PARENT: %s' % (i, a))
        p('  @%d HEAD  : %s' % (i, b))
p('')

open(r'tmp\dump_ac22_split.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/dump_ac22_split.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
