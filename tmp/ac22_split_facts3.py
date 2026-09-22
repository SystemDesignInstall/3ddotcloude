import subprocess
REPO=r'C:\Code\ClaudeDot\spatial-platform'
M=r'docs/architecture/P3-milestone-status.md'
T=r'tests/unit/test_p3_trajectory_optimization.cpp'
def show(rev,p):
    return subprocess.run(['git','show',rev+':'+p],cwd=REPO,capture_output=True).stdout.decode('utf-8','replace')
lines = show('72b88d2',M).split('\n')
print('--- HEAD milestone H166..H169 FULL (AC-22 PROVEN tail) ---')
for i in range(166,170):
    print('%d|%s' % (i, lines[i-1]))
print()
lines = show('fbef42b',M).split('\n')
print('--- PARENT milestone L136 FULL ---')
print('136|%s' % lines[135])
print()
print('--- PARENT milestone: last line + total ---')
print('total=%d last=%s' % (len(lines),lines[-1]))
