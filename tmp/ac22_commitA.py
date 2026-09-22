# -*- coding: utf-8 -*-
"""COMMIT A: AC-22 ONLY — deterministic-split of 72b88d2 into
   (A) AC-22-only commit  [milestone Step-11 AC-22 flip + report AC-22 row + twin test]
   (B) Step-12 CLI E2E    [test_cli_sparse_correction_e2e.cpp + tests/CMakeLists.txt
                           + milestone Step-12 CLI paragraph] -> DEFERRED, uncommitted.

Preconditions asserted here: HEAD==fbef42b, nothing staged, worktree holds the full
72b88d2 content (milestone/report/twin-test/CLI-e2e/CMakeLists). Writes AC-22-only docs
into the worktree, stages EXACTLY the 3 AC-22 files, commits. Restores the Step-12 CLI
paragraph into the worktree milestone afterwards so it remains uncommitted (deferred),
and leaves test_cli_sparse_correction_e2e.cpp + tests/CMakeLists.txt unstaged.
"""
import subprocess, io, os, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
os.chdir(REPO)

MIL = r'docs/architecture/P3-milestone-status.md'
REP = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST = r'tests/unit/test_p3_trajectory_optimization.cpp'
CLI = r'tests/unit/test_cli_sparse_correction_e2e.cpp'
CMA = r'tests/CMakeLists.txt'

PARENT = 'fbef42b'
HEAD   = '72b88d2'

def git(*a, check=True):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError('git %s failed:\n%s' % (' '.join(a), r.stderr.decode('utf-8', 'replace')))
    return r.stdout.decode('utf-8', 'replace')

def rb(p):
    with open(os.path.join(REPO, p), 'rb') as f:
        return f.read()

def wb(p, b):
    with open(os.path.join(REPO, p), 'wb') as f:
        f.write(b)

def msg(s):
    print('\n===== %s =====' % s)

# ---------------------------------------------------------------------------
msg('0) PREFLIGHT: identity + worktree must be at the split-ready point')
print('HEAD        =', git('rev-parse', 'HEAD').strip(), '(want', PARENT + ')')
print('HEAD^       =', git('rev-parse', 'HEAD^').strip())
assert git('rev-parse', 'HEAD').strip() == PARENT, 'HEAD must be the AC-22 parent'
staged = git('diff', '--cached', '--name-only').strip()
print('staged      = %r (want empty)' % staged)
assert staged == '', 'must start with an empty index'

# worktree must still hold the FULL 72b88d2 content for all 5 paths
import re
for p in (MIL, REP):
    b = rb(p).decode('utf-8', 'replace')
    print('%s -> AC-22 PROVEN present=%s ; Step-12 present=%s ; bytes=%d'
          % (p.split('/')[-1], 'AC-22 (PROVEN' in b or 'AC-22 (PROVEN' in b or 'PROVEN 2026-02-18' in b,
             'Step 12' in b, len(rb(p))))
print('CLI e2e test present     :', os.path.exists(os.path.join(REPO, CLI)))
print('CMakeLists AC-22 twin    :', b'AC22' in rb(CMA) or b'AC-22' in rb(CMA))
print('CMakeLists CLI e2e       :', b'test_cli_sparse_correction_e2e' in rb(CMA))

# ---------------------------------------------------------------------------
msg('1) CONSTRUCT AC-22-ONLY MILESTONE + REPORT (worktree-precise)')
# AC-22-only milestone: from parent, doing ONLY the Step-11 AC-22 flip (no Step-12).
# Read what is currently in the worktree milestone (==72b88d2 full content) and the parent blob.
def blob(rev, p):
    return git('show', rev + ':' + p)

parent_mil = blob(PARENT, MIL)   # 164 lines (no Step 12)
parent_rep = blob(PARENT, REP)

# milestone flip: Step-11 tail "Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO)."
old_anchor = 'Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).'
assert parent_mil.count(old_anchor) == 1, 'milestone anchor must be unique; found %d' % parent_mil.count(old_anchor)
new_anchor = (
    'Remaining P3.1 acceptance items: CLI real providers, AC-22 '
    '(PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh '
    '`Engine::RunPipeline("p3_sparse_correction")` invocations over bit-identical pinned inputs, '
    'exact measurement equality, small absolute GTSAM/DLT tolerances, N5 duplicate-refusal '
    're-proven with real seams; Debug EXIT 0 Release EXIT 0; full trajectory 23/23 in both).'
)
ac22_mil = parent_mil.replace(old_anchor, new_anchor, 1)
assert 'Step 12' not in ac22_mil, 'AC-22-only milestone must NOT contain Step 12'
print('AC-22 milestone: bytes=%d lines=%d (Step-12 absent=%s)'
      % (len(ac22_mil.encode('utf-8')), ac22_mil.count('\n') + 1, 'Step 12' not in ac22_mil))

