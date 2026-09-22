import subprocess, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def show(rev):
    lines = subprocess.run(['git','show',rev+':'+M], cwd=REPO, capture_output=True).stdout.decode('utf-8','replace').split('\n')
    print('===== %s : total lines %d =====' % (rev, len(lines)-1))
    for i,l in enumerate(lines, 1):
        s = l if len(l)<=130 else l[:130]+'…'
        print('%3d|%s' % (i,s))
    print()
show('fbef42b')
