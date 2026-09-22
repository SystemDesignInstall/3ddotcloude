# -*- coding: utf-8 -*-
"""AC-22-ONLY docs generator (commit-A content). Mutates NOTHING in worktree;
only reads git blobs and writes tmp/ac22_*_ac22only.{md,cpp} + tmp/ac22_gen_facts.txt.
Self-asserting; every assert is byte-truth on git blobs.
"""
import subprocess, io, os, hashlib

REPO    = r'C:\Code\ClaudeDot\spatial-platform'
MIL     = 'docs/architecture/P3-milestone-status.md'
REP     = 'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST     = 'tests/unit/test_p3_trajectory_optimization.cpp'
PARENT  = 'fbef42b'
HEADOBJ = '72b88d2'   # authoritative HEAD blob: the combined(AC22+Step12) state

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    assert r.returncode == 0, 'git %s failed: %s' % (a, r.stderr.decode('utf-8', 'replace'))
    return r.stdout

def blob(rev, p):
    return git('show', rev + ':' + p).decode('utf-8', 'replace')

def md5b(b):
    return hashlib.md5(b).hexdigest()

def writef(p, data):
    with open(os.path.join(REPO, 'tmp', p), 'w', encoding='utf-8', newline='\n') as f:
        f.write(data)

out = io.StringIO()
def p(*a):
    print(*a, file=out)

# ---------------------------------------------------------------------------
p('===== 0) shape pins =====')
M_par = blob(PARENT, MIL); M_hea = blob(HEADOBJ, MIL)
R_par = blob(PARENT, REP); R_hea = blob(HEADOBJ, REP)
T_par = blob(PARENT, TST); T_hea = blob(HEADOBJ, TST)
p('parent milestone: bytes=%d lines=%d' % (len(M_par.encode('utf-8')), M_par.count('\n') + 1))
p('HEAD   milestone: bytes=%d lines=%d ; "Step 12" count=%d ; AC-22-PROVEN-present=%s'
      % (len(M_hea.encode('utf-8')), M_hea.count('\n') + 1, M_hea.count('Step 12'),
         'AC22_DeterministicSecondRunThroughProductionSurface' in M_hea))
p('parent report  : bytes=%d lines=%d' % (len(R_par.encode('utf-8')), R_par.count('\n') + 1))
p('HEAD   report  : bytes=%d lines=%d' % (len(R_hea.encode('utf-8')), R_hea.count('\n') + 1))
p('parent test    : bytes=%d lines=%d' % (len(T_par.encode('utf-8')), T_par.count('\n') + 1))
p('HEAD   test    : bytes=%d lines=%d' % (len(T_hea.encode('utf-8')), T_hea.count('\n') + 1))

# ===========================================================================
p('')
p('===== 1) MILESTONE AC-22-ONLY = parent + SINGLE Step-11 tail flip (pure; no Step-12) =====')
old_tail = 'Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).'
assert M_par.count(old_tail) == 1, 'parent must contain this tail exactly once (got %d)' % M_par.count(old_tail)
new_tail = ('Remaining P3.1 acceptance items: CLI real providers, AC-22 '
            '(PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh '
            '`Engine::RunPipeline("p3_sparse_correction", \u2026)` invocations through REAL GTSAM seams, '
            'semantic twin equivalence asserted, N5 duplicate-refusal re-proven; !detached Debug EXIT 0 '
            '+ !detached Release EXIT 0, full trajectory 23/23 in both).')
m_ac22 = M_par.replace(old_tail, new_tail, 1)
assert 'Step 12' not in m_ac22, 'AC-22-only milestone must NOT contain "Step 12" (CLI deferred)'
assert m_ac22.count('Step 12') == 0 and m_ac22.count('NOT started per GO') == 0
# keep the two-projects/CLI mention in Step-11 body intact, but AC-22 PROVEN as its own flip:
assert 'CLI real providers' in m_ac22
p('  AC-22-only milestone: bytes=%d lines=%d ; Step-12 absent=%s ; PROVEN twin present=%s'
      % (len(m_ac22.encode('utf-8')), m_ac22.count('\n') + 1,
         'Step 12' not in m_ac22, 'DeterministicSecondRunThroughProductionSurface' in m_ac22))
writef('ac22_milestone_ac22only.md', m_ac22)

# ===========================================================================
p('')
p('===== 2) REPORT AC-22-ONLY = verify parent..HEAD report diff == EXACTLY one AC-22 row; use HEAD report =====')
rdiff = git('diff', PARENT, HEADOBJ, '--', REP).decode('utf-8', 'replace')
r_adds = [l for l in rdiff.split('\n') if l.startswith('+') and not l.startswith('+++')]
r_rems = [l for l in rdiff.split('\n') if l.startswith('-') and not l.startswith('---')]
p('  report parent..HEAD: +%d -%d' % (len(r_adds), len(r_rems)))
assert len(r_adds) == 1 and len(r_rems) == 1, 'report must differ by exactly one AC-22 row (got +%d -%d)' % (len(r_adds), len(r_rems))
p('  - parent row: %s' % r_rems[0][1:160])
p('  + HEAD  row: %s' % r_adds[0][1:160])
assert 'AC-22' in r_adds[0] and 'PROVEN' in r_adds[0] and 'NOT started' not in r_adds[0]
r_ac22 = R_hea   # HEAD report == AC-22-only report (single AC-22 row flip, no CLI/Step-12 refs)
assert 'Step 12' not in r_ac22 
writef('ac22_report_ac22only.md', r_ac22)

# ===========================================================================
p('')
p('===== 3) TEST AC-22-ONLY = parent + pure AC-22 twin add (assert: no CLI/Step-12 refs) =====')
tdiff = git('diff', PARENT, HEADOBJ, '--', TST).decode('utf-8', 'replace')
t_adds = [l for l in tdiff.split('\n') if l.startswith('+') and not l.startswith('+++')]
t_rems = [l for l in tdiff.split('\n') if l.startswith('-') and not l.startswith('---')]
p('  test parent..HEAD: +%d -%d' % (len(t_adds), len(t_rems)))
assert len(t_rems) == 0, 'twin test must be pure add (got %d removals)' % len(t_rems)
# no CLI / Step-12 / E2E references in the AC-22 twin add:
cli_refs = [l for l in t_adds if ('cli' in l.lower()) or ('Step 12' in l) or ('E2E' in l)]
assert not cli_refs, 'AC-22 twin add must not reference CLI/Step-12/E2E: %r' % cli_refs[:3]
p('  twin add lines=%d ; CLI/Step-12 refs=%d' % (len(t_adds), len(cli_refs)))
t_ac22 = T_hea   # HEAD test == parent + pure twin add
writef('ac22_test_ac22only.cpp', t_ac22)

# ===========================================================================
p('')
p('===== 4) md5 pins (commit-A aim) =====')
p('  milestone AC-22-only md5=%s' % md5b(m_ac22.encode('utf-8')))
p('  report    AC-22-only md5=%s' % md5b(r_ac22.encode('utf-8')))
p('  test      AC-22-only md5=%s' % md5b(t_ac22.encode('utf-8')))

open(os.path.join(REPO, 'tmp', 'ac22_gen_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_gen_facts.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
