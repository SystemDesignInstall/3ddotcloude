# -*- coding: utf-8 -*-
"""Authoritative AC-22 split facts + AC-22-only doc construction.

READS git only (parent fbef42b, head obj 72b88d2 area) + worktree.
WRITES only under tmp/ (tmp/ac22_split_facts.txt + 3 AC-22-only docs).
NO git mutation. Bytes self-asserted from git blobs.
"""
import subprocess, io, os, hashlib

REPO='C:\Code\ClaudeDot\spatial-platform'
MIL='docs/architecture/P3-milestone-status.md'
REP='docs/architecture/P3.1-production-sparse-correction-verification-report.md'
TST='tests/unit/test_p3_trajectory_optimization.cpp'
CLITST='tests/unit/test_cli_sparse_correction_e2e.cpp'
CMAKE='tests/CMakeLists.txt'
PARENT='fbef42b'
HEADSHA='72b88d2'
P2='fbef42b'      # parent commit
H2='72b88d2'      # head object (72b88d2...)

def git(*a):
    r=subprocess.run(['git']+list(a),cwd=REPO,capture_output=True)
    return r.stdout

def blob(rev,path):
    return git('show',rev+':'+path)

def tf(buffer):  # truncated-safe show
    return buffer.decode('utf-8','replace')

def md5(b):
    return hashlib.md5(b).hexdigest()

def lines(s):
    return s.count(chr(10))+1

out=io.StringIO()
def p(*a):
    print(*a,file=out)

# ---------------------------------------------------------------------------
def git_diff(*args):
    return git('diff',*args)

# ---- parent milestone: no Step 12, AC-22 NOT-started anchor ----
M_par=blob(PARENT,MIL); M_head=blob(HEADSHA,MIL)
p('MILESTONE parent: bytes=%d lines=%d  step12=%s  AC22-Anchor="%s"'%(
    len(M_par),lines(M_par.decode('utf-8','replace')),
    b'Step 12' in M_par,
    (b'Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO)' in M_par and 'found') or 'nf'))
assert b'Step 12' not in M_par
assert M_par.count(b'AC-22 (NOT started per GO)')==1
p('MILESTONE HEAD: bytes=%d lines=%d  step12=%s  AC22PROVEN=%s'%(
    len(M_head),lines(M_head.decode('utf-8','replace')),
    b'Step 12' in M_head, b'AC-22 (PROVEN' in M_head))
