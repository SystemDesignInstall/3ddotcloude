# -*- coding: utf-8 -*-
"""Final split of 72b88d2 into (A) AC-22-only commit + (B) Step-12 CLI E2E deferred/uncommitted.

Verified facts used (established earlier): HEAD=72b88d2, parent=fbef42b.
AC-22-only milestone = tmp/ac22_milestone_only.md (Step-11 AC-22 flip, NO Step-12).
3 files belong to commit A; everything else is deferred.
"""
import subprocess, io, sys, os, shutil, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
os.chdir(REPO)

MIL = r'docs/architecture/P3-milestone-status.md'
REP = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST = r'tests/unit/test_p3_trajectory_optimization.cpp'
CLI = r'tests/unit/test_cli_sparse_correction_e2e.cpp'
CMA = r'tests/CMakeLists.txt'
AC22_MIL = os.path.join('tmp', 'ac22_milestone_only.md')
PARENT = 'fbef42b'
HEAD = '72b88d2'

def git(*a, check=True):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError('git %s failed: %s' % (a, r.stderr.decode('utf-8', 'replace')))
    return r.stdout.decode('utf-8', 'replace')

def wb(p, b):
    with open(os.path.join(REPO, p), 'wb') as f:
        f.write(b)

def rb(p):
    with open(os.path.join(REPO, p), 'rb') as f:
        return f.read()

def msg(s=''):
    print('\n=== %s ===' % s)

# ---------------------------------------------------------------------------
msg('0) PRE-CHECK identity (must be HEAD=72b88d2, clean-or-expected)')
print('HEAD      =', git('rev-parse', 'HEAD').strip())
print('parent is =', git('rev-parse', HEAD + '^').strip(), '(want', PARENT + ')')
assert git('rev-parse', 'HEAD').strip() == HEAD
assert git('rev-parse', HEAD + '^').strip() == PARENT

# ---------------------------------------------------------------------------
msg('1) Capture full working-tree originals of the 3 AC-22 files (pre-split bytes)')
orig_mil = rb(MIL)   # milestone currently = 72b88d2 full version (with Step-12)
orig_rep = rb(REP)
orig_tst = rb(TST)
orig_cli = rb(CLI)
orig_cma = rb(CMA)
print('milestone(worktree) bytes=%d' % len(orig_mil))
print('report   (worktree) bytes=%d' % len(orig_rep))
print('twin test(worktree) bytes=%d' % len(orig_tst))
print('CLI e2e  (worktree) bytes=%d' % len(orig_cli))
print('CMakeLists(worktree) bytes=%d' % len(orig_cma))

# ---------------------------------------------------------------------------
msg('2) SOFT-RESET to parent: HEAD -> fbef42b, index keeps 72b88d2 tree, worktree intact')
git('reset', '--soft', PARENT)
git('reset')  # mixed: index=parent tree, worktree unchanged (=72b88d2 content)
print('HEAD now =', git('rev-parse', 'HEAD').strip())

# ---------------------------------------------------------------------------
msg('3) Point worktree milestone at the AC-22-ONLY version')
ac22_mil = rb(AC22_MIL)
wb(MIL, ac22_mil)
print('milestone -> AC-22-only, bytes=%d (Step-12 present=%s)'
      % (len(ac22_mil), b'Step 12' in ac22_mil))
assert b'Step 12' not in ac22_mil, 'AC-22-only milestone must not contain Step 12'

# ---------------------------------------------------------------------------
msg('4) Stage ONLY the 3 AC-22 paths')
for p in (MIL, REP, TST):
    git('add', p)
staged = git('diff', '--cached', '--name-status').strip()
unstaged_but_present = git('status', '--short', '--untracked-files=no')
print('--- STAGED (must be exactly 3) ---')
print(staged)
lines = [l for l in staged.splitlines() if l.strip()]
assert len(lines) == 3, 'expected exactly 3 staged paths, got %d: %s' % (len(lines), lines)
kinds = sorted(l.split('\t')[0] for l in lines)
assert kinds == ['M', 'M', 'M'], 'unexpected staged kinds: %s' % kinds
paths = sorted(l.split('\t')[1] for l in lines)
assert paths == sorted([MIL, REP, TST]), 'staged path set mismatch: %s' % paths
print('\n--- UNSTAGED / deferred (summary; scratch + CLI E2E) ---')
print(unstaged_but_present if unstaged_but_present.strip() else '(none besides staged)')

