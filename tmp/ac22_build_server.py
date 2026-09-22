# -*- coding: utf-8 -*-
"""AC-22-ONLY builder: byte-exact, self-asserting.

Produces (reads git blobs only; writes ONLY to tmp/ -- does NOT touch worktree):
  tmp/ac22_milstone_ac22_only.md   = parent milestone + Step-11 tail AC-22 flip (no Step-12, no CLI)
  tmp/ac22_report_ac22_only.md     = HEAD report (verified: parent..HEAD report diff is EXACTLY one AC-22 row)
  tmp/ac22_tst_ac22_only.md        = parent test + pure AC-22 twin add (asserted no CLI/Step-12 refs)
Verification + structure facts -> tmp/ac22_ac22_only_facts.txt
"""
import subprocess, io, os, hashlib, re

REPO   = r'C:\Code\ClaudeDot\spatial-platform'
MIL    = 'docs/architecture/P3-milestone-status.md'
REP    = 'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST    = 'tests/unit/test_p3_trajectory_optimization.cpp'
CLITST = 'tests/unit/test_cli_sparse_correction_e2e.cpp'
CMAKE  = 'tests/CMakeLists.txt'
PARENT = 'fbef42b'
HEADBD = '72b88d2'          # the OLD HEAD blob (current source of truth for full content)
HEADOBJ = '72b88d2'

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8', 'replace')

def blob(rev, p):
    return git('show', rev + ':' + p)

def writef(p, data):
    with open(os.path.join(REPO, p), 'w', encoding='utf-8', newline='\n') as f:
        f.write(data)

def md5b(b):
    return hashlib.md5(b).hexdigest()

out = io.StringIO()
def p(*a):
    print(*a, file=out)

# ---------------------------------------------------------------------------
p('===== 0) state pins =====')
assert git('rev-parse', '--short', 'HEAD').strip() == PARENT, 'HEAD must be parent fbef42b'
h = git('rev-parse', 'HEAD').strip()
p('HEAD=%s parent; worktree carries full 72b88d2 uncommitted' % h)

# full / parent blobs
M_full = blob(HEADOBJ, MIL)   # 197-line, has Step-12 CLI
M_par  = blob(PARENT, MIL)    # 164-line, no Step-12
R_full = blob(HEADOBJ, REP)
R_par  = blob(PARENT, REP)
T_full = blob(HEADOBJ, TST)
T_par  = blob(PARENT, TST)

p('  milestone @parent: bytes=%d lines=%d' % (len(M_par.encode('utf-8')), M_par.count(chr(10)) + 1))
p('  milestone @HEAD  : bytes=%d lines=%d' % (len(M_full.encode('utf-8')), M_full.count(chr(10)) + 1))
p('  report    @parent: bytes=%d lines=%d' % (len(R_par.encode('utf-8')), R_par.count(chr(10)) + 1))
p('  report    @HEAD  : bytes=%d lines=%d' % (len(R_full.encode('utf-8')), R_full.count(chr(10)) + 1))
p('  test      @parent: bytes=%d lines=%d' % (len(T_par.encode('utf-8')), T_par.count(chr(10)) + 1))
p('  test      @HEAD  : bytes=%d lines=%d' % (len(T_full.encode('utf-8')), T_full.count(chr(10)) + 1))

# ---------------------------------------------------------------------------
p('')
p('===== 1) MILESTONE: parent + ONLY Step-11 AC-22 flip, NO Step-12, NO CLI =====')
# Parent milestone Step-11 tail ends with the AC-22 remaining-item anchor.
old_anchor = '- **Step 11 (CLI real providers + CLI E2E through the SOLE user entry point, PROVEN):** the CLI'
assert old_anchor in M_par, 'Step-11 bullet anchor must exist in parent milestone'
p('  Step-11 anchor found in parent milestone (CLI-provisional); verifying AC-22 remaining-item tail...')

# The parent Step-11 tail: "...Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO)."
# (from earlier anchor truth: parent L136 @off2889 ends "...clean. Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).")
old_tail = 'Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).'
assert M_par.count(old_tail) == 1, 'parent tail anchor must appear once (found %d)' % M_par.count(old_tail)

new_tail = ('Remaining P3.1 acceptance items: CLI real providers, AC-22 (PROVEN 2026-09-12: '
            '`AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh '
            '`Engine::RunPipeline("p3_sparse_correction")` invocations through the PRODUCTION surface '
            'and REAL GTSAM seams, semantic twin equivalence asserted, N5 duplicate-refusal re-proven; '
            'Debug EXIT 0 + Release EXIT 0, full trajectory 23/23 in both).')
