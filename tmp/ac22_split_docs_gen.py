# -*- coding: utf-8 -*-
"""AC-22-only docs generator — SELF-CONTAINED, authoritative.

Builds, from the PARENT (fbef42b) git blobs only:
  * tmp/ac22_milestone_only.md  = parent milestone + ONLY the Step-11 AC-22 flip (NO Step 12)
  * tmp/ac22_report_only.md     = parent report   + ONLY the AC-22 row flip (NOT PROVEN -> PROVEN)
Byte-exact, LF, UTF-8. Also prints verification facts for the commit split.
"""
import subprocess, io, os
REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL  = r'docs/architecture/P3-milestone-status.md'
REP  = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
PARENT = 'fbef42b'

def blob(rev, p):
    r = subprocess.run(['git', 'show', rev + ':' + p], cwd=REPO, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('git show %s failed' % (rev + ':' + p))
    return r.stdout.decode('utf-8', 'replace')

def wb(p, s):
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(s)

out = io.StringIO()
def msg(*a): print(*a, file=out)

parent_mil = blob(PARENT, MIL)
parent_rep = blob(PARENT, REP)
msg('parent milestone: bytes=%d lines=%d' % (len(parent_mil.encode('utf-8')), parent_mil.count('\n') + 1))
msg('parent report   : bytes=%d lines=%d' % (len(parent_rep.encode('utf-8')), parent_rep.count('\n') + 1))

# ---------------------------------------------------------------------------
# 1) AC-22 flip in the MILESTONE Step-11 tail (L136): the AC-22 NOT-started anchor
old_anchor = 'AC-22 (NOT started per GO).'
assert parent_mil.count(old_anchor) == 1, 'milestone anchor must be unique; found %d' % parent_mil.count(old_anchor)

# The twin test that PROVES AC-22 lives at HEAD as `Engine::RunPipeline` twin block in
# tests/unit/test_p3_trajectory_optimization.cpp — the pure AC-22 addition we keep in commit A.
new_anchor = ('AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` '
              'twin \u2014 two fresh `Engine::RunPipeline("p3_sparse_correction")` invocations over '
              'bit-identical pinned inputs through REAL GTSAM seams, semantic equivalence asserted, '
              'N5 duplicate-refusal re-proven; Debug EXIT 0 + Release EXIT 0; full trajectory 23/23 '
              'in both).')
ac22_mil = parent_mil.replace(old_anchor, new_anchor, 1)
assert old_anchor not in ac22_mil, 'anchor fully replaced'
assert 'Step 12' not in ac22_mil, 'AC-22-only milestone must NOT contain Step 12'
assert 'Step 11' in ac22_mil

# ---------------------------------------------------------------------------
# 2) AC-22 row flip in the REPORT: NOT PROVEN -> PROVEN twin
old_row = '| AC-22: Deterministic second run | No double-run | NOT PROVEN |'
assert parent_rep.count(old_row) == 1, 'report AC-22 row must be unique; found %d' % parent_rep.count(old_row)
new_row = ('| AC-22: Deterministic second run | Twin double-run via `Engine::RunPipeline` '
           '(two fresh projects, real GTSAM seams), Debug EXIT 0 + Release EXIT 0 | PROVEN |')
ac22_rep = parent_rep.replace(old_row, new_row, 1)
assert old_row not in ac22_rep
assert '| PROVEN |' in ac22_rep and 'Twin double-run' in ac22_rep

# ---------------------------------------------------------------------------
msg('')
msg('=== AC-22-only milestone ===')
msg('  bytes=%d lines=%d  Step-12 absent=%s  AC-22 PROVEN present=%s'
    % (len(ac22_mil.encode('utf-8')), ac22_mil.count('\n') + 1,
       'Step 12' not in ac22_mil, 'AC-22 (PROVEN 2026-09-12' in ac22_mil))
msg('=== AC-22-only report ===')
msg('  bytes=%d lines=%d  PROVEN row present=%s'
    % (len(ac22_rep.encode('utf-8')), ac22_rep.count('\n') + 1,
       '| AC-22: Deterministic second run | Twin double-run' in ac22_rep))

wb(r'tmp\ac22_milestone_only.md', ac22_mil)
wb(r'tmp\ac22_report_only.md', ac22_rep)
msg('')
msg('WROTE tmp/ac22_milestone_only.md + tmp/ac22_report_only.md')

tmp = r'tmp\ac22_split_docs_facts.txt'
open(tmp, 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE %s bytes=%d' % (tmp, len(out.getvalue().encode('utf-8'))))
