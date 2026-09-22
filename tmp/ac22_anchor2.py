import subprocess
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def blob(rev):
    return subprocess.run(['git','show',rev+':'+M],cwd=REPO,capture_output=True).stdout.decode('utf-8')
def grep(rev,label):
    lines = blob(rev).split('\n')
    print('===== %s @%s : "AC-22"/"AC22"/"Remaining"/"per GO" anchors (trunc 140) =====' % (label,rev))
    for i,l in enumerate(lines,1):
        if ('AC-22' in l or 'AC22' in l or 'Remaining P3.1' in l or 'Remaining P3.1 items' in l or 'per GO' in l or 'NOT started' in l):
            s=l if len(l)<=140 else l[:140]+'…'
            print('%4d|%s' % (i,s))
    print()
grep('fbef42b','PARENT')
grep('72b88d2','HEAD')
