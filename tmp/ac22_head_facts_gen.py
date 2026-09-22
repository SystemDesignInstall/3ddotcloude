# -*- coding: utf-8 -*-
import subprocess, io
REPO=r'C:\Code\ClaudeDot\spatial-platform'
M=r'docs/architecture/P3-milestone-status.md'
def show(rev):
    return subprocess.run(['git','show',rev+':'+M],cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
for rev,label in [('fbef42b','PARENT'),('72b88d2','HEAD')]:
    t=show(rev); L=t.split('\n')
    p('========== @%s (%s) total_lines=%d ==========' % (rev,label,len(L)-1))
    for i,ln in enumerate(L,1):
        if 'AC-22' in ln:
            # only show interesting: which line & the AC-22 context
            import re
            for m in re.finditer('AC-22', ln):
                a=max(0,m.start()-60); st=m.start()
                seg=ln[a:min(len(ln),st+80)]
                p('  L%d @off%d: ...%s...' % (i, st, seg))
    p('')
    # Step-12 presence
    p('  Step-12 present=%s ; "Step 12" count=%d' % ('yes' if any('Step 12' in l for l in L) else 'no', sum('Step 12' in l for l in L)))
    # Full Step-12 paragraph if present
    for i,ln in enumerate(L,1):
        if 'Step 12' in ln and ln.startswith('- **Step 12'):
            p('  ==== FULL Step-12 paragraph @L%d (%d chars) ====' % (i,len(ln)))
            p(ln)
    p('')
open(r'tmp\ac22_head_facts.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE %d bytes' % len(out.getvalue().encode('utf-8')))
