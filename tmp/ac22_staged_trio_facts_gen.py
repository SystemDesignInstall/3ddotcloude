# -*- coding: utf-8 -*-
import subprocess, io, os, re
REPO=r'C:\Code\ClaudeDot\spatial-platform'
def git(*a):
    r=subprocess.run(['git']+list(a),cwd=REPO,capture_output=True)
    if r.returncode!=0:
        raise RuntimeError('git %s -> %s'%(a,r.stderr.decode('utf-8','replace')))
    return r.stdout
def staged(p):
    return git('show',':0:'+p)
def headblob(p):
    return git('show','HEAD:'+p)
def staged(p):
    ok,out=subprocess.run(['git','show',':0:'+p],cwd=REPO,capture_output=True)
    return out
def md5b(b):
    import hashlib; return hashlib.md5(b).hexdigest()
def tos(b):
    return b.decode('utf-8','replace')
def lines(b):
    return b.count(b'\n')+1
out=io.StringIO()
def pp(*a):
    print(*a,file=out)
FILES={
 'milestone': 'docs/architecture/P3-milestone-status.md',
 'report'   : 'docs/architecture/P3.1-production-sparse-correction-verification-report.md',
 'test'     : 'tests/unit/test_p3_trajectory_optimization.cpp'}
pp('===== A) STAGED trio: byte-truth + step12/cli leak pins =====')
for k,p in FILES.items():
    b=staged(p)
    s=tos(b)
    pp('%-9s staged: bytes=%d lines=%d md5=%s'%(k,len(b),lines(b),md5b(b)))
    pp('         Step-12-para=%-5s  CLI-refs=%-5d  AC22-TWIN-anchor=%s'
         %('Step 12' in s, sum(1 for l in s.split('\n') if 'CLI' in l or 'E2E' in l),
           'DeterministicSecondRunThroughProductionSurface' in s))
    # AC-22 row/anchors
    pp('         milestone PROVEN-flip   =%s' % ('AC-22 (PROVEN' in s))
    pp('         report AC-22-PROVEN row =%s' % ('| AC-22: Deterministic second run' in s and 'PROVEN |' in s))
    pp('         NOT-started residue     =%s' % ('AC-22 (NOT started per GO)' in s or 'AC-22 (NOT started per GO)' in s))
pp('')
pp('===== B) CROSS-CHECK staged vs pinned md5s (aims: milestone=d5376a25, report=?, test=?) =====')
for k,p in FILES.items():
    b=staged(p)
    aim={'milestone':'d5376a25','report':'HEAD blob','test':'HEAD blob'}[k]
    pp('  %-9s staged-md5=%s  aim=%s  match=%s'%(k,md5b(b)[:16],aim,md5b(b)[:8]==aim[:8] if aim not in('HEAD blob',) else '?'))

open(os.path.join(REPO,'tmp','ac22_staged_trio_facts.txt'),'w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_staged_trio_facts.txt bytes=%d'%len(out.getvalue().encode('utf-8')))
