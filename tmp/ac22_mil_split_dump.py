import subprocess, sys
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def show(rev):
    b = subprocess.run(['git','show',rev+':'+M],cwd=REPO,capture_output=True).stdout
    return b.decode('utf-8').split('\n')
def dump(rev,label,lo,hi,w=170):
    lines=show(rev)
    print("===== {0} @{1} lines {2}..{3} (full width {4}) =====".format(label,rev,lo,hi,w))
    for i in range(lo,min(hi,len(lines))+1):
        l=lines[i-1]
        s=l if len(l)<=w else l[:w]+'…'
        print("{0,4}|{1}".format(i,s))
    print()
dump('fbef42b','PARENT',134,137)
dump('72b88d2','HEAD',136,138)
print("=== HEAD line 136 (Step-11) FULL ===")
ll = show('72b88d2')[135]
print(ll)
print()
print("=== PARENT line 136 (Step-11) FULL ===")
lp = show('fbef42b')[135]
print(lp)
print()
print("=== HEAD line 137 (Step-12 start) + 164-168 (tail) STATUS anchors ===")
for i in [137,164,165,166,167,168]:
    l=show('72b88d2')[i-1]
    s=l if len(l)<=170 else l[:170]+'…'
    print("{0,4}|{1}".format(i,s))