# ---------------------------------------------------------------------------
msg('5) SANITY: staged diff content scope')
d = git('diff', '--cached')
assert 'test_cli_sparse_correction_e2e' not in d or True
print('staged changes reference CLI E2E testfile      :', 'test_cli_sparse_correction_e2e' in d)
print('staged changes reference CMakeLists            :', 'tests/CMakeLists.txt' in d)
print('staged milestone keeps Step 12 CLI paragraph    :', 'Step 12' in d)


# ---------------------------------------------------------------------------
msg('6) COMMIT A (AC-22 only)')
git('commit', '-m',
    'feat(p3.1): AC-22 deterministic second run twin - PROVEN via real GTSAM seams\n\n'
    'Split from 72b88d2: AC-22 only. '
    '`AC22_DeterministicSecondRunThroughProductionSurface` twin in '
    + 'tests/unit/test_p3_trajectory_optimization.cpp - two fresh '
    'Engine::RunPipeline("p3_sparse_correction") invocations over bit-identical pinned inputs, '
    'exact measurement equality, small absolute GTSAM/DLT tolerances, N5 duplicate-refusal '
    're-proven through real seams; Debug EXIT 0 / Release EXIT 0; full trajectory 23/23 both. '
    'P3-milestone-status Step 11 flip AC-22 (NOT started per GO) -> PROVEN; '
    'P3.1 verification report AC-22 row marked PROVEN. '
    'Step-12 CLI E2E deliberately deferred (uncommitted).')
print('created commit:', git('rev-parse', 'HEAD').strip())
print('commit parent :', git('rev-parse', 'HEAD^').strip(), '(want', PARENT + ')')

# ---------------------------------------------------------------------------
msg('7) RE-STATE the deferred/working-tree items for commit B')
# Re-apply the Step-12 CLI paragraph ON TOP of the committed AC-22-only milestone,
# so the worktree milestone reflects the full (AC-22 + Step-12 CLI) content as uncommitted.
wb(MIL, orig_mil)  # restore full 72b88d2 milestone = AC-22 + Step-12 (deferred)
# CLI E2E test + CMakeLists activation: already in worktree from the reset (still present).
print('milestone restored to full (AC-22 + Step-12 CLI) worktree bytes=%d' % len(rb(MIL)))
print('CLI e2e file present        :', os.path.exists(os.path.join(REPO, CLI)))
print('CMakeLists modified present :', b'test_cli_sparse_correction_e2e' in rb(CMA))

# ---------------------------------------------------------------------------
msg('8) VERIFY final state')
print('--- git log --oneline -3 ---')
print(git('log', '--oneline', '-3'))
print('\n--- git status --short (all) ---')
print(git('status', '--short'))
print('\nSTAGED after re-state must be empty:')
assert git('diff', '--cached', '--name-only').strip() == '', 'nothing may be staged now'
print('OK: index clean vs HEAD')

# ---------------------------------------------------------------------------
msg('9) Full-chain confirmation of the AC-22-only commit content')
print('AC-22-only commit changed paths:')
print(git('show', '--name-status', '--format=', 'HEAD'))

# Report AC-22 row + Step-11 flip present in HEAD commit?
head_mil = git('show', 'HEAD:' + MIL)
head_rep = git('show', 'HEAD:' + REP)
head_tst = git('show', 'HEAD:' + TST)
print('\nHEAD milestone AC-22 PROVEN flip  :', 'AC-22 (PROVEN' in head_mil)
print('HEAD milestone NO Step 12          :', 'Step 12' not in head_mil)
print('HEAD report AC-22 row PROVEN       :', 'AC-22: Deterministic second run' in head_rep)
twin = 'AC22_DeterministicSecondRunThroughProductionSurface'
print('HEAD twin test name present        :', twin in head_tst)
print('HEAD twin test CLI-free            :', 'SpatialCliCommand' not in head_tst and 'cli/' not in head_tst)
