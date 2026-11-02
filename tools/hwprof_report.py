"""Names the samples of hwprof.txt (LSWTCS_HWPROF) from the ELF's symbols; reads only those files.

For each thread and draw band: its CPU, and where its samples are by exact function (pc), with blocking system calls
shown under the guest or runtime function that called them. Chains come from a stack scan, so they are indications.
"""
from pathlib import Path
import argparse,bisect,collections,re,subprocess

BANDS=('light (<300 draws)','medium (<1300)','heavy (1300+)','no frame for over 1 s')
def load_symbols(nm,elf):
    out=subprocess.run([str(nm),'-n','--defined-only',str(elf)],capture_output=True,text=True,check=True).stdout
    addresses,names=[],[]
    for line in out.splitlines():
        parts=line.split()
        if len(parts)==3 and parts[1] in 'tTwW':
            addresses.append(int(parts[0],16));names.append(parts[2])
    return addresses,names
def symbol(symbols,offset):
    addresses,names=symbols
    index=bisect.bisect_right(addresses,offset)-1
    if index<0:return hex(offset)
    guest=re.match(r'_Z\d+(sub_[0-9A-F]+)R10PPCContextPh$',names[index])   # a generated guest function
    return guest[1] if guest else names[index]
def is_wait(name):
    return name.startswith('svc') or 'WaitSynchronization' in name or 'ArbitrateLock' in name
def is_runtime_wrapper(name):
    return is_wait(name) or re.match(r'_?_?(pthread|cnd|mtx|mutex|thrd|condvar|usleep|nanosleep|__retarget|_?_?libc|std::|_ZNSt|_ZSt)',name) is not None
def parse(path):
    threads,samples,cpu={}, [],collections.defaultdict(list)
    for line in path.read_text(errors='replace').splitlines():
        parts=line.split()
        if not parts:continue
        if parts[0]=='thread' and len(parts)>=9:
            threads[int(parts[1])]=f'{parts[2]} (sched {parts[4]})'
            if float(parts[6])>=0:cpu[int(parts[1])].append(float(parts[6]))
        elif parts[0]=='s' and len(parts)>=6:
            samples.append((int(parts[1]),int(parts[2]),int(parts[3]),int(parts[4]),[int(x,16) for x in parts[5:]]))
    return threads,samples,cpu
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path,help='folder holding hwprof.txt')
    parser.add_argument('--elf',type=Path,required=True,help='lswtcs.elf of the same build')
    parser.add_argument('--nm',type=Path,default=Path('aarch64-none-elf-nm'))
    parser.add_argument('--band',type=int,default=2,help='0 light, 1 medium, 2 heavy, 3 no frame for over 1 s: what a freeze waits on')
    parser.add_argument('--top',type=int,default=14)
    args=parser.parse_args()
    symbols=load_symbols(args.nm,args.elf)
    threads,samples,cpu=parse(args.directory/'hwprof.txt')
    print(f'{len(samples)} distinct stacks, band: {BANDS[args.band]}')
    for target,name in sorted(threads.items()):
        rows=[s for s in samples if s[0]==target and s[1]==args.band]
        total=sum(s[3] for s in rows)
        if not total:continue
        used=cpu.get(target,[])
        mean=sum(used)/len(used) if used else -1
        turn=sum(s[3] for s in rows if s[2])
        print(f'\n{name}: {total} samples, {turn*100/total:.0f} % holding the turn, CPU over the whole run {mean:.1f} %')
        for label,chosen in (('holding the turn',[s for s in rows if s[2]]),('not holding it',[s for s in rows if not s[2]])):
            weight=sum(s[3] for s in chosen)
            if not weight:continue
            leaf,callers=collections.Counter(),collections.Counter()
            for row in chosen:
                names=[symbol(symbols,f) for f in row[4] if f]
                if not names:continue
                leaf[names[0]]+=row[3]
                if is_wait(names[0]):
                    caller=next((n for n in names[1:] if not is_runtime_wrapper(n)),'?')
                    callers[f'{names[0]} <- {caller}']+=row[3]
            waits=sum(c for n,c in leaf.items() if is_wait(n))
            print(f'  {label}: {weight} samples, {waits*100/weight:.0f} % blocked in a system call')
            for function,count in leaf.most_common(args.top):
                print(f'    {count*100/weight:5.1f} %  {function}')
            for chain,count in callers.most_common(8):
                print(f'    wait {count*100/weight:5.1f} %  {chain}')
if __name__=='__main__':main()
