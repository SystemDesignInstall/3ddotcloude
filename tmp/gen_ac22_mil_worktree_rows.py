# -*- coding: utf-8 -*-
# Print the milestone Step-11 tail region BYTE-EXACT to a UTF-8 file (no truncation, no console).
import io, os
p=r'docs/architecture/P3-milestone-status.md'
b=open(p,'r',encoding='utf-8',newline='').read()
L=b.split('\n')
out=io.StringIO()
for i,l in enumerate(L,1):
    if 'Remaining P3.1' in l or 'AC-22' in l or 'Step 12' in l or 'Step-12' in l:
        out.write('L%d len=%d | %s\n' % (i,len(l),l))
open(r'tmp\ac22_mil_worktree_rows.txt','w',encoding='utf-8',newline='\n').write(out.getvalue())
print('WROTE bytes=%d' % len(out.getvalue().encode('utf-8')))
