# -*- coding: utf-8 -*-
"""Deterministic split: produce AC-22-ONLY versions of the TWO P3 docs.

Committed HEAD = 72b88d2 (the AC-22 twin + Step-12 CLI E2E blob), parent = fbef42b.

Goal of the whole session: split 72b88d2 so that ONE part (the AC-22 deterministic-second-run
twin) becomes its own committed milestone while the Step-12 CLI E2E work is DEFERRED back to
uncommitted (this is the user's explicit plan: the over-flagging go-decision was repudiated,
Step 11's AC-22 twin is the PROVEN surface, Step 12 CLI E2E returns to "deferred/uncommitted").

This generator ONLY writes two markdown outputs (AC-22-ONLY milestone + AC-22-ONLY report)
to tmp/, derived byte-exactly from the parent blobs. The actual git split (reset/stage/commit =
EXACTLY the AC-22 flip, leaving Step-12 CLI E2E uncommitted again) is done by a SEPARATE
executor script with hard assertions. Nothing in this generator touches git state.
"""
import subprocess, io, os, sys
REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL = r'docs/architecture/P3-milestone-status.md'
REP = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
PARENT = 'fbef42b'

def blob(rev, p):
    r = subprocess.run(['git', 'show', rev + ':' + p], cwd=REPO, capture_output=True)
    assert r.returncode == 0, 'git show %s:%s failed' % (rev, p)
    return r.stdout.decode('utf-8')

parent_mil = blob(PARENT, MIL)
parent_rep = blob(PARENT, REP)

# ===========================================================================
# 1) MILESTONE: parent + ONLY the single-flag flip on Step-11's tail.
#    Parent Step-11 (L136) ends:
#      "...Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO)."
#    Flip ONLY that AC-22 flag to PROVEN-twai (the 2026-09-12 twin run proof). NO Step 12.
old_anchor = 'Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).'
assert parent_mil.count(old_anchor) == 1, 'parent milestone anchor must be unique (count=%d)' % parent_mil.count(old_anchor)
new_anchor = ('Remaining P3.1 acceptance items: CLI real providers, AC-22 (PROVEN 2026-09-12: '
              '`AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh '
              '`Engine::RunPipeline("p3_sparse_correction", \u2026)` invocations through REAL GTSAM '
              'seams (two fresh projects, bit-identical pinned inputs), semantic twin equivalence '
              'asserted with exact equality + twin small absolute tolerances, N5 duplicate-refusal '
              're-proven through the production surface; Debug EXIT 0 Release EXIT 0; full '
              'trajectory 23/23 in both).')
ac22_mil = parent_mil.replace(old_anchor, new_anchor, 1)
assert 'Remaining P3.1 acceptance items: CLI real providers, AC-22 (PROVEN' in ac22_mil
assert 'AC-22 (NOT started per GO)' not in ac22_mil
assert 'Step 12' not in ac22_mil, 'AC-22-only milestone MUST NOT contain Step 12'

# ===========================================================================
# 2) REPORT: parent + ONLY the AC-22 row flip (NOT PROVEN -> PROVEN twin).
old_row = '| AC-22: Deterministic second run | No double-run | NOT PROVEN |'
assert parent_rep.count(old_row) == 1, 'parent report AC-22 row must be unique (count=%d)' % parent_rep.count(old_row)
new_row = ('| AC-22: Deterministic second run | Twin double-run via `AC22_DeterministicSecondRunThroughProductionSurface` '
           '(two fresh `Engine::RunPipeline("p3_sparse_correction")` invocations through REAL GTSAM seams, '
           'semantic twin equivalence, N5 duplicate-refusal re-proven), Debug EXIT 0 + Release EXIT 0 | PROVEN |')
ac22_rep = parent_rep.replace(old_row, new_row, 1)
assert '| AC-22: Deterministic second run |' in ac22_rep and '| PROVEN |' in ac22_rep
assert 'No double-run' not in ac22_rep

# ===========================================================================
# 3) Write both to tmp/ (byte-exact, LF, UTF-8) for the executor.
import os
os.makedirs(os.path.join(REPO, 'tmp'), exist_ok=True)
for name, content in (('ac22_milestone_only.md', ac22_mil),
                      ('ac22_report_only.md', ac22_rep)):
    p = os.path.join(REPO, 'tmp', name)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    print('WROTE tmp/%s  bytes=%d lines=%d' % (name, len(content.encode('utf-8')), content.count('\n') + 1))

print('AC-22-only milestone: Step 12 present = %s' % ('Step 12' in ac22_mil))
print('AC-22-only report   : AC-22 PROVEN   = %s' % ('| PROVEN |' in ac22_rep))
print('Verified AC-22 flip anchors are byte-exact against parent blobs.')
