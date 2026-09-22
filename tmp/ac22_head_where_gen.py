# -*- coding: utf-8 -*-
import subprocess, io, os
REPO=r'C:\Code\ClaudeDOT\spatial-platform'
M='docs/architecture/P3-milestone-status.md'
def g(*a):
    return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout
out=io.StringIO(); p=lambda *a: print(*a,file=out)
def lines(rev):
    return g('show',rev+':'+M).decode('utf-8','replace').split('\n')
H=lines('72b88d2'); PL=lines('fbef42b')
p('HEAD milestone lines=%d  parent lines=%d' % (len(H)-1, len(PL)-1))
p('')
# Find every line position containing "Step 12" / "AC-22" in HEAD
p('=== HEAD: positions of "Step 12", "Step-12", "CLI", "AC-22", "PROVEN" ===')
for i,l in enumerate(H,1):
    if any(x in l for x in ('Step 12','Step-12','CLI real providers','AC-22','Remaining P3.1')):
        hits=[x for x in ('Step 12','Step-12','CLI real providers','AC-22','PROVEN','NOT started') if x in l]
        p('  L%d [%s] len=%d  %s' % (i, ','.join(hits), len(l), (l[:120]+'...') if len(l)>120 else l))
p('')
p('=== HEAD L136 (Step-11) FULL ===')
p(H[135])
p('')
p('=== HEAD: the AC-22 PROVEN text (twin paragraph) — search all lines for "AC22_DeterministicSecondRun" ===')
for i,l in enumerate(H,1):
    if 'AC22_DeterministicSecondRun' in l:
        p('  L%d starts: %s' % (i, l[:150]))
p('')
# =====================================================================
# THE key question: does HEAD milestone contain a STANDALONE Step-11 AC-22 PROVEN
# paragraph (so AC-22-only flip = that paragraph, no CLI/Step-12)? Or does AC-22
# PROVEN live ONLY inside the Step-12 CLI paragraph?
p('=== Where is AC-22 PROVEN in HEAD milestone? List all lines w/ AC-22 mention ===')
for i,l in enumerate(H,1):
    if 'AC-22' in l:
        shown = l[2:120] if l.startswith('- ') else l[:120]
        p('L%d|%s' % (i, shown))
open(r'tmp\ac22_head_where.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_head_where.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
