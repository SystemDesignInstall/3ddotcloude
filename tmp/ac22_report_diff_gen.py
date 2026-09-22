# -*- coding: utf-8 -*-
import subprocess, io
REPO=r'C:\Code\ClaudeDot\spatial-platform'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
MIL=r'docs/architecture/P3-milestone-status.md'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
out=io.StringIO(); p=lambda *x: print(*x,file=out)
d=git('diff','fbef42b','72b88d2','--',REP)
p('===== REPORT full diff fbef42b..72b88d2 =====')
for l in d.split('\n'):
    p(l if len(l)<4000 else l[:3990]+'...[+%d]'%(len(l)-3990))
p('')
open(r'tmp\ac22_report_full_diff.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE bytes=%d' % len(out.getvalue().encode('utf-8')))
