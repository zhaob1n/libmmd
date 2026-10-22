#!/usr/bin/env python3
"""Rank docs examples by rendered-size mismatch vs cached mermaid-cli PNGs (run docs_sbs.py first)."""
import glob, math, os, re, subprocess, sys
from PIL import Image
rows = []
for src in sorted(glob.glob('tests/docs/**/*.mmd', recursive=True)):
    tag = os.path.relpath(src, 'tests/docs')[:-4].replace('/', '_')
    mp = f'/tmp/dsbs/m_{tag}.png'
    if not os.path.exists(mp): continue
    r = subprocess.run(['build/mmdc-c', src, '-o', '/tmp/rank.svg'], capture_output=True)
    if r.returncode: rows.append((99, tag, 'ERR')); continue
    m = re.search(r'viewBox="([-\d.e ]+)"', open('/tmp/rank.svg').read())
    if not m: continue
    _, _, w, h = map(float, m.group(1).split())
    mw, mh = Image.open(mp).size
    d = abs(math.log((w / max(h, 1)) / (mw / mh)))  # aspect only: mermaid-cli PNGs cap width at 784
    rows.append((d, tag, f'ours {w:.0f}x{h:.0f} mermaid {mw}x{mh}'))
rows.sort(reverse=True)
for d, t, s in rows[:int(sys.argv[1]) if len(sys.argv) > 1 else 40]: print(f'{d:6.2f} {t:28} {s}')
print(f'n={len(rows)} mean={sum(r[0] for r in rows)/len(rows):.3f}')
