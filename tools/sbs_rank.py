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
    ms = mp[:-4] + '.svg'  # exact mermaid viewBox when cached: PNGs are scaled/cropped to 784px wide
    if mw == 784 and not os.path.exists(ms) and os.environ.get('SBS_FETCH'):
        subprocess.run([os.path.expanduser('~/node_modules/.bin/mmdc'), '-p', os.path.expanduser('~/pptr.json'),
                        '-i', src, '-o', ms], capture_output=True)
    if os.path.exists(ms):
        mv = re.search(r'viewBox="([-\d.e ]+)"', open(ms).read())
        if mv: mw, mh = (round(float(x)) for x in mv.group(1).split()[2:])
    elif mw == 784: rows.append((None, tag, 'crop (set SBS_FETCH=1)')); continue
    d = abs(math.log((w / max(h, 1)) / (mw / mh)))  # aspect only: mermaid-cli PNGs cap width at 784
    rows.append((d, tag, f'ours {w:.0f}x{h:.0f} mermaid {mw}x{mh}'))
crop = [r for r in rows if r[0] is None]
rows = [r for r in rows if r[0] is not None]
rows.sort(reverse=True)
for d, t, s in rows[:int(sys.argv[1]) if len(sys.argv) > 1 else 40]: print(f'{d:6.2f} {t:28} {s}')
if crop: print(f'skipped {len(crop)} 784px-cropped refs without cached svg')
print(f'n={len(rows)} mean={sum(r[0] for r in rows)/len(rows):.3f}')
