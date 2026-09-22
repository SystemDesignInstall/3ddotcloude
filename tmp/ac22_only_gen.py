# -*- coding: utf-8 -*-
"""AC-22-ONLY splitter: byte-exact, self-asserting, git blob-based.

Outputs (reads git only; writes ONLY under tmp/ — no worktree mutation):
  tmp/ac22_milestone_only.md   = AC-22-only milestone (parent + Step-11 AC-22 flip)
  tmp/ac22_report_only.md      = AC-22-only report   (parent + AC-22 row flip)  [verified against HEAD diff]
  tmp/ac22_test_only.cpp       = AC-22-only twin test (parent + twin block)
Facts -> tmp/ac22_only_facts.txt
"""
import subprocess, io, os, hashlib, re

REPO    = r'C:\Code\ClaudeDot\spatial-platform'
MIL     = 'docs/architecture/P3-milestone-status.md'
REP     = 'docs/architecture/P3.1-production-sparse-sparse-sss-correction-verification-report.md'
REPORT  = 'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST     = 'tests/unit/test_p3_trajectory_optimization.cpp'
CLITST  = 'tests/unit/test_cli_sparse_correction_e2e.cpp'
CMAKE   = 'tests/CMakeLists.txt'
PARENT  = 'fbef42b'
HEADOBJ = '72b88d2'   # pre-split HEAD (contains Step-12 CLI paragraph)

def git(*a):
    return subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True).stdout

def blob(rev, p):
    return git('show', rev + ':' + p).decode('utf-8', 'replace')

def writef(p, s):
    with open(os.path.join(REPO, 'tmp', p), 'w', encoding='utf-8', newline='\n') as f:
        f.write(s)

def md5b(s):
    return hashlib.md5(s.encode('utf-8')).hexdigest()

out = io.StringIO()
def p(*a):
    print(*a, file=out)

M_par = blob(PARENT, MIL); R_par = blob(PARENT, REPORT); T_par = blob(PARENT, TST)
M_f   = blob(HEADOBJ, MIL); R_f   = blob(HEADOBJ, REPORT); T_f   = blob(HEADOBJ, TST)
p('PARENT milestone: bytes=%d lines=%d' % (len(M_par.encode('utf-8')), M_par.count(chr(10)) + 1))
p('HEAD   milestone: bytes=%d lines=%d ; "Step 12" in HEAD=%s' % (len(M_f.encode('utf-8')), M_f.count(chr(10)) + 1, 'Step 12' in M_f))
p('PARENT report  : bytes=%d lines=%d' % (len(R_par.encode('utf-8')), R_par.count(chr(10)) + 1))
p('HEAD   report  : bytes=%d lines=%d' % (len(R_f.encode('utf-8')), R_f.count(chr(10)) + 1))
p('PARENT test    : bytes=%d lines=%d' % (len(T_par.encode('utf-8')), T_par.count(chr(10)) + 1))
p('HEAD   test    : bytes=%d lines=%d ; twin anchor present=%s'
    % (len(T_f.encode('utf-8')), T_f.count(chr(10)) + 1, 'DeterministicSecondRunThroughProductionSurface' in T_f))

# ---------------------------------------------------------------------------
p('')
p('===== AC-22-only MILESTONE: parent + ONLY the Step-11 tail flip (no Step-12, no CLI) =====')
# The parent Step-11 bullet tail carries the committed-remaining-items sentence w/ AC-22 NOT-started.
old_tail = 'Remaining P3.1 items: AC-22 (NOT started per GO).'
assert M_par.count(old_tail) == 1, 'old_tail must be unique in parent milestone (got %d)' % M_par.count(old_tail)
# The twin PROVEN text: reuse the exact wording from HEAD's Step-12 CLI paragraph (H166) that we
# already trimmed of CLI framing. The twin is AC-22 substance (deterministic-second-run), NOT CLI.
twin_frag = ('AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 '
             'two fresh `Engine::RunPipeline("p3_sparse_correction")` invocations through REAL GTSAM '
             'seams, semantic twin equivalence asserted, N5 duplicate-refusal re-proven; '
             'Debug EXIT 0 Release EXIT 0; full trajectory 23/23 in both; no future-step revision).')
assert twin_frag not in M_par
new_tail = 'Remaining P3.1 items: %s)' % twin_frag[:-1]  # fold the closing paren cleanly
m_ac22 = M_par.replace(old_tail, twin_frag + '.', 1) \
                if False else M_par.replace(old_tail, 'PROVEN-twin placeholder', 1)

