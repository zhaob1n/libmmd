#!/usr/bin/env python3
"""Side-by-side (ours | mermaid-cli) PNGs for tests/docs files -> /tmp/dsbs/<doc>_<NN>.png (parallel).
usage: docs_sbs.py tests/docs/x/01.mmd ...   (or a doc name: docs_sbs.py block)"""
import subprocess, sys, os, re, glob, concurrent.futures as cf
from PIL import Image
os.makedirs('/tmp/dsbs', exist_ok=True)
def one(src):
    tag = src.replace('/', '_')[11:-4]; pid = os.getpid()
    cs, cp, mp = f'/tmp/dsbs/c_{tag}_{pid}.svg', f'/tmp/dsbs/c_{tag}_{pid}.png', f'/tmp/dsbs/m_{tag}.png'
    try:
        subprocess.run(['build/mmdc-c', src, '-o', cs], check=True, capture_output=True)
        w, h = [int(float(x)) + 2 for x in re.search(r'width="([\d.]+)" height="([\d.]+)"', open(cs).read(600)).groups()]
        subprocess.run(['google-chrome', '--headless', '--no-sandbox', '--disable-gpu', '--hide-scrollbars', f'--window-size={min(w,4000)},{min(h,4000)}', '--screenshot=' + cp, 'file://' + cs], check=True, timeout=60, capture_output=True)
        if not os.path.exists(mp):
            subprocess.run([os.path.expanduser('~/node_modules/.bin/mmdc'), '-p', os.path.expanduser('~/pptr.json'), '-i', src, '-o', mp, '-b', 'white'], check=True, timeout=90, capture_output=True)
        a, b = Image.open(cp).convert('RGB'), Image.open(mp).convert('RGB')
        im = Image.new('RGB', (a.width + b.width + 20, max(a.height, b.height)), 'red'); im.paste(a, (0, 0)); im.paste(b, (a.width + 20, 0))
        out = f'/tmp/dsbs/{tag}_{pid}.png'; im.save(out); os.remove(cp); return out
    except Exception as e: return f'ERR {src} {e}'
args = [f for a in sys.argv[1:] for f in (sorted(glob.glob(f'tests/docs/{a}/*.mmd')) if not a.endswith('.mmd') else [a])]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(one, args): print(r)
