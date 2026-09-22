# -*- coding: utf-8 -*-
# Build AC-22-ONLY milestone = PARENT milestone (fbef42b) + single Step-11 tail flip.
import subprocess, io, sys
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def show(rev):
    r = subprocess.run(['git','show',rev+':'+M], cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8')
pmil = show('fbef42b')
# The OLD AC-22 sentence (Step-11 tail), byte-exact, MUST occur exactly once.
old = 'Remaining P3.1 items: AC-22 (NOT started per GO).'
assert pmil.count(old) == 1, 'expected exactly one AC-22 NOT-started sentence, got %d' % pmil.count(old)
new = ('Remaining P3.1 items: AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` '
       'twin — two fresh `Engine::RunPipeline` invocations through REAL GTSAM seams, semantic '
       'equivalence asserted, N5 duplicate-refusal re-proven; Debug EXIT 0 Release EXIT 0; full '
       'trajectory 23/23 (Step 11 was 22/22; the +1 is the AC-22 twin) in both configs).')
amil = pmil.replace(old, new, 1)
assert amil.count('AC-22 (NOT started') == 0
assert amil.count('AC-22 (PROVEN') == 1
assert 'Step 12 (CLI' not in amil, 'Step-12 CLI paragraph must NOT be in the AC-22-only milestone!'
with io.open(r'C:\Code\ClaudeDot\spatial-platform\tmp\ac22_milestone_only.md','w',encoding='utf-8',newline='') as f:
    f.write(amil)
print('OK ac22_milestone_only.md  bytes=%d  lines=%d' % (len(amil.encode('utf-8')), amil.count(chr(10))))
print('contains Step-12 CLI: False (asserted)')
sys.stdout.flush()
