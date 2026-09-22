import subprocess, sys
repo = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def show(rev):
    out = subprocess.run(['git','show',rev+':'+M], cwd=repo, capture_output=True).stdout
    return out.decode('utf-8').split('\n')
def dump(rev,label,lo,hi):
    lines = show(rev)
    print('===== %s @%s : milestone lines %d..%d (trunc 140) =====' % (label,rev,lo,hi))
    for i in range(lo,min(hi,len(lines))+1):
        l=lines[i-1]
        s=l if len(l)<=140 else l[:140]+'…'
        print('%3d|%s' % (i,s))
    print()
    # find key anchors
    key=[('Step 12','Step-12 present?'),('AC-22 (PROVEN','AC22 PROVEN?'),('AC-22 (NOT','AC22 NOT?'),('Remaining P3.1 items','Remaining?'),('NOT started per GO','NOT-started?')]
    print('--- %s anchors ---' % label)
    for i,l in enumerate(lines,1):
        for (pat,label2) in key:
            if pat in l:
                s=l if len(l)<=120 else l[:120]+'…'
                print('L%4d [%s] %s' % (i,label2,s))
    print()
dump('fbef42b','PARENT ',133,172)
dump('72b88d2','HEAD   ',133,172)
