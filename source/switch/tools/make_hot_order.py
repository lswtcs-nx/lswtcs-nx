# Usage: make_hot_order.py <output .ld> <elf> <run dir> [more run dirs]
# Writes a linker section ordering file: the guest functions the console sampled, hottest first, so they sit together instead of spread over 80 MB of code.
# Only the placement changes. Functions not sampled keep their place. The input is hwprof.txt from any run of a build with the same code.
import re,subprocess,bisect,collections,sys
out,elf=sys.argv[1:3]
nm='C:/devkitPro/msys2/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm.exe'
A=[];N=[]
for l in subprocess.run([nm,'-n','--defined-only',elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split(None,2)
    if len(p)==3 and p[1] in 'tTwW':A.append(int(p[0],16));N.append(p[2])
c=collections.Counter()
for run in sys.argv[3:]:
    for l in open(run+'/hwprof.txt',errors='replace'):
        if not l.startswith('s '):continue
        p=l.split()
        if len(p)<6 or p[5]=='0':continue
        o=int(p[5],16);j=bisect.bisect_right(A,o)-1
        if j>=0:c[N[j]]+=int(p[4])
hot=[]
for name,n in c.most_common():
    m=re.search(r'sub_([0-9A-F]{8})',name)
    if m and n>=2:hot.append('__imp__sub_'+m.group(1))
hot=list(dict.fromkeys(hot))
with open(out,'w',newline='\n') as f:
    f.write('.text : {\n')
    for h in hot:f.write(f'  *(.text.{h})\n')
    f.write('}\n')
print(len(hot),'functions')