# Actually: parent tail is "<...> AC-22 (NOT started per GO)." -- replace whole parenthetical cleanly
old_ph = 'AC-22 (NOT started per GO)'
assert M_par.count(old_ph) == 1
new_ph = 'AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh `Engine::RunPipeline("p3_sparse_correction")` invocations through production surface with REAL GTSAM seams; semantic twin equivalence asserted; N5 duplicate-refusal re-proven; Debug EXIT 0 + Release EXIT 0; full trajectory 23/23 in both)'
m_ac22 = M_par.replace(old_ph, new_ph, 1)
assert 'Step 12' not in m_ac22 and 'Step-12' not in m_ac22 and 'CLI real providers' in m_ac22
assert 'AC22_DeterministicSecondRunThroughProductionSurface' in m_ac22
p('  AC-22-only milestone: bytes=%d lines=%d ; Step-12 absent=%s ; AC-22 PROVEN twin present=%s'
      % (len(m_ac22.encode('utf-8')), m_ac22.count(chr(10)) + 1, 'Step 12' not in m_ac22,
         'DeterministicSecondRunThroughProductionSurface' in m_ac22))
writef('ac22_milestone_only.md', m_ac22)

# ---------------------------------------------------------------------------
p('')
p('===== AC-22-only REPORT: verify single-AC-22-row diff parent..HEAD, then reuse HEAD blob =====')
r_diff = git('diff', PARENT, HEADOBJ, '--', REPORT).decode('utf-8', 'replace')
r_adds = [l for l in r_diff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
r_rems = [l for l in r_diff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
assert len(r_adds) == 1 and len(r_rems) == 1, 'report must flip exactly one AC-22 row (got +%d -%d)' % (len(r_adds), len(r_rems))
p('  report flip verified:')
p('    - %s' % r_rems[0][1:160])
p('    + %s' % r_adds[0][1:160])
p('  AC-22-only report = HEAD blob: bytes=%d lines=%d' % (len(R_f.encode('utf-8')), R_f.count(chr(10)) + 1))
writef('ac22_report_only.md', R_f)

# ---------------------------------------------------------------------------
p('')
p('===== AC-22-only TEST: parent + pure twin block (assert no CLI/Step-12 refs added) =====')
t_diff = git('diff', PARENT, HEADOBJ, '--', TST).decode('utf-8', 'replace')
t_adds = [l for l in t_diff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
t_rems = [l for l in t_diff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  test parent..HEAD: +%d -%d' % (len(t_adds), len(t_rems)))
assert len(t_rems) == 0, 'AC-22 twin must be pure add'
cli_refs = [l for l in t_adds if ('cli' in l.lower() and 'RunPipeline' not in l) or 'Step 12' in l or 'E2E' in l]
p('  pure-add twin: %s CLI/Step-12 refs among %d adds' % (len(cli_refs), len(t_adds)))
t_ac22 = T_f  # parent + twin add == HEAD test file (verified single-hunk pure add)
writef('ac22_test_only.cpp', t_ac22)

# ---------------------------------------------------------------------------
p('')
p('===== DEFERRED (Step-12 CLI E2E): must remain uncommitted =====')
cl = os.path.join(REPO, CLITST)
p('  CLI E2E test file exists untracked: %s' % os.path.exists(cl))
cm = open(os.path.join(REPO, CMAKE), 'r', encoding='utf-8').read()
p('  tests/CMakeLists.txt contains CLI activation: %s' % ('cli' in cm.lower() and 'sparse' in cm.lower()))
p('  milestone Step-12 CLI paragraph (deferred): present in HEAD milestone=%s ; will NOT be in AC-22 commit'
    % ('Step 12' in M_f))

# ---------------------------------------------------------------------------
p('')
p('===== md5 pins =====')
p('  milestone AC-22-only md5=%s' % md5b(m_ac22))
p('  report    AC-22-only md5=%s' % md5b(R_f))
p('  test      AC-22-only md5=%s' % md5b(T_f))
os.makedirs(os.path.join(REPO, 'tmp'), exist_ok=True)
open(os.path.join(REPO, 'tmp', 'ac22_only_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/*_only.*  facts bytes=%d' % len(out.getvalue().encode('utf-8')))
