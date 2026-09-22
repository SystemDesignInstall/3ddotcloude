# -*- coding: utf-8 -*-
# VERIFY the two AC-22-only tmp docs against parent/HEAD blobs; also assert twin test purity.
import subprocess, io, os
REPO=r'C:\Code\ClaudeDot\spatial-platform'
MIL=r'docs/architecture/P3-milestone-status.md'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST=r'tests/unit/test_p3_trajectory_optimization.cpp'
PARENT='fbef42b'; HEAD='72b88d2'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout
def blob(rev,p): return git('show',rev+':'+p).decode('utf-8','replace')
def rb(p):
    with open(os.path.join(REPO,p),'rb') as f: return f.read()
def wb(p,b):
    with open(os.path.join(REPO,p),'wb') as f: f.write(b)
def git_text(*a): return git(*a).decode('utf-8','replace')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
pm=blob(PARENT,MIL); pr=blob(PARENT,REP)
hm=blob(HEAD,MIL);  hr=blob(HEAD,REP); ht=blob(HEAD,TST)

# 1) milestone-only vs parent: single-flip, no Step-12
only_mil_fn=os.path.join(REPO,'tmp','ac22_milestone_only.md')
if os.path.exists(only_mil_fn):
    only_mil=open(only_mil_fn,'r',encoding='utf-8').read()
    p('tmp/ac22_milestone_only.md: bytes=%d lines=%d' % (len(only_mil.encode('utf-8')), only_mil.count('\n')+1))
    p('  == parent (lines=%d)?. %s' % (pm.count('\n')+1, 'Step 12 not in only_mil = %s' % ('Step 12' not in only_mil)))
    p('  AC-22 anchor PROVEN-in-only = %s' % ('AC-22 (PROVEN 2026-09-12' in only_mil))
    p('  AC-22 NOT-started-gone     = %s' % ('AC-22 (NOT started per GO)' not in only_mil))
else:
    p('MISSING tmp/ac22_milestone_only.md')
p('')

# 2) report-only: single row flip
only_rep_fn=os.path.join(REPO,'tmp','ac22_report_only.md')
if os.path.exists(only_rep_fn):
    only_rep=open(only_rep_fn,'r',encoding='utf-8').read()
    p('tmp/ac22_report_only.md: bytes=%d lines=%d' % (len(only_rep.encode('utf-8')), only_rep.count('\n')+1))
else:
    p('MISSING tmp/ac22_report_only.md')
p('')
# 3) twin test file at HEAD: does it contain Step-12/CLI refs? (must be AC-22 twin ONLY for commit A)
p('HEAD twin test bytes=%d lines=%d' % (len(ht.encode('utf-8')), ht.count('\n')+1))
p('  contains AC22 twin anchor : %s' % ('AC22_DeterministicSecondRunThroughProductionSurface' in ht))
p('  contains "Step 12"        : %s' % ('Step 12' in ht))
p('  contains "CLI"            : %s' % ('CLI' in ht))
p('  contains "cli/"           : %s' % ('cli/' in ht))
open(os.path.join(REPO,'tmp','ac22_verify_split_tmp.txt'),'w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE', os.path.join('tmp','ac22_verify_split_tmp.txt'), 'bytes=%d' % len(out.getvalue().encode('utf-8')))
