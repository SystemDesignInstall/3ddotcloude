import subprocess, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
REPO = r'C:\Code\ClaudeDot\spatial-platform'
M = r'docs/architecture/P3-milestone-status.md'
def show(rev):
    return subprocess.run(['git','show',rev+':'+M], cwd=REPO, capture_output=True).stdout.decode('utf-8','replace')

# EXACT text of the PARENT milestone Step-11 bullet tail (line 136)
pmil = show('fbef42b').split('\n')
l136 = pmil[135]
print('PARENT L136 len=%d' % len(l136))
print('PARENT L136 tail-from-pos-1500:')
print(l136[1450:])
print()
# find every AC-22 / "Remaining P3.1 items:" occurrence with exact positions in parent
import re
for pat in [r'AC-22', r'AC-22 \(', r'Remaining P3\.1 items', r'NOT started', r'NOT started per GO', r'per GO']:
    ms = list(re.finditer(re.escape(pat) if not pat.startswith(r'AC-22 \(') else pat, l136))
    # simpler: use findall with plain
    hits = [m.start() for m in re.finditer(re.escape(pat), l136)]
    print('parent L136: %-22s -> %s' % (pat, hits[:6]))
print()
print('PARENT L136 last 130 chars repr (decode errs=None safe):')
print(repr(l136[-130:]))
