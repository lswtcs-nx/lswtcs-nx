"""Summarize one Switch run from frames.log and stdout.log; reads only those files."""
from pathlib import Path
import argparse,re

def percentile(values,fraction):
    values=sorted(values)
    return values[min(len(values)-1,int(len(values)*fraction))] if values else 0.0
def load_frames(path):
    rows,marks=[],[]
    state,state_t=None,0.0
    for line in path.open(errors='replace'):
        mark=re.match(r'# mark (\d+) replay (\d+) t ([\d.]+)',line)
        if mark:marks.append((int(mark[1]),float(mark[3])));continue
        ab=re.match(r'# ab (\S+) replay (\d+) t ([\d.]+)',line)
        if ab:state,state_t=ab[1],float(ab[3]);continue
        if line.startswith('#'):continue
        parts=line.split('|')
        fields=parts[0].split()
        if len(fields)<9:continue
        try:
            row=dict(replay=int(fields[0]),t=float(fields[2]),replay_ms=float(fields[5]),
                     gpuwait=float(fields[6]),hold=float(fields[7]),draws=int(fields[8]),ab=state,ab_t=state_t)
            if len(parts)>=8:   # from 0.0.13-log7
                played,empty=parts[5].split()
                row.update(played=int(played),empty=int(empty),groups=int(parts[6]),copies=int(parts[7]))
            rows.append(row)
        except ValueError:continue
    for previous,row in zip(rows,rows[1:]):row['dt']=(row['t']-previous['t'])*1000
    return [row for row in rows if 'dt' in row],marks
def print_windows(rows,seconds):
    print(f'\nFrames per {seconds} s window; dt in ms; steady excludes frames with a shader hold')
    print('    t  fps  steady_fps  dt_p50  dt_p95  dt_max  replay  gpuwait  hold_ms  draws  >100ms')
    for start in range(0,int(rows[-1]['t'])+1,seconds):
        window=[row for row in rows if start<=row['t']<start+seconds]
        if not window:continue
        dts=[row['dt'] for row in window]
        steady=[row['dt'] for row in window if not row['hold']]
        steady_fps=1000/(sum(steady)/len(steady)) if steady else 0
        print(f"{start:5d} {len(window)/seconds:4.1f} {steady_fps:11.1f} {percentile(dts,.5):7.1f} {percentile(dts,.95):7.1f} {max(dts):7.0f}"
              f" {percentile([r['replay_ms'] for r in window],.5):7.1f} {percentile([r['gpuwait'] for r in window],.5):8.1f}"
              f" {sum(r['hold'] for r in window):8.0f} {percentile([r['draws'] for r in window],.5):6.0f} {sum(d>100 for d in dts):7d}")
def print_marks(rows,marks):
    if not marks:return
    print('\nMarks: frames in the 10 s around each one')
    for number,t in marks:
        near=[row for row in rows if abs(row['t']-t)<=5]
        print(f"  mark {number} at {t:.1f} s: dt_p50 {percentile([r['dt'] for r in near],.5):.1f} ms, "
              f"dt_max {max([r['dt'] for r in near],default=0):.0f} ms, draws {percentile([r['draws'] for r in near],.5):.0f}")
BANDS=((0,300),(300,800),(800,1300),(1300,1900),(1900,99999))
def print_ab(rows):
    """Settings compared over the same session: frames in the first 2 s of a state and hitches are left out."""
    states=sorted({r['ab'] for r in rows if r['ab']})
    if not states:return
    print('\nA/B states (M merging, A audio first, W walk off rotation) by recorded draws; speed = audio played per real time')
    print('state      draws   frames   s   fps  audio%  empty%  replay  gpuwait  merged%')
    pairs=list(zip(rows,rows[1:]))
    for low,high in BANDS:
        for state in states:
            use=[(a,b) for a,b in pairs if a['ab']==state==b['ab'] and low<=b['draws']<high and b['t']-b['ab_t']>=2 and b['dt']<300]
            if len(use)<20:continue
            seconds=sum(b['t']-a['t'] for a,b in use)
            have=[(a,b) for a,b in use if 'played' in a and 'played' in b]
            speed=sum(b['played']-a['played'] for a,b in have)*256/48000/max(seconds,1e-6)*100
            empty=sum(b['empty']-a['empty'] for a,b in have)/10/max(seconds,1e-6)
            merged=sum(b.get('groups',0)>0 for a,b in use)*100/len(use)
            print(f"{state:8s} {low:5d}+ {len(use):7d} {seconds:4.0f} {len(use)/seconds:5.1f} {speed:6.0f} {empty:7.1f}"
                  f" {percentile([b['replay_ms'] for a,b in use],.5):7.1f} {percentile([b['gpuwait'] for a,b in use],.5):8.1f} {merged:8.0f}")
