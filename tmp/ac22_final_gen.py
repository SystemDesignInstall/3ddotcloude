# -*- coding: utf-8 -*-
"""FINAL AC-22 SPLIT — build + verify (writes ONLY tmp/, no git mutation).

Goal: commit A on top of fbef42b == "AC-22 only" (3 files):
  1. docs/architecture/P3-milestone-status.md        -> parent + Step-11 AC-22 flip (NO Step-12 CLI)
  2. docs/architecture/P3.1-production-sparse-correction-verification-report.md -> parent + single AC-22 row flip
  3. tests/unit/test_p3_trajectory_optimization.cpp  -> parent + pure AC-22 twin block (assert no CLI/Step-12 refs)
Deferred (stays uncommitted): Step-12 CLI paragraph + tests/CMakeLists.txt activation + tests/unit/test_cli_sparse_correction_e2e.cpp.

Outputs (self-asserting):
  tmp/ac22_final_milestone_only.md
  tmp/ac22_final_report_only.md
  tmp/ac22_final_test_only.md      (identity check vs parent+pure-twin)
  tmp/ac22_final_step12_paragraph.md  (explicit deferred content)
  tmp/ac22_final_facts.txt
"""
import subprocess, io, os, hashlib, re

REPO   = r'C:\Code\ClaudeDot\spatial-platform'
MIL    = 'docs/architecture/P3-milestone-status.md'
REP    = 'docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST    = 'tests/unit/test_p3_trajectory_optimization.cpp'
CLITST = 'tests/unit/test_cli_sparse_correction_e2e.cpp'
CMAKE  = 'tests/CMakeLists.txt'
PARENT = 'fbef42b'
HEAD   = '72b88d2'

def git(*a):
    r = subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('git %s failed: %s' % (a, r.stderr.decode('utf-8', 'replace')[:300]))
    return r.stdout.decode('utf-8', 'replace')

def blob(rev, p):
    return git('show', rev + ':' + p)

def md5b(s):
    return hashlib.md5(s.encode('utf-8')).hexdigest()

def bz(p):  # worktree bytes
    return os.path.getsize(os.path.join(REPO, p))

out = io.StringIO()
def p(*a):
    print(*a, file=out)

# ---------------------------------------------------------------- fetch ----
M_par = blob(PARENT, MIL); M_head = blob(HEAD, MIL)
R_par = blob(PARENT, REP); R_head = blob(HEAD, REP)
T_par = blob(PARENT, TST); T_head = blob(HEAD, TST)

p('PARENT milestone: bytes=%d lines=%d' % (len(M_par.encode('utf-8')), M_par.count(chr(10)) + 1))
p('HEAD   milestone: bytes=%d lines=%d' % (len(M_head.encode('utf-8')), M_head.count(chr(10)) + 1))
p('PARENT report  : bytes=%d lines=%d' % (len(R_par.encode('utf-8')), R_par.count(chr(10)) + 1))
p('HEAD   report  : bytes=%d lines=%d' % (len(R_head.encode('utf-8')), R_head.count(chr(10)) + 1))
p('PARENT test    : bytes=%d lines=%d' % (len(T_par.encode('utf-8')), T_par.count(chr(10)) + 1))
p('HEAD   test    : bytes=%d lines=%d' % (len(T_head.encode('utf-8')), T_head.count(chr(10)) + 1))

