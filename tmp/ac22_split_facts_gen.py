# -*- coding: utf-8 -*-
import subprocess, io, os, re
REPO=r'C:\Code\ClaudeDot\spatial-platform'
MIL=r'docs/architecture/P3-milestone-status.md'
REP=r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
def git(*a): return subprocess.run(['git']+list(a),cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
def blob(rev,p): return git('show',rev+':'+p)

out=io.StringIO(); p=lambda *a: print(*a,file=out)

# 1) EXACT parent Step-11 line (L136) - show full tail around AC-22 + exact full line len
parent_mil=blob('fbef42b',MIL)
L=parent_mil.split('\n')
l136=L[135]
p('PARENT Step-11 L136: full_len=%d' % len(l136))
i=l136.find('AC-22 (NOT started per GO)')
p('  AC-22 NOT-started anchor @ offset %d' % i)
p('  contextual tail: [%s]' % l136[-120:])
# full tail from "Remaining P3.1 acceptance items:"
j=l136.find('Remaining P3.1 acceptance items:')
p('  FULL tail-from-anchor: [%s]' % l136[j:])
p('')
# exact REPLACE fragment to construct AC-22-only tail
old='Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).'
p('REPLACE-OLD exact-match-in-L136 = %s' % (old in l136))
new_tail='Remaining P3.1 acceptance items: CLI real providers, AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh `Engine::RunPipeline` invocations through REAL GTSAM seams, semantic equivalence asserted, N5 duplicate-refusal re-proven; Debug EXIT 0 Release EXIT 0; full trajectory 23/23 in both).'
ac22_mil=parent_mil.replace(old,new_tail,1)
p('new_tail in ac22_mil=%s ; old gone=%s' % (new_tail in ac22_mil, old not in ac22_mil))
p('ac22_mil: bytes=%d lines=%d' % (len(ac22_mil.encode('utf-8')), ac22_mil.count(chr(10))+1))
p('Step-12 present in ac22_mil = %s (must be False)' % ('Step 12' in ac22_mil))
open(r'tmp\ac22_milestone_only.md','w',encoding='utf-8',newline='').write(ac22_mil)
p('WROTE tmp/ac22_milestone_only.md')

# 2) Report AC-22 single-row flip check (pure)
d=git('diff','fbef42b','72b88d2','--',REP)
rows=[x for x in d.split('\n') if x.startswith('+') and 'AC-22' in x]
p('REPORT AC-22 rows added: %d' % len(rows))
for r in rows: p('  + %s' % r[:220])
p('')

# 3) New test file 72b88d2 vs parent - is it PURE ADD? which filename?
ts=git('diff','--name-status','fbef42b','72b88d2')
p('ALL changed paths fbef42b..72b88d2:')
for x in ts.split('\n'): p('  %s' % x)
p('')
p('LOG fbef42b..72b88d2 messages:')
for l in git('log','--format=%h %s','fbef42b..72b88d2').split('\n'): p('  %s' % l)
open(r'tmp\ac22_split_facts.txt','w',encoding='utf-8',newline='').write(out.getvalue())
print('done %d bytes' % len(out.getvalue().encode('utf-8')))
