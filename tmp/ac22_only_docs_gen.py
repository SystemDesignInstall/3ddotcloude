# -*- coding: utf-8 -*-
"""AC-22-ONLY doc builder — authoritative, byte-asserted.

Produces (LF, UTF-8) from the PARENT (fbef42b) blobs:
  tmp/ac22_milestone_md.py  (unused placeholder)
Actually writes the two AC-22-only markdown + the twin-test-source AC-22 block header,
each with hard byte assertions, then reports read-back facts. NO git mutation here.
"""
import subprocess, io, os, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
MIL  = r'docs/architecture/P3-milestone-status.md'
REP  = r'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST  = r'tests/unit/test_p3_trajectory_optimization.cpp'
PARENT = 'fbef42b'

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else r.stderr.decode('utf-8', 'replace')

def blob(rev, p):
    return subprocess.run(['git', 'show', rev + ':' + p], cwd=REPO, capture_output=True).stdout.decode('utf-8', 'replace')

def wb(p, data):
    with open(os.path.join(REPO, p), 'w', encoding='utf-8', newline='\n') as f:
        f.write(data)
    return data

def wf(p, data):
    with open(os.path.join(REPO, p), 'wb') as f:
        f.write(data.encode('utf-8'))
    return data

out = io.StringIO(); p = lambda *a: print(*a, file=out)

# ---------------------------------------------------------------------------
p('===== 1) FETCH PARENT blobs (byte-truth) =====')
pmil = blob(PARENT, MIL); prep = blob(PARENT, REP)
p('parent milestone: bytes=%d lines=%d' % (len(pmil.encode('utf-8')), pmil.count('\n') + 1))
p('parent report   : bytes=%d lines=%d' % (len(prep.encode('utf-8')), prep.count('\n') + 1))
p('parent milestone "Step 12" present : %s' % ('Step 12' in pmil))
p('parent report AC-22 row NOT PROVEN : %s' % ('| AC-22: Deterministic second run | No double-run | NOT PROVEN |' in prep))

# ---------------------------------------------------------------------------
p('')
p('===== 2) AC-22-only MILESTONE = parent + ONLY the Step-11 tail flip =====')
old_anchor = 'Remaining P3.1 items: AC-22 (NOT started per GO).'
assert pmil.count(old_anchor) == 1, 'anchor must be unique (found %d)' % pmil.count(old_anchor)
new_anchor = ('Remaining P3.1 items: AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` '
              'twin \u2014 two fresh `Engine::RunPipeline("p3_sparse_correction", \u2026)` invocations through '
              'PRODUCTION-SURFACE `Engine::RunPipeline` + REAL GTSAM seams (two fresh projects, bit-identical '
              'pinned inputs), semantic/twin equivalence asserted with real seams, N5 duplicate-refusal '
              're-proven; Debug EXIT 0 Release EXIT 0; full trajectory 23/23 in both, no future revision).')
ac22_mil = pmil.replace(old_anchor, new_anchor, 1)
assert 'Step 12' not in ac22_mil, 'AC-22-only milestone must NOT contain Step 12 (CLI deferred)'
assert 'CLI real providers' in ac22_mil   # Step-11's remaining-items label survives
assert old_anchor not in ac22_mil
assert new_anchor in ac22_mil
p('  AC-22-only milestone: bytes=%d lines=%d  Step-12 absent=%s'
      % (len(ac22_mil.encode('utf-8')), ac22_mil.count('\n') + 1, 'Step 12' not in ac22_mil))

# ---------------------------------------------------------------------------
p('')
p('===== 3) AC-22-only REPORT = parent + ONLY the AC-22 row flip =====')
old_row = '| AC-22: Deterministic second run | No double-run | NOT PROVEN |'
assert prep.count(old_row) == 1, 'report AC-22 row must be unique (found %d)' % prep.count(old_row)
new_row = ('| AC-22: Deterministic second run | Twin double-run via `Engine::RunPipeline` '
           '(two fresh projects, REAL GTSAM seams), Debug EXIT 0 + Release EXIT 0, semantic twin '
           'equiv + N5 re-proven | PROVEN |')
ac22_rep = prep.replace(old_row, new_row, 1)
assert old_row not in ac22_rep and new_row in ac22_rep
assert '| AC-22: Deterministic second run' in ac22_rep and 'PROVEN |' in ac22_rep
p('  AC-22-only report: bytes=%d lines=%d  AC-22 PROVEN=%s'
      % (len(ac22_rep.encode('utf-8')), ac22_rep.count('\n') + 1,
         'PROVEN |' in ac22_rep.split('\n')[ac22_rep.count('\n') - 4] if False else 'twin-PROVEN' in new_row))

# ---------------------------------------------------------------------------
p('')
p('===== 4) WRITE AC-22-only docs + verify read-back =====')
wb(os.path.join('tmp', 'ac22_milestone_only.md'), ac22_mil)
wb(os.path.join('tmp', 'ac22_report_only.md'), ac22_rep)
p('  wrote tmp/ac22_milestone_only.md  bytes=%d' % len(ac22_mil.encode('utf-8')))
p('  wrote tmp/ac22_report_only.md     bytes=%d' % len(ac22_rep.encode('utf-8')))

p('')
p('===== 5) FACT: report diff fbef42b..HEAD = EXACTLY the AC-22 row? =====')
d = git('diff', PARENT, '72b88d2', '--', REP)
adds = [l for l in d.split('\n') if l.startswith('+') and not l.startswith('+++')]
rems = [l for l in d.split('\n') if l.startswith('-') and not l.startswith('---')]
p('  report diff: +%d -%d  single-row=%s (adds)==[%r]' % (len(adds), len(rems), len(adds) == 1 and len(rems) == 1, adds[0][:120] if adds else ''))
assert len(adds) == 1 and len(rems) == 1, 'report diff must be exactly one AC-22 row flip'

p('')
p('===== 6) FACT: milestone diff fbef42b..HEAD = Step-11 flip + Step-12 CLI para =====')
dm = git('diff', PARENT, '72b88d2', '--', MIL)
md_adds = [l for l in dm.split('\n') if l.startswith('+') and not l.startswith('+++')]
md_rems = [l for l in dm.split('\n') if l.startswith('-') and not l.startswith('---')]
p('  milestone diff: +%d -%d ; Step-12-in-adds=%s ; AC22-twin-in-adds=%s'
      % (len(md_adds), len(md_rems), any('Step 12' in l for l in md_adds), any('twin' in l for l in md_adds)))
p('  milestone added lines:')
for l in md_adds:
    p('    + %s' % (l[:110] + '...' if len(l) > 110 else l))

# ---------------------------------------------------------------------------
p('')
p('===== DONE. Nothing git-mutated. =====')
open(os.path.join(REPO, 'tmp', 'ac22_only_docs_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_only_docs_facts.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
