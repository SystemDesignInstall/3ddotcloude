# -*- coding: utf-8 -*-
import subprocess, io, os
REPO=r'C:\Code\ClaudeDot\spatial-platform'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
PARENT='fbef42b'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout
pr=git('show',PARENT+':'+REP).decode('utf-8','replace')
lines=pr.split('\n')
out=io.StringIO(); p=lambda *a: print(*a,file=out)
p('parent report total lines=%d' % (len(lines)-1))
p('=== full AC-22 rows in PARENT report ===')
for i,l in enumerate(lines,1):
    if 'AC-22' in l:
        p('  L%d | %s' % (i,l))
# Also: find the row that says AC-22 NOT PROVEN
anchor='| AC-22: Deterministic second run |'
cnt=pr.count(anchor)
p('AC-22 row anchor count = %d' % cnt)
open(r'tmp\ac22_report_parent_rows.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE')