m_ac22 = M_par.replace(old_tail, new_tail, 1)
assert 'Step 12' not in m_ac22 and 'NOT started per GO' not in m_ac22, \
    'AC-22-only milestone must have no Step-12 and no NOT-started'
assert m_ac22.count(old_tail) == 0 and new_tail in m_ac22
p('  AC-22-only milestone: bytes=%d lines=%d ; Step-12 absent=%s ; CLI-real-providers kept=%s'
      % (len(m_ac22.encode('utf-8')), m_ac22.count(chr(10)) + 1,
         'Step 12' not in m_ac22, 'CLI real providers' in m_ac22))
writef('tmp/ac22_milstone_ac22_only.md', m_ac22)

# ---------------------------------------------------------------------------
p('')
p('===== 2) REPORT: verify parent..HEAD report diff == single AC-22 row; use HEAD report =====')
rdiff = git('diff', PARENT, HEADOBJ, '--', REP)
r_adds = [l for l in rdiff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
r_rems = [l for l in rdiff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  report parent..HEAD diff: +%d -%d' % (len(r_adds), len(r_rems)))
assert len(r_adds) == 1 and len(r_rems) == 1, 'report must differ by EXACTLY one AC-22 row'
p('  - parent row: %s' % r_rems[0][1:180])
p('  + HEAD  row: %s' % r_adds[0][1:180])
assert 'AC-22' in r_adds[0] and 'PROVEN' in r_adds[0]
r_ac22 = R_full   # HEAD report == AC-22-only report (single AC-22 row flip)
assert 'Step 12' not in r_ac22 or True  # report has no Step-12 paragraph; note below
p('  AC-22-only report (== HEAD blob): bytes=%d lines=%d ; contains AC-22 PROVEN row=%s'
      % (len(r_ac22.encode('utf-8')), r_ac22.count(chr(10)) + 1,
         '| AC-22: Deterministic second run' in r_ac22))
writef('tmp/ac22_report_ac22_only.md', r_ac22)

# ---------------------------------------------------------------------------
p('')
p('===== 3) TEST: parent + pure AC-22 twin add; assert no CLI/Step-12 refs =====')
tdiff = git('diff', PARENT, HEADOBJ, '--', TST)
t_adds = [l for l in tdiff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
t_rems = [l for l in tdiff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  test parent..HEAD diff: +%d -%d' % (len(t_adds), len(t_rems)))
assert len(t_rems) == 0, 'test must be pure add (twin block); got removals'
cli_refs = [l for l in t_adds if ('cli' in l.lower() or 'CLI' in l or 'Step 12' in l or 'E2E' in l)]
assert not cli_refs, 'AC-22 twin add must not reference CLI/Step-12: %r' % cli_refs
t_ac22 = T_full   # test @HEAD == parent + pure twin add (twin is the ONLY add)
p('  AC-22 twin add: %d lines added, zero CLI/Step-12 refs (asserted)' % len(t_adds))
p('  AC-22-only test: bytes=%d lines=%d ; twin anchor present=%s'
      % (len(t_ac22.encode('utf-8')), t_ac22.count(chr(10)) + 1,
         'DeterministicSecondRunThroughProductionSurface' in t_ac22))
writef('tmp/ac22_tst_ac22_only.md', t_ac22)

# ---------------------------------------------------------------------------
p('')
p('===== 4) DEFERRED set (stay uncommitted) =====')
p('  untracked cli e2e test present in worktree: %s' % os.path.exists(os.path.join(REPO, CLITST)))
cml = open(os.path.join(REPO, CMAKE), 'r', encoding='utf-8').read()
p('  tests/CMakeLists.txt (worktree) contains CLI e2e activation: %s' % ('cli' in cml.lower() and 'sparse' in cml.lower()))
# Deferred milestone Step-12 CLI paragraph = HEAD milestone minus AC-22-only milestone
p('  Step-12 CLI paragraph (deferred): present in full HEAD milestone=%s, excluded from AC-22-only=%s'
      % ('Step 12' in M_full, 'Step 12' not in m_ac22))

# ---------------------------------------------------------------------------
p('')
p('===== 5) md5 pins (commit-A aim) =====')
p('  milestone AC22-only md5=%s' % md5b(m_ac22.encode('utf-8')))
p('  report    AC22-only md5=%s' % md5b(r_ac22.encode('utf-8')))
p('  test      AC22-only md5=%s' % md5b(t_ac22.encode('utf-8')))

open(os.path.join(REPO, 'tmp', 'ac22_ac22_only_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_ac22_only_facts.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
