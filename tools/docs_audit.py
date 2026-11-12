#!/usr/bin/env python3
"""Render every tests/docs/*/*.mmd with ours (and, with --mmdc, mermaid-cli) and print per-doc pass rates.
Usage: docs_audit.py [-v] [-q] [--mmdc] [mmd binary]; exits 1 if any mermaid-ok example fails.
Mermaid results are cached in tests/docs/mermaid_status.txt (path ok|fail) so runs without --mmdc are fast."""
import os, sys, glob, subprocess, concurrent.futures as cf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import env
sys.argv[1:] = env.at_root(sys.argv[1:])
files = sorted(glob.glob('tests/docs/*/*.mmd'))
cache = 'tests/docs/mermaid_status.txt'
exe = next((a for a in sys.argv[1:] if not a.startswith('-')), 'build/mmd')
ms = dict(l.split() for l in open(cache)) if os.path.exists(cache) else {}
def ours(f):
    r = subprocess.run([exe, f, '-o', '/dev/null'], capture_output=True, timeout=20)
    return 'ok' if r.returncode == 0 else 'fail:' + r.stderr.decode(errors='replace').strip().split('\n')[0][:120]
if '--mmdc' in sys.argv:
    ms = {f: 'ok' if r['ok'] and 'Syntax error' not in r['svg'] else 'fail' for f, r in env.mermaid_render(files).items()}
    open(cache, 'w').write(''.join(f'{k} {v}\n' for k, v in sorted(ms.items())))
with cf.ThreadPoolExecutor(8) as ex: us = dict(zip(files, ex.map(ours, files)))
by = {}
for f in files:
    d = f.split('/')[2]; m = ms.get(f, '?'); u = us[f]
    s = by.setdefault(d, [0, 0, 0])
    if m == 'ok': s[0] += 1; s[1] += u == 'ok'
    else: s[2] += 1
    if m == 'ok' and u != 'ok' and '-v' in sys.argv: print('FAIL', f, u)
tm = tu = 0
for d, (m, u, x) in sorted(by.items()):
    tm += m; tu += u
    if '-q' not in sys.argv: print(f'{d:28} {u:3}/{m:<3} ours/mermaid-ok  ({x} mermaid-fail)')
print(f'{"TOTAL":28} {tu:3}/{tm:<3} = {100 * tu / max(tm, 1):.1f}%')
sys.exit(0 if tu == tm else 1)
