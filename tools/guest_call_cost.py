# Usage: guest_call_cost.py <run dir>... : CPU per guest call from [THREADCPU] windows (one per A/B visit, about 6 s), heavy windows only.
# For builds that change generated code but not the guest's work (log64: inlinable calls). Each window gives the main guest thread's
# CPU per replay (guest_00000000/0) and the guest calls per replay (all threads; the counter runs in every guest prologue, inlined or
# not, so it measures the guest's work, not the build). Reports ns of main-thread CPU per guest call over windows of 100 k calls or more,
# as a median and a 90 % bootstrap interval, and the slope of a fit through the origin.
import re, sys, random
random.seed(1)
# --state S (log92): only windows of A/B state S (exact name), e.g. the base state when another state changes the GPU clock.
args = sys.argv[1:]
state = None
if '--state' in args:
    i = args.index('--state'); state = args[i + 1]; del args[i:i + 2]
for d in args:
    pts = []
    for l in open(d + '/stdout.log', errors='replace'):
        if '[THREADCPU]' not in l: continue
        if state is not None and f'state {state}:' not in l: continue
        m = re.search(r'guest_00000000/0 ([\d.]+)', l); k = re.search(r'guest calls per replay (\d+) k', l)
        if m and k and int(k[1]) >= 100: pts.append((int(k[1]) * 1000, float(m[1]) * 1e6))
    if not pts: print(d, 'no heavy windows'); continue
    ns = sorted(c / n for n, c in pts)
    boot = sorted(sorted(random.choices(ns, k=len(ns)))[len(ns) // 2] for _ in range(2000))
    slope = sum(n * c for n, c in pts) / sum(n * n for n, c in pts)
    print(f'{d}: {len(pts)} heavy windows, main-thread ns per guest call median {ns[len(ns)//2]:.0f} (90 % {boot[100]:.0f} to {boot[1899]:.0f}), '
          f'fit {slope:.0f}, calls per replay {min(p[0] for p in pts)//1000}-{max(p[0] for p in pts)//1000} k')
