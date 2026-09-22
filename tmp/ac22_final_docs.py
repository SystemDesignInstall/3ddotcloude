# -*- coding: utf-8 -*-
"""AC-22 split — AUTHORITATIVE FACT + DOC BUILD (tmp/ only; no git mutation).
Reads git blobs; writes facts + the 3 AC-22-only docs into tmp/. Nothing else.
"""
import subprocess, io, os, hashlib, re

REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL  = 'docs/architecture/P3-milestone-status.md'
REP  = 'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST  = 'tests/unit/test_p3_trajectory_optimization.cpp'
CLIT = 'tests/unit/test_cli_sparse_correction_e2e.cpp'
CL   = 'tests/CMakeLists.txt'
PARENT = 'fbef42b'
HEADOB = '72b88d2'

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    return r.stdout

def blob(rev, p):
    return git('show', rev + ':' + p).decode('utf-8', 'replace')

def b(p):
    return open(os.path.join(REPO, p), 'rb').read()

def wb(p, data):
    with open(os.path.join(REPO, p), 'wb') as f:
        f.write(data)

def md5b(x):
    return hashlib.md5(x).hexdigest()

def lines(s):
    return s.count(chr(10)) + 1

out = io.StringIO()
def p(*a):
    print(*a, file=out)

# -- 1) purify check: is tmp/ac22_milestone_only.md a pure AC-22-only milestone? --
fm = os.path.join(REPO, 'tmp', 'ac22_milestone_only.md')
if os.path.exists(fm):
    wt_mil = open(fm, 'rb').read()
    s = wt_mil.decode('utf-8', 'replace')
    p('tmp/ac22_milestone_only.md bytes=%d lines=%d' % (len(wt_mil), lines(s)))
    p('  Step-12 present=%s  "Step 12" count=%d  AC-22 PROVEN (PROVEN 2026-09-12) present=%s'
      % ('Step 12' in s, s.count('Step 12'), 'PROVEN 2026-09-12' in s))
    p('  AC-22 PROVEN twin sentence present=%s'
      % ('DeterministicSecondRunThroughProductionSurface' in s and 'twin' in s))
    p('  CLI real providers sentence still present=%s' % ('CLI real providers' in s))
else:
    p('tmp/ac22_milestone_only.md MISSING')
p('')

# -- 2) the report: parent row vs HEAD row for AC-22 --
par = blob(PARENT, REP); hd = blob(HEADOB, REP)
parrows = [l for l in par.split(chr(10)) if l.startswith('| AC-22')]
hdrows  = [l for l in hd.split(chr(10)) if l.startswith('| AC-22')]
p('report parent AC-22 rows: %d ; HEAD AC-22 rows: %d' % (len(parrows), len(hdrows)))
for l in parrows: p('  PARENT: %s' % l[:160])
for l in hdrows:  p('  HEAD  : %s' % l[:160])
p('report bytes: parent=%d HEAD=%d ; lines: parent=%d HEAD=%d' % (len(par.encode('utf-8')), len(hd.encode('utf-8')), lines(par), lines(hd)))

# -- 3) milestone parent vs HEAD byte/lines --
Mp = blob(PARENT, MIL); Mh = blob(HEADOB, MIL)
p('milestone bytes: parent=%d HEAD=%d ; lines: parent=%d HEAD=%d' % (len(Mp.encode('utf-8')), len(Mh.encode('utf-8')), lines(Mp), lines(Mh)))
p('  Step-11 tail in parent: "NOT started per GO" count=%d' % Mp.count('NOT started per GO'))
p('  Step-11 tail in HEAD  : "NOT started per GO" count=%d ; "PROVEN 2026-09-12" count=%d' % (Mh.count('NOT started per GO'), Mh.count('PROVEN 2026-09-12')))
p('  HEAD has Step-12 CLI paragraph (Step 12 count)=%d' % Mh.count('Step 12'))

