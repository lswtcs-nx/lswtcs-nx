"""Check or apply the recorded generated-source delta; no game generation/download."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,tempfile

BASE=Path(__file__).resolve().parents[1]
def normalized(raw):
    try:text=raw.decode('utf-8-sig')
    except UnicodeDecodeError:text=raw.decode('cp1252')
    return text.replace('\r\n','\n').encode('utf-8')
def load(which):
    name='folded-generation-baseline.json' if which=='baseline' else 'generated-output-target.json'
    return json.loads((BASE/'provenance'/name).read_text(encoding='utf-8'))
def check(root,manifest):
    root=Path(root).resolve()
    if not root.is_dir():raise ValueError('Generated source directory does not exist')
    expected={entry['file']:entry for entry in manifest}
    actual={p.name for p in root.iterdir() if p.is_file() and p.suffix in {'.cpp','.h','.inc'}}
    problems=[]
    for name in sorted(actual ^ expected.keys()):problems.append('unexpected/missing file: '+name)
    for name,entry in expected.items():
        if Path(name).name!=name:raise ValueError('Manifest names must be plain filenames')
        p=root/name
        if not p.is_file():continue
        if p.is_symlink():raise ValueError('Refusing generated-source symlinks')
        if hashlib.sha256(normalized(p.read_bytes())).hexdigest()!=entry['normalized_sha256']:
            problems.append('content mismatch: '+name)
    if problems:raise ValueError('Baseline/target mismatch; no patch applied.\n'+'\n'.join(problems[:25]))
    return len(manifest)
def apply(root,baseline,target,patch,git='git'):
    root=Path(root).resolve();check(root,baseline) # all files verified before writes
    if not shutil.which(git):raise ValueError('An existing Git executable is required; nothing is installed')
    patch=Path(patch).resolve()
    # Strict LF patch context, after exact normalized-hash preflight. These are
    # regenerated source files, not the original executable/assets or build objects.
    for entry in baseline:
        p=root/entry['file'];raw=p.read_bytes();data=normalized(raw)
        if data!=raw:p.write_bytes(data)
    with tempfile.TemporaryDirectory(prefix='tcs360-patch-check-') as temporary:
        db=Path(temporary)/'empty.git'
        subprocess.run([git,'init','--bare','-q',str(db)],check=True,capture_output=True)
        command=[git,'-C',str(root),'--git-dir='+str(db),'--work-tree='+str(root),'apply']
        # Explicit work tree prevents an enclosing repository from silently skipping
        # paths. The temporary database has no commits/history or actual branches.
        subprocess.run(command+['--check',str(patch)],check=True,capture_output=True)
        subprocess.run(command+[str(patch)],check=True,capture_output=True)
    return check(root,target)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('check');p.add_argument('stage',choices=['baseline','target']);p.add_argument('directory',type=Path)
    p=sub.add_parser('apply');p.add_argument('directory',type=Path)
    args=parser.parse_args()
    try:
        count=check(args.directory,load(args.stage)) if args.command=='check' else apply(args.directory,load('baseline'),load('target'),BASE/'source/patches/recomp_output_edits.patch')
    except (ValueError,subprocess.CalledProcessError) as error:
        parser.exit(1,str(error)+'\n')
    print(json.dumps({'passed':True,'files':count,'action':args.command,'fresh_generation_or_build':False}))
if __name__=='__main__':main()
