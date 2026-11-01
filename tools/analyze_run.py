"""Summarize one Switch run from frames.log and stdout.log; reads only those files."""
from pathlib import Path
import argparse,re

def percentile(values,fraction):
    values=sorted(values)
    return values[min(len(values)-1,int(len(values)*fraction))] if values else 0.0
def load_frames(path):
    rows,marks=[],[]
    for line in path.open(errors='replace'):
        mark=re.match(r'# mark (\d+) replay (\d+) t ([\d.]+)',line)
        if mark:marks.append((int(mark[1]),float(mark[3])));continue
        if line.startswith('#'):continue
        fields=line.split('|')[0].split()
        if len(fields)<9:continue
        try:rows.append(dict(replay=int(fields[0]),t=float(fields[2]),replay_ms=float(fields[5]),
                             gpuwait=float(fields[6]),hold=float(fields[7]),draws=int(fields[8])))
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
def print_stdout(path):
    lines=path.read_bytes().decode('latin-1').splitlines()
    print(f"\nSave commits: {sum('[SAVEFS] commit' in l for l in lines)}")
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
    print_marks(rows,marks)
    print_stdout(args.directory/'stdout.log')
if __name__=='__main__':main()