# -- 4) test parent vs HEAD: pure twin add? --
Tp = blob(PARENT, TST); Th = blob(HEADOB, TST)
p('test bytes: parent=%d HEAD=%d ; lines: parent=%d HEAD=%d' % (len(Tp.encode('utf-8')), len(Th.encode('utf-8')), len(Tp), len(Th)))
td = git('diff', PARENT, HEADOB, '--', TST).decode('utf-8', 'replace')
adds = [l for l in td.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
rems = [l for l in td.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  test parent..HEAD: +%d -%d  pure-add=%s twin-anchor=%s'
  % (len(adds), len(rems), len(rems) == 0, any('DeterministicSecondRunThroughProductionSurface' in l for l in adds)))
tmp_t = os.path.join(REPO, 'tmp', 'ac22_tst_twin_only.cpp')
if os.path.exists(tmp_t):
    wt_t = open(tmp_t, 'rb').read()
    p('tmp/ac22_tst_twin_only.cpp bytes=%d lines=%d  == HEAD blob? %s'
      % (len(wt_t), lines(wt_t.decode('utf-8', 'replace')), wt_t == Th.encode('utf-8')))

# -- 5) CLI deferred files present in worktree --
p('CLI e2e test untracked exists: %s' % os.path.isfile(os.path.join(REPO, CLIT)))
cm = b(CL)
p('tests/CMakeLists.txt worktree bytes=%d contains CLI e2e activation=%s' % (len(cm), b'cli' in cm.lower() and (b'e2e' in cm.lower() or b'E2E' in cm)))
wm = b(MIL)
p('worktree milestone bytes=%d lines=%d ; Step-12 CLI paragraph present=%s' % (len(wm), lines(wm.decode('utf-8', 'replace')), b'Step 12' in wm))
# does worktree milestone == HEAD blob?
p('worktree milestone == HEAD blob: %s' % (wm == Mh.encode('utf-8')))

# -- 6) build AC-22-only docs from blobs --
def ac22_milestone_blob():
    """parent milestone + Step-11 AC-22 flip (PROVEN twin) — NO Step-12."""
    old = 'AC-22 (NOT started per GO)'
    assert Mp.count(old) == 1
    new = ('AC-22 (PROVEN 2026-09-12: `DeterministicSecondRunThroughProductionSurface` twin \u2014 '
           'two fresh `Engine::RunPipeline("p3_sparse_correction")` invocations through REAL '
           'GTSAM seams, semantic twin equivalance asserted, N5 duplicate refusal re-proven; '
           'Debug EXIT 0 + Release EXIT 0; full trajectory 23/23 in both)')
    return Mp.replace(old, new, 1), new

m_ac22, m_tail = ac22_milestone_blob()
p('')
p('AC-22-only milestone: bytes=%d lines=%d ; Step-12 absent=%s ; PROVEN twin present=%s'
  % (len(m_ac22.encode('utf-8')), lines(m_ac22), 'Step 12' not in m_ac22, 'PROVEN 2026-09-12' in m_ac22))

r_ac22 = hd  # report diff is exactly the single AC-22 row flip; HEAD report IS AC-22-only
p('AC-22-only report (== HEAD blob): bytes=%d lines=%d' % (len(r_ac22.encode('utf-8')), lines(r_ac22)))

t_ac22 = Tp + ('\n// AC-22 twin add (pure, via production surface)\n')  # placeholder; real add comes from diff
# Write out the actual twin-added lines from the diff to a separate doc
twin_block = ''.join(sorted(adds, key=len))  # crude; real block = raw added lines
open(os.path.join(REPO, 'tmp', 'ac22_tst_twin_add_block.txt'), 'w', encoding='utf-8', newline='\n').write(twin_block)

# write final docs
for fn, data in (('ac22_milestone_only.md', m_ac22), ('ac22_report_only.md', r_ac22)):
    with open(os.path.join(REPO, 'tmp', fn), 'w', encoding='utf-8', newline='\n') as f:
        f.write(data)

open(os.path.join(REPO, 'tmp', 'ac22_final_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_final_facts.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
