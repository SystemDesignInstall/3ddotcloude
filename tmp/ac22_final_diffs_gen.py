# -*- coding: utf-8 -*-
import subprocess, io
REPO=r'C:\Code\ClaudeDot\spatial-platform'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
OBS=r'tests/CMakeLists.txt'
MIL=r'docs/architecture/P3-milestone-status.md'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
for f,lab in [(REP,'REPORT'),(OBS,'tests/CMakeLists.txt'),(MIL,'MILESTONE')]:
    d=git('diff','fbef42b','72b88d2','--',f)
    p('##### %s : diff fbef42b..72b88d2 (%d bytes) #####' % (lab,len(d.encode('utf-8'))))
    p(d)
    p('')
open(r'tmp\ac22_final_diffs.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_final_diffs.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