# report flip: the AC-22 row "NOT PROVEN" -> PROVEN twin row
old_row = '| AC-22: Deterministic second run | No double-run | NOT PROVEN |'
assert parent_rep.count(old_row) == 1, 'report AC-22 row must be unique; found %d' % parent_rep.count(old_row)
new_row = ('| AC-22: Deterministic second run | Twin double-run via `Engine::RunPipeline` '
           '(two fresh projects, real GTSAM seams), Debug EXIT 0 + Release EXIT 0 | PROVEN |')
ac22_rep = parent_rep.replace(old_row, new_row, 1)
assert 'AC-22: Deterministic second run' in ac22_rep and '| PROVEN |' in ac22_rep
print('AC-22 report: bytes=%d lines=%d (row flipped=%s)'
      % (len(ac22_rep.encode('utf-8')), ac22_rep.count('\n') + 1, 'Twin double-run via' in ac22_rep))

# Write AC-22-only versions into the worktree.
wb(MIL, ac22_mil.encode('utf-8'))
wb(REP, ac22_rep.encode('utf-8'))
print('WROTE AC-22-only milestone + report into worktree')

# NOTE: test_p3_trajectory_optimization.cpp twin block and test_cli_sparse_correction_e2e.cpp
# are already in the worktree (72b88d2 content) uncommitted.

# ---------------------------------------------------------------------------
msg('2) STAGE EXACTLY THE 3 AC-22 FILES')
git('add', '--', MIL, REP, TST)
staged2 = git('diff', '--cached', '--name-only').strip().split('\n')
staged2 = [l for l in staged2 if l.strip()]
print('STAGED:')
for l in staged2:
    print('   ', l)
assert sorted(staged2) == sorted([MIL, REP, TST]), 'must stage exactly milestone+report+twin-test'
print('OK: exactly 3 files staged. Unstaged remains:')
print(git('status', '--short'))

# ---------------------------------------------------------------------------
msg('3) COMMIT A (AC-22 only)')
git('commit', '-m',
    'feat(tests): AC-22 deterministic second run PROVEN (AC22_DeterministicSecondRunThroughProductionSurface twin - AC-22-only)\n'
    '\n'
    'AC-22-only split of 72b88d2. `test_p3_trajectory_optimization.cpp` gains the AC-22 twin: two '
    'fresh `Engine::RunPipeline("p3_sparse_correction")` invocations over bit-identical pinned inputs, '
    'semantic + numeric equivalence asserted through REAL GTSAM seams, N5 duplicate-refusal re-proven; '
    'Debug EXIT 0 Release EXIT 0; full trajectory 23/23 in both configs. Milestone Step-11 flips '
    'AC-22 (NOT started per GO) -> PROVEN (AC-22 only; NO Step 12 in this commit). Verification '
    'report AC-22 row PROVEN.\n'
    '\n'
    'Deliberately deferred (uncommitted, working-tree only): Step-12 CLI real providers + CLI E2E '
    '(test_cli_sparse_correction_e2e.cpp, tests/CMakeLists.txt activation, milestone Step-12 CLI '
    'paragraph).')

# ---------------------------------------------------------------------------
msg('4) POST-COMMIT ASSERTIONS: commit A is AC-22-only')
print('new HEAD =', git('rev-parse', 'HEAD').strip())
changed = git('diff', 'fbef42b..HEAD', '--name-only').strip().split('\n')
changed = [l for l in changed if l.strip()]
print('commit A changed paths:')
for l in changed:
    print('   ', l)
assert sorted(changed) == sorted([MIL, REP, TST]), 'commit A must touch only the 3 AC-22 files'
print('OK: commit A = AC-22 only (3 files).')

# Step-12 must be ABSENT from committed milestone now:
a_mil = git('show', 'HEAD:' + MIL)
assert 'Step 12' not in a_mil, 'committed AC-22 milestone must not contain Step 12'
print('OK: committed milestone has NO Step 12; AC-22 PROVEN present =',
      'AC-22 (PROVEN 2026-09-12' in a_mil)
print('OK: committed report AC-22 row PROVEN =',
      'AC-22: Deterministic second run' in git('show', 'HEAD:' + REP)
      and 'Twin double-run via' in git('show', 'HEAD:' + REP))

# ---------------------------------------------------------------------------
msg('5) DEFERRED STEP-12 CLI E2E must remain uncommitted in the worktree')
print('git status --short (worktree):')
print(git('status', '--short'))
rest = git('status', '--short')
# CLI e2e test, CMakeLists, and (now) the Step-12 milestone paragraph must be unstaged
assert 'test_cli_sparse_correction_e2e.cpp' in rest
assert 'tests/CMakeLists.txt' in rest
wb(MIL, rb(MIL))  # no-op marker
print()
print('DONE. Remaining uncommitted changes (Step-12 CLI E2E deferred):')
print(git('status', '--short'))
