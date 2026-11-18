"""Host check of lsw_spirv_glsl.cc's remove_single_pass_main_loop (log118's LSWTCS_GLLOOPEND case included).

Builds the function, cut out of source/switch/source/lsw_spirv_glsl.cc, with a host clang++ and runs it on synthetic GLSL:
  - the loop's only break is its last statement: the loop goes;
  - the last if block ends with its own break before the final one (Xenia's exece with forward jumps): with
    LSWTCS_GLLOOPEND=1 the inner break and the loop go, without it nothing changes;
  - the last block is a loop or a switch ending with break: its break stays (it means something else), and the main loop
    goes as in the plain case, since that break is not the main loop's;
  - another break of the main loop in the body: nothing changes, with the setting or without.
With --glprogs DIR it also runs every saved program source there (sdmc:/lswtcs/glprogs2 of a session, local only) and
reports how many main loops the setting removes; each result must differ from its input only by the removed lines.
Usage: python tests/glsl_loop_check.py [--clang path] [--glprogs DIR]
"""
import argparse, glob, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'source/switch/source/lsw_spirv_glsl.cc'

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
FUNCTION
int main(int argc, char** argv) {
  for (int i = 1; i < argc; i += 2) {
    std::ifstream in(argv[i], std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    remove_single_pass_main_loop(s);
    std::ofstream(argv[i + 1], std::ios::binary) << s;
  }
  return 0;
}
'''

HEAD = '#version 430\nvoid main()\n{\n    int xe_var_skip = 0;\n    SPIRV_CROSS_LOOP\n    for (;;)\n    {\n'
FOOT = '    }\n    o = v;\n}\n'


def case(body):
    return HEAD + body + FOOT


CASES = {
    'plain': (case('        v = 1.0;\n        break;\n'), {'0': 'loop gone', '1': 'loop gone'}),
    'exece': (case('        if (xe_var_skip == 0)\n        {\n            v = 2.0;\n            break;\n        }\n        break;\n'),
              {'0': 'kept', '1': 'loop gone'}),
    'exece_else': (case('        if (xe_var_skip == 0)\n        {\n            v = 2.0;\n        }\n        else\n        {\n'
                        '            v = 3.0;\n            break;\n        }\n        break;\n'), {'0': 'kept', '1': 'loop gone'}),
    'inner_loop': (case('        for (int i = 0; i < 4; i++)\n        {\n            v += 1.0;\n            break;\n        }\n'
                        '        break;\n'), {'0': 'loop gone', '1': 'loop gone'}),
    'inner_switch': (case('        switch (xe_var_skip)\n        {\n            case 0:\n            v = 1.0;\n            break;\n'
                          '        }\n        break;\n'), {'0': 'loop gone', '1': 'loop gone'}),
    'other_break': (case('        if (v > 0.0)\n        {\n            break;\n        }\n        if (xe_var_skip == 0)\n        {\n'
                         '            v = 2.0;\n            break;\n        }\n        break;\n'), {'0': 'kept', '1': 'kept'}),
}


def build(clang, tmp):
    src = SRC.read_text(encoding='utf-8')
    m = re.search(r'static void remove_single_pass_main_loop\(std::string& glsl\) \{.*?\n\}\n', src, re.S)
    if not m:
        sys.exit('remove_single_pass_main_loop not found')
    cpp = Path(tmp) / 'loop_check.cpp'
    cpp.write_text(HARNESS.replace('FUNCTION', m.group(0)), encoding='utf-8')
    exe = Path(tmp) / 'loop_check.exe'
    r = subprocess.run([clang, '-std=c++17', '-O1', str(cpp), '-o', str(exe)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stdout + r.stderr)
    return exe


def run(exe, texts, setting, tmp):
    args, outs = [], []
    for k, t in enumerate(texts):
        i, o = Path(tmp) / f'in{k}.glsl', Path(tmp) / f'out{k}.glsl'
        i.write_bytes(t.encode('latin1'))
        args += [str(i), str(o)]
        outs.append(o)
    env = dict(os.environ, LSWTCS_GLLOOPEND=setting)
    for start in range(0, len(args), 200):   # command line length
        subprocess.run([str(exe)] + args[start:start + 200], check=True, env=env)
    return [o.read_bytes().decode('latin1') for o in outs]


def has_loop(t):
    return '\n    SPIRV_CROSS_LOOP\n    for (;;)\n' in t


def only_removed(a, b):
    """b is a with whole lines removed or the loop head turned into a plain block."""
    la, lb = a.split('\n'), b.split('\n')
    j = 0
    for line in la:
        if j < len(lb) and line == lb[j]:
            j += 1
        elif line in ('    SPIRV_CROSS_LOOP', '            break;', '        break;') or line == '    for (;;)':
            continue
        else:
            return False
    return j == len(lb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--clang', default=os.environ.get('CLANGXX', r'C:\Program Files\LLVM\bin\clang++.exe'))
    ap.add_argument('--glprogs')
    args = ap.parse_args()
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(args.clang, tmp)
        for setting in ('0', '1'):
            outs = run(exe, [c[0] for c in CASES.values()], setting, tmp)
            for (name, (text, want)), out in zip(CASES.items(), outs):
                got = 'kept' if has_loop(out) else 'loop gone'
                good = got == want[setting] and (got == 'kept' and out == text or got == 'loop gone' and only_removed(text, out))
                if name.startswith('inner_'):   # the inner loop's / switch's own break must stay
                    good &= out.count('            break;') == text.count('            break;')
                ok &= good
                print(f"{'ok  ' if good else 'FAIL'} LSWTCS_GLLOOPEND={setting} {name}: {got}")
        if args.glprogs:
            texts = []
            for f in glob.glob(os.path.join(args.glprogs, '*.glsl3')):
                s = open(f, 'rb').read().decode('latin1')
                idx = [m.start() for m in re.finditer('#version', s)]
                for k, i in enumerate(idx):
                    t = s[i: idx[k + 1] if k + 1 < len(idx) else len(s)]
                    t = t.split('\0', 1)[0]
                    if t not in texts:
                        texts.append(t)
            before = sum(map(has_loop, texts))
            for setting in ('0', '1'):
                outs = run(exe, texts, setting, tmp)
                changed = [(a, b) for a, b in zip(texts, outs) if a != b]
                bad = [1 for a, b in changed if not only_removed(a, b)]
                ok &= not bad and (setting == '1' or not changed)
                print(f"glprogs, LSWTCS_GLLOOPEND={setting}: {len(texts)} sources, {before} with a main loop, "
                      f"{len(changed)} changed ({sum(1 for a, b in changed if not has_loop(b))} lose the loop), "
                      f"{len(bad)} changed in another way")
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
