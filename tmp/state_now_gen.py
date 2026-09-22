# -*- coding: utf-8 -*-
import subprocess, io, os
REPO=r'C:\Code\ClaudeDot\spatial-platform'
out=io.StringIO(); p=lambda *a: print(*a,file=out)
def git(*a):
    r=subprocess.run(['git']+list(a),cwd=REPO,capture_output=True)
    return r.stdout.decode('utf-8','replace') if r.returncode==0 else r.stderr.decode('utf-8','replace')
p('HEAD      = %s' % git('rev-parse','HEAD').strip())
p('HEAD^     = %s' % git('rev-parse','HEAD^').strip())
p('branch    = %s' % git('rev-parse','--abbrev-ref','HEAD').strip())
p('')
p('===== git status --short =====')
p(git('status','--short'))
p('')
p('===== staged (cached) =====')
c=git('diff','--cached','--name-only').strip()
p(c if c else '(none)')
p('')
# which revs exist
p('fbef42b exists =', git('cat-file','-t','fbef42b').strip()!='')
p('72b88d2 exists =', git('cat-file','-t','72b88d2').strip()!='')
open(r'tmp\state_now.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/state_now.txt')
