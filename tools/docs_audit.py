#!/usr/bin/env python3
"""Render every tests/docs/*/*.mmd with ours (and, with --mmdc, mermaid-cli) and print per-doc pass rates.
Usage: docs_audit.py [-v] [-q] [--mmdc] [mmdc-c binary]; exits 1 if any mermaid-ok example fails.
Mermaid results are cached in tests/docs/mermaid_status.txt (path ok|fail) so runs without --mmdc are fast."""
import os, sys, glob, subprocess, concurrent.futures as cf
files = sorted(glob.glob('tests/docs/*/*.mmd'))
cache = 'tests/docs/mermaid_status.txt'
exe = next((a for a in sys.argv[1:] if not a.startswith('-')), 'build/mmdc-c')
ms = dict(l.split() for l in open(cache)) if os.path.exists(cache) else {}
def ours(f):
    r = subprocess.run([exe, f, '-o', '/dev/null'], capture_output=True, timeout=20)
    return 'ok' if r.returncode == 0 else 'fail:' + r.stderr.decode(errors='replace').strip().split('\n')[0][:120]
def merm(f):
    os.makedirs('/tmp/da', exist_ok=True)
    o = '/tmp/da/' + f.replace('/', '_')[11:-4] + '.svg'
    try:
        r = subprocess.run([os.path.expanduser('~/node_modules/.bin/mmdc'), '-p', os.path.expanduser('~/pptr.json'), '-i', f, '-o', o, '-q'], capture_output=True, timeout=90)
        return 'ok' if r.returncode == 0 and os.path.exists(o) and b'Syntax error' not in open(o, 'rb').read() else 'fail'
    except subprocess.TimeoutExpired: return 'fail'
if '--mmdc' in sys.argv:
    with cf.ThreadPoolExecutor(6) as ex: ms = dict(zip(files, ex.map(merm, files)))
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
