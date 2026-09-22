# -*- coding: utf-8 -*-
# ROOT: dump authoritative AC-22 anchor facts across BOTH milestone files, BOTH revs.
# Writes UTF-8 to ac22_anchor_truth.txt for clean reading.
import subprocess, io
REPO = r'C:\Code\ClaudeDot\spatial-platform'
ML = [r'docs/architecture/P3-milestone-status.md',
      r'docs/architecture/P3.1-production-sparse-correction-verification-report.md']
REVS = [('fbef42b','PARENT'), ('72b88d2','HEAD')]
def git(*a):
    return subprocess.run(['git']+list(a), cwd=REPO, capture_output=True).stdout.decode('utf-8','replace')
out = io.StringIO()
def p(*a): print(*a, file=out)

p('===== AC-22 anchor truth: grep each milestone file @ each rev =====')
for m in ML:
    p('##### FILE %s #####' % m.split('/')[-1])
    for rev,label in REVS:
        r = git('grep','-n','-E','AC-22|AC22|NOT started|PROVEN|PROVEN per GO|NOT started per GO|PROVEN 2026',rev,'--',m)
        p('--- @%s (%s) ---' % (rev,label))
        for line in r.split('\n'):
            if not line.strip(): continue
            # strip repo prefix
            line = line.split(':',2)
            if len(line)>=3:
                ln = line[1]; txt = line[2]
            else:
                ln=''; txt=line[-1] if line else ''
            t = txt if len(txt)<=200 else txt[:200]+'…'
            p('  L%s|%s' % (ln, t))
        p('')
    p('')

# dump the FULL milestone tail region at both revs (to design exact surgical flip)
for m in ML:
    p('####### MILESTONE %s — tail region diff-anchored #######' % m.split('/')[-1])
    for rev,label in REVS:
        lines = git('show', rev+':'+m).split('\n')
        p('  ==== @%s (%s) total=%d lines ====' % (rev,label,len(lines)-1))
        # print last 8 lines truncated 160
        for i in range(max(1,len(lines)-8), len(lines)):
            l = lines[i-1]
            s = l if len(l)<=160 else l[:160]+'…'
            p('   L%d|%s' % (i,s))
        p('')

open(r'tmp\ac22_anchor_truth.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_anchor_truth.txt  bytes=%d' % len(out.getvalue().encode('utf-8')))