# diff parent..HEAD milestone: what added?
md=git_diff(PARENT,HEADSHA,'--',MIL).decode('utf-8','replace')
adds=[l for l in md.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
rems=[l for l in md.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('MIL parent..HEAD diff: +%d -%d'%(len(adds),len(rems)))
p('  AC-22 PROVEN line present in adds=%s','Step 12' in str(adds))
p('  CLI refs in adds=%s','cli' in md.lower())
for l in adds:
    if 'AC-22' in l or 'Step 12' in l or 'CLI' in l:
        p('    + '+l[:150])
# NOTE: milestone line acquired BOTH Step-12 CLI paragraph AND AC-22 PROVEN twin flip.

# REPORT -------------------------------------------------------------------
R_par=blob(PARENT,REP); R_head=blob(HEADSHA,REP)
p('')
p('REPORT parent: bytes=%d lines=%d AC22row(parent)="%s"'%(
    len(R_par),lines(R_par.decode('utf-8','replace')),
    [l for l in R_par.decode('utf-8','replace').split(chr(10)) if l.startswith('| AC-22')]))
p('REPORT HEAD: bytes=%d lines=%d AC22row(HEAD)="%s"'%(
    len(R_head),lines(R_head.decode('utf-8','replace')),
    [l for l in R_head.decode('utf-8','replace').split(chr(10)) if l.startswith('| AC-22')]))
rd=git_diff(PARENT,HEADSHA,'--',REP).decode('utf-8','replace')
p('REPORT parent..HEAD diff: +%d -%d ; AC-22-only?=%s'%(
    rd.count(chr(10)-rd.count('')),len([l for l in rd.split(chr(10)) if l.startswith('-') and not l.startswith('---')]),
    'CLI' not in rd))

# TEST ----------------------------------------------------------------------
T_par=blob(PARENT,TST); T_head=blob(HEADSHA,TST)
td=git_diff(PARENT,HEADSHA,'--',TST).decode('utf-8','replace')
tadds=[l for l in td.split(chr(10)) if l.startswith('+') and not l.startswith('+++')]
trems=[l for l in td.split(chr(10)) if l.startswith('-') and not l.startswith('---')]
p('')
p('TEST parent: bytes=%d lines=%d ; HEAD: bytes=%d lines=%d'%(
    len(T_par),lines(T_par.decode('utf-8','replace')),len(T_head),lines(T_head.decode('utf-8','replace'))))
p('TEST parent..HEAD diff: +%d -%d  step12refs-in-adds=%s  cli-in-adds=%s'%(
    len(tadds),len(trems),'Step 12' in ' '.join(tadds),'cli' in ' '.join(tadds).lower()))
assert len(trems)==0,'AC-22 twin must be pure add (no removals)'
assert not any('Step 12' in l or 'cli' in l.lower() for l in tadds),'twin add must not reference CLI/Step-12'
p('  twin anchor present: %s'% any('DeterministicSecondRun' in l or 'Deterministic second run' in l.lower() for l in tadds))
p('')

# CLI deferred --------------------------------------------------------------
p('CLI e2e test untracked(worktree): %s'% os.path.exists(os.path.join(REPO,CLITST)))
import glob
cm=open(os.path.join(REPO,CMAKE),'r',encoding='utf-8').read()
p('tests/CMakeLists.txt worktree contains CLI e2e activation: %s'% ('cli' in cm.lower() and 'sparse' in cm.lower()))
p('  (activation is DEFERRED — stays uncommitted)')

# ---------------------------------------------------------------------------
# BUILD AC-22-only docs
# 1) MILESTONE AC-22-only = PARENT + AC-22 flip (PROVEN twin), NO Step-12
old_tail='Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO)'
new_tail='AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin \u2014 two fresh `Engine::RunPipeline("p3_sparse_correction",\u2026)` invocations through REAL GTSAM seams, semantic twin equivalence asserted, N5 duplicate-refusal re-proven; Debug EXIT 0 + Release EXIT 0, full trajectory 23/23)'
assert M_par.count(old_tail.encode('utf-8'))==1
M_ac=M_par.decode('utf-8','replace').replace(old_tail,new_tail,1)
assert 'Step 12' not in M_ac
p('AC22-ONLY milestone: bytes=%d lines=%d ; Step-12 absent=%s ; PROVEN twin present=%s'%(
    len(M_ac.encode('utf-8')),lines(M_ac),'Step 12' not in M_ac,'twin' in M_ac))

# 2) REPORT AC-22-only = PARENT + AC-22 row flip
old_row=[l for l in R_par.decode('utf-8','replace').split(chr(10)) if l.startswith('| AC-22')][0]
new_row='| AC-22: Deterministic second run | Twin via `Engine::RunPipeline` (two fresh projects, REAL GTSAM seams), Debug EXIT 0 + Release EXIT 0, full 23/23 | PROVEN |'




R_ac=R_par.decode('utf-8','replace').replace(old_row,new_row,1)
assert R_ac.count('| AC-22: Deterministic second run |')==1 and 'PROVEN' in R_ac
p('AC22-ONLY report: bytes=%d lines=%d ; single-row flip applied=%s'%(
    len(R_ac.encode('utf-8')),lines(R_ac),'Twin via' in R_ac))

# 3) TEST AC-22-only = PARENT + pure twin add (the twin block from HEAD diff)
twin_block=''.join(l[1:]+chr(10) for l in tadds if not l.startswith('+++'))
# find the exact splice: twin added at end of parent test (per HEAD blob = parent+twin)
T_ac=T_head.decode('utf-8','replace')  # HEAD test == parent + pure twin add (asserted above)
p('AC22-ONLY test   : bytes=%d lines=%d (== HEAD blob, since twin is the ONLY add)'%(
    len(T_ac.encode('utf-8')),lines(T_ac)))

# write tmp docs
for fn,data in [('tmp/ac22_milestone_only.md',M_ac),('tmp/ac22_report_only.md',R_ac),('tmp/ac22_twin_only.cpp',T_ac)]:
    with open(os.path.join(REPO,fn),'w',encoding='utf-8',newline=chr(10)) as f:
        f.write(data)
    if fn=='tmp/ac22_milestone_only.md':
        # dedicated AC-22-only milestone doc
        with open(os.path.join(REPO,'tmp','ac22_milestone_ac22_only.md'),'w',encoding='utf-8',newline=chr(10)) as f2:
            f2.write(M_ac)

open(os.path.join(REPO,'tmp','ac22_split_facts.txt'),'w',encoding='utf-8',newline='').write(out.getvalue())
print('WROTE tmp/ac22_split_facts.txt: %d bytes'%len(out.getvalue().encode('utf-8')))