def print_diagnostics(lines):
    clocks=[l for l in lines if l.startswith('[CLOCK] t=')]
    service=[l for l in lines if l.startswith('[CLOCK] services')]
    if service:print('\n'+service[0])
    if clocks:
        values={}
        for line in clocks:
            for key,value in re.findall(r'(cpu|gpu|emc|gpu_load|soc|pcb|battery)=(-?[\d.]+)',line):values.setdefault(key,[]).append(float(value))
        print(f'{len(clocks)} clock samples, min / median / max:')
        for key,value in values.items():print(f'  {key:9s} {min(value):8.1f} {percentile(value,.5):8.1f} {max(value):8.1f}')
        print(' ',clocks[-1])
    checks=[l for l in lines if l.startswith('[UNTILECHECK]')]
    if checks:
        flagged=[l for l in checks if 'FLAGGED' in l]
        print(f'\nMerged against unmerged: {len(checks)} checks, {len(flagged)} flagged')
        for line in (flagged or checks)[:12]:print(' ',line[:170])
    rejected=[0,0,0,0,0]
    for line in lines:
        match=re.match(r'\[GLUNTILE\] rejected since last report: uncheckable (\d+), content (\d+), tile state (\d+), bottom (\d+), order (\d+)',line)
        if match:rejected=[a+int(b) for a,b in zip(rejected,match.groups())]
    if any(rejected):print('\nMerge refusals: uncheckable {}, content {}, tile state {}, bottom {}, order {}'.format(*rejected))
    for tag,title in (('[CAPTIME]','Captures'),('[GLHIST]','Frame history'),('[CAPSAVE]','Background saves')):
        found=[l for l in lines if l.startswith(tag)]
        if found:
            print(f'\n{title}:')
            for line in found[:12]:print(' ',line[:170])
    spikes=[l for l in lines if l.startswith('[GSPIKE]')]
    if spikes:
        print(f'\nGuest frames over LSWTCS_SPIKEMS: {len(spikes)}; slowest:')
        timed=sorted(spikes,key=lambda l:-float(re.search(r'frame=([\d.]+)',l)[1]))
        for line in timed[:12]:print(' ',line[:230])
def print_stdout(path):
    lines=path.read_bytes().decode('latin-1').splitlines()
    print_diagnostics(lines)
    print(f"\nSave commits: {sum('[SAVEFS] commit' in l for l in lines)}")
    tiles=[re.search(r'of (\d+) draws.*untiled: (\d+) tile groups, (\d+) tile repeats',l) for l in lines if 'untiled:' in l]
    heavy=[match for match in tiles if match and int(match[1])>=300]
    print(f"Sampled frames with at least 300 draws: {len(heavy)}, with a merged tile group: {sum(int(m[2])>0 for m in heavy)}")
    audio=[l for l in lines if l.startswith('[AUDIO] ') and 'buffers played' in l]
    if audio:print('Last audio line:',audio[-1])
    print('Audio windows with an empty device queue ([AUDIOQ]):')
    queues=[l for l in lines if l.startswith('[AUDIOQ]')]
    for line in queues:
        if 'empties=0 ' not in line:print(' ',line)
    print(f"  {len(queues)} windows logged; absent lines mean a build older than 0.0.12-log1")
    gpu=[l for l in lines if l.startswith('[GLGPU] replay')]
    if gpu:
        print('\nGPU samples ([GLGPU]):')
        for line in gpu[:60]:print(' ',line[:170])
    tops={}
    for line in lines:
        match=re.match(r'\[GLGPU\]   program (\d+) \(PS (\w+)\): ([\d.]+) ms over (\d+) draws',line)
        if match:
            entry=tops.setdefault(match[2],[0.0,0]);entry[0]+=float(match[3]);entry[1]+=int(match[4])
    if tops:
        print('\nMost expensive pixel shaders, summed over samples:')
        for key,(ms,draws) in sorted(tops.items(),key=lambda item:-item[1][0])[:10]:
            print(f'  PS {key}: {ms:.1f} ms over {draws} draws')
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path,help='folder holding frames.log and stdout.log')
    parser.add_argument('--window',type=int,default=30,help='window length in seconds')
    args=parser.parse_args()
    rows,marks=load_frames(args.directory/'frames.log')
    print(f"{len(rows)} frames over {rows[-1]['t']:.0f} s; crash.log present: {(args.directory/'crash.log').exists()}")
    print_windows(rows,args.window)
    print_ab(rows)
    print_marks(rows,marks)
    print_stdout(args.directory/'stdout.log')
if __name__=='__main__':main()
