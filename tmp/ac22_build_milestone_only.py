# -*- coding: utf-8 -*-
"""Build the AC-22-ONLY milestone for the rewrite.

Evidence (authoritative, live repo):
  - PARENT fbef42b milestone L136 (Step 11, PROVEN) ends: "...AC-22 (NOT started per GO)."
  - HEAD 72b88d2 milestone = PARENT + only one hunk @@ -134,6 +134,39 @@ (add-only, del=0) that
    appends the Step-12 CLI paragraph and flips AC-22 to PROVEN inside Step-12's tail.
  - User GO: rewrite 72b88d2 so HEAD = AC-22 twin test + AC-22 flips in the TWO verification
    docs, with Step-12 CLI E2E deferred (uncommitted).

Therefore the AC-22-ONLY milestone = PARENT milestone with ONLY the Step-11 tail flipped:
  "Remaining P3.1 items: AC-22 (NOT started per GO)."
      -> the AC-22 PROVEN twin statement (NO Step-12 / CLI content).
No Step-12 CLI paragraph appears in the committed milestone.
"""
import io, sys, subprocess

REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL = r'docs/architecture/P3-milestone-status.md'
REP = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TWIN = r'tests/unit/test_p3_trajectory_optimization.cpp'

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8', 'replace')

def show(rev, path):
    return git('show', rev + ':' + path)

def lines(s):
    return s.split('\n')

PAR = 'fbef42b'
HEAD = '72b88d2'

# ---- 1. Parent milestone: find the ONE AC-22 (NOT started per GO) anchor ----
pmil = show(PAR, MIL)
# locate the NotStarted sentence; expected at very end of the Step-11 bullet
import re
needle_old = 'Remaining P3.1 items: AC-22 (NOT started per GO).'
hits = [m.start() for m in re.finditer(re.escape(needle_old), pmil)]
assert len(hits) == 1, "expected exactly one AC-22 NotStarted anchor, got %d" % len(hits)

# tail context around the anchor (to keep the surrounding Step-11 prose intact)
i = hits[0]
ctx_before = pmil[max(0, i - 80):i]
ctx_after = pmil[i + len(needle_old):i + len(needle_old) + 40]
print('anchor@%d' % i)
print('before…%r' % (ctx_before[-80:],))
print('after…%r' % (ctx_after[:40],))

# ---- 2. The AC-22 PROVEN tail (reuse the exact proven wording from HEAD's Step-12 tail ----
# but WITHOUT any Step-12/CLI references, since Step 12 CLI is deferred in this commit.
headmil = show(HEAD, MIL)
proven_m = re.search(r'AC-22 \(PROVEN[^)]*\)', headmil)
assert proven_m, 'could not extract AC-22 PROVEN from HEAD milestone'
print('HEAD PROVEN tail: %s' % proven_m.group(0)[:200])

# AC-22-only proven text (no CLI/Step-12 mention):
proven_only = ('AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` '
               'twin — two fresh `Engine::RunPipeline` invocations through REAL GTSAM seams, '
               'semantic equivalence asserted, N5 duplicate-refusal re-proven; Debug EXIT 0 '
               'Release EXIT 0; full trajectory 23/23 in both configs).')

new_mil = pmil[:i] + 'Remaining P3.1 items: ' + proven_only + pmil[i + len(needle_old):]
# sanity: no Step-12 paragraph, one AC-22, no 'Step 12'
assert 'AC-22 (NOT started' not in new_mil
assert 'Step 12' not in new_mil

with io.open(r'C:\Code\ClaudeDot\spatial-platform\tmp\ac22_milestone_only.md', 'w',
             encoding='utf-8', newline='') as f:
    f.write(new_mil)
print('wrote tmp/ac22_milestone_only.md bytes=%d' % len(new_mil.encode('utf-8')))
print('parent milestone bytes=%d' % len(pmil.encode('utf-8')))
