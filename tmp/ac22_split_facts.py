import subprocess, sys
REPO = r'C:\Code\ClaudeDot\spatial-platform'
def blob(rev, path):
    r = subprocess.run(['git','show', rev+':'+path], cwd=REPO, capture_output=True)
    return r.stdout.decode('utf-8')

print('=== PARENT fbef42b milestone Step-11 line (136) FULL tail after last `git diff` mention ===')
ls = blob('fbef42b','docs/architecture/P3-milestone-status.md').split('\n')
l = ls[135]
idx = l.rfind('NOT started per GO')
print('tail-start=', idx)
print('TAIL>>', l[idx-260:] if idx>260 else l)
print()
print('=== Who carries "AC-22 (" in PARENT? (need exactly ONE anchor: Step-11 tail) ===')
for i,ll in enumerate(ls,1):
    if 'AC-22 (' in ll or 'NOT started per GO' in ll or 'PROVEN per GO' in ll:
        j = ll.find('Remaining P3.1')
        seg = ll[j:j+160] if j>=0 else ll[max(0,ll.find('AC-22')-40):ll.find('AC-22')+160]
        print('%3d| …%s' % (i, seg))
        break
print()
print('=== Test file diff fbef42b..72b88d2 : PURE-ADD check (first 3 and last 3 +/- lines) ===')
d = blob('72b88d2','tests/unit/test_p3_trajectory_optimization.cpp')
p = blob('fbef42b','tests/unit/test_p3_trajectory_optimization.cpp')
print('HEAD test lines=%d  PARENT test lines=%d  delta=%d' % (len(d.split('\n')),len(p.split('\n')),len(d.split('\n'))-len(p.split('\n'))))
r = subprocess.run(['git','diff','fbef42b','72b88d2','--','tests/unit/test_p3_trajectory_optimization.cpp'],cwd=REPO,capture_output=True)
diff = r.stdout.decode('utf-8')
lines = [x for x in diff.split('\n') if x.startswith('+') or x.startswith('-')]
minus=[x for x in lines if x.startswith('-') and not x.startswith('---')]
plus=[x for x in lines if x.startswith('+') and not x.startswith('+++')]
print('added(+)=%d  removed(-)=%d' % (len(plus),len(minus)))
print('-- first 2 removed (should be NONE, i.e. pure add) --')
for x in minus[:2]: print(repr(x[:120]))
print('-- first 2 added --')
for x in plus[:2]: print(repr(x[:120]))
print('-- last 2 added --')
for x in plus[-2:]: print(repr(x[:120]))
