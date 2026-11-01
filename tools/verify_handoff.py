"""Verify the received source/context manifest without installing or fetching anything."""
from pathlib import Path
import argparse,hashlib,json,re,sys

ROOT=Path(__file__).resolve().parents[1]
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(4<<20),b''):h.update(block)
    return h.hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--share-scan',action='store_true')
    args=parser.parse_args();manifest=json.loads((ROOT/'manifest.json').read_text(encoding='utf-8'))
    issues=[];expected={f['path'] for f in manifest['files']}
    for record in manifest['files']:
        p=ROOT/record['path']
        if not p.is_file():issues.append('missing: '+record['path']);continue
        if p.is_symlink() or digest(p)!=record['sha256']:issues.append('changed/link: '+record['path'])
    if args.share_scan:
        binary={'.exe','.dll','.elf','.nro','.nacp','.o','.obj','.gch','.iso','.xex','.dat','.xci','.nsp','.nca','.rgba','.dksh','.keys','.pyc'}
        homes=re.compile(r'(?:[A-Za-z]:[/\\]+Users[/\\]+(?!<)[A-Za-z0-9_.-]+|/home/(?!<)[A-Za-z0-9_.-]+)')
        secrets=re.compile(r'(?:ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----)')
        for p in ROOT.rglob('*'):
            rel=p.relative_to(ROOT).as_posix()
            if '.git' in p.relative_to(ROOT).parts or p.is_symlink():issues.append('Git metadata/link: '+rel)
            if not p.is_file():continue
            if rel not in expected and rel!='manifest.json':issues.append('not in received manifest: '+rel)
            if p.suffix.lower() in binary:issues.append('binary/game/cache artifact: '+rel)
            data=p.read_bytes()
            if b'\0' in data:issues.append('binary content: '+rel);continue
            text=data.decode('utf-8',errors='replace')
            # /home/... is a generic example in upstream build comments.
            if any(re.split(r'[/\\]+',m.group())[-1].strip('.') for m in homes.finditer(text)):
                issues.append('private home path: '+rel)
            if secrets.search(text):issues.append('credential-shaped content: '+rel)
    print(json.dumps({'passed':not issues,'files':len(manifest['files']),'share_scan':args.share_scan,'issues':issues},indent=2))
    return 0 if not issues else 1
if __name__=='__main__':sys.exit(main())