# ===========================================================================
p('')
p('===== 1) MILESTONE: assert HEAD = parent + Step-11 AC-22 flip + Step-12 CLI paragraph (pure adds) =====')
mdiff = git('diff', PARENT, HEAD, '--', MIL)
m_adds = [l for l in mdiff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
m_rems = [l for l in mdiff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  milestone parent..HEAD: +%d -%d (assert: pure-add, no removals)' % (len(m_adds), len(m_rems)))
assert len(m_rems) == 0, 'milestone parent..HEAD must be pure-add (Step-12 CLI paragraph + AC-22 flip); found %d removals' % len(m_rems)
p('  Step-11-flip line:')
flip_lines = [l for l in m_adds if 'AC-22 (PROVEN' in l]
p('    %s' % (flip_lines[0] if flip_lines else '!! NO AC-22-PROVEN line in +diff — flip may live inside Step-12 paragraph'))
assert len(flip_lines) >= 1, 'need at least one AC-22-PROVEN line in HEAD milestone'
# Both AC-22 flip AND Step-12 CLI paragraph must be present in HEAD:
p('  "Step 12" present in HEAD=%s ; "Step 12" in parent=%s' % ('Step 12' in M_head, 'Step 12' in M_par))
assert 'Step 12' in M_head and 'Step 12' not in M_par
p('  "CLI real providers" in HEAD=%s' % ('CLI real providers' in M_head))

# --- Build AC-22-ONLY milestone: parent + Step-11 flip ONLY (no Step-12) ---
anchor_old = 'AC-22 (NOT started per GO)'
assert M_par.count(anchor_old) >= 1, 'anchor must be present in parent'
# The flip rewrites the SCALAR marker: use the exact PROVEN phrase from HEAD's AC-22 line(s).
proven_phrase = None
for l in flip_lines:
    m = re.search(r'AC-22 \(PROVEN 2026-09-12[^)]*\)', l)
    if m:
        proven_phrase = m.group(0)
        break
assert proven_phrase, 'must extract AC-22 (PROVEN ...) phrase from HEAD milestone'
p('  extracted AC-22 PROVEN phrase: %s' % proven_phrase)
m_ac22 = M_par.replace(anchor_old, proven_phrase, 1)
assert 'Step 12' not in m_ac22, 'AC-22-only milestone must NOT contain Step 12'
p('  AC-22-only milestone: bytes=%d lines=%d Step-12-absent=%s AC-22-PROVEN=%s'
      % (len(m_ac22.encode('utf-8')), m_ac22.count(chr(10)) + 1, 'Step 12' not in m_ac22, proven_phrase in m_ac22))
if os.path.exists(os.path.join(REPO, 'tmp', 'ac22_milestone_only.md')):
    prev = open(os.path.join(REPO, 'tmp', 'ac22_milestone_only.md'), 'r', encoding='utf-8').read()
    p('  PREVIOUS tmp/ac22_milestone_only.md: bytes=%d lines=%d matches=%s'
          % (len(prev.encode('utf-8')), prev.count(chr(10)) + 1, prev == m_ac22))
open(os.path.join(REPO, 'tmp', 'ac22_final_milestone_only.md'), 'w', encoding='utf-8', newline='\n').write(m_ac22)

# ===========================================================================
p('')
p('===== 2) REPORT: parent..HEAD must be EXACTLY the single AC-22 row flip =====')
rdiff = git('diff', PARENT, HEAD, '--', REP)
r_adds = [l for l in rdiff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
r_rems = [l for l in rdiff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  report parent..HEAD: +%d -%d (assert: single AC-22 row)' % (len(r_adds), len(r_rems)))
assert len(r_adds) == 1 and len(r_rems) == 1, 'report parent..HEAD must differ by exactly one AC-22 row'
p('  - %s' % r_rems[0][1:170])
p('  + %s' % r_adds[0][1:170])
assert 'AC-22' in r_rems[0] and 'AC-22' in r_adds[0]
r_ac22 = R_head   # HEAD report == parent + AC-22 row flip (already AC-22-only; no CLI/Step-12)
p('  AC-22-only report = HEAD blob: bytes=%d lines=%d' % (len(r_ac22.encode('utf-8')), r_ac22.count(chr(10)) + 1))
p('  CLI/Step-12 refs in report=%s/%s'
      % ('CLI' in r_ac22, 'Step 12' in r_ac22))
open(os.path.join(REPO, 'tmp', 'ac22_final_report_only.md'), 'w', encoding='utf-8', newline='\n').write(r_ac22)

# ===========================================================================
p('')
p('===== 3) TEST: parent + pure AC-22 twin block; assert NO CLI/Step-12 refs added =====')
tdiff = git('diff', PARENT, HEAD, '--', TST)
t_adds = [l for l in tdiff.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
t_rems = [l for l in tdiff.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('  test parent..HEAD: +%d -%d' % (len(t_adds), len(t_rems)))
assert len(t_rems) == 0, 'test must be pure-add (the AC-22 twin); got %d removals' % len(t_rems)
bad = [l for l in t_adds if ('Step 12' in l) or ('E2E' in l) or ('RunCommand' in l) or ('CLI real' in l) or ('test_cli' in l)]
assert not bad, 'AC-22 twin test add must have zero CLI/Step-12 refs; found: %r' % bad[:3]
twin_anchor = 'DeterministicSecondRunThroughProductionSurface'
p('  twin anchor in added lines: %d occurrences (pure AC-22 twin block)' % sum(1 for l in t_adds if twin_anchor in l))
assert any(twin_anchor in l for l in t_adds)
t_ac22 = T_head   # HEAD test == parent + pure twin add
p('  AC-22-only test = HEAD blob: bytes=%d lines=%d ; twin-pure=%s'
      % (len(t_ac22.encode('utf-8')), t_ac22.count(chr(10)) + 1, not bad))
open(os.path.join(REPO, 'tmp', 'ac22_final_test_only.md'), 'w', encoding='utf-8', newline='\n').write(t_ac22)

p('')
p('===== 4) DEFERRED Step-12 extract (explicit; NOT part of commit A) =====')
# the Step-12 CLI paragraph = the part of HEAD milestone that is NOT in the AC-22-only milestone
step12_par = M_head.replace(m_ac22, '', 1) if m_ac22 in M_head else None
if step12_par is None:
    # build: HEAD minus (AC-22-only) → find "Step 12" paragraph boundaries
    i12 = M_head.find('Step 12')
    i11 = M_head.rfind('- **Step 11', 0, i12)
    step12_par = M_head[i11:i12] + 'Step 12' + M_head[m(0,0)] if False else None
# simpler: paragraph = from "**Step 12" line to end of file (milestone tail after Step 11)
lines = M_head.split(chr(10))
start = next((i for i, l in enumerate(lines) if '**Step 12' in l), None)
if start is not None:
    step12_par = chr(10).join(lines[start:])
p('  Step-12 CLI paragraph (deferred): starts at line=%s ; bytes=%d lines=%d'
      % (start, len(step12_par.encode('utf-8')), step12_par.count(chr(10)) + 1))
open(os.path.join(REPO, 'tmp', 'ac22_final_step12_paragraph.md'), 'w', encoding='utf-8', newline='\n').write(step12_par)

# ===========================================================================
p('')
p('===== 5) FACT summary =====')
p('  commit A (AC-22 only, staged): milestone_only + report_only + test_only')
p('  deferred (worktree, uncommitted): Step-12 CLI paragraph + %s + %s' % (CMAKE, CLITST))
p('  anchor md5: milestone_only=%s' % md5b(m_ac22))
p('  anchor md5: report_only   =%s' % md5b(r_ac22))
p('  anchor md5: test_only     =%s' % md5b(t_ac22))
open(os.path.join(REPO, 'tmp', 'ac22_final_facts.txt'), 'w', encoding='utf-8', newline='\n').write(out.getvalue())
print('WROTE tmp/ac22_final_facts.txt bytes=%d' % len(out.getvalue().encode('utf-8')))
