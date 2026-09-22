# -*- coding: utf-8 -*-
import subprocess, io
REPO=r'C:\Code\ClaudeDot\spatial-platform'
def git(*a):
    r=subprocess.run(['git']+list(a),cwd=REPO,capture_output=True)
    return r.stdout.decode('utf-8','replace') if r.returncode==0 else r.stderr.decode('utf-8','replace')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
p('HEAD      = %s' % git('rev-parse','HEAD').strip())
p('parent^   = %s' % git('rev-parse','HEAD^').strip())
p('')
p('===== git status --short =====')
p(git('status','--short'))
p('===== staged name-only =====')
p(git('diff','--cached','--name-only') if git('diff','--cached','--name-only').strip() else '(none)')
p('')
# milestone worktree: does it still contain Step 12? and AC-22 PROVEN twin?
m=r'docs/architecture/P3-milestone-status.md'
b=open(m,'rb').read()
p('milestone worktree bytes=%d lines=%d' % (len(b), b.count(b'\n')+1))
p('  contains "Step 12"                  : %s' % (b'Step 12' in b))
p('  contains AC-22 PROVEN twin anchor   : %s' % (b'AC-22 (PROVEN' in b))
p('  contains "NOT started per GO"       : %s' % (b'NOT started per GO' in b))
p('')
# report worktree
r=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
bb=open(r,'rb').read()
p('report worktree bytes=%d lines=%d' % (len(bb), bb.count(b'\n')+1))
p('  contains AC-22 PROVEN row          : %s' % (b'AC-22: Deterministic second run' in bb and b'PROVEN' in bb))
p('')
open(r'tmp/state_head.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE')
