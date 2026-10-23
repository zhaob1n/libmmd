#!/usr/bin/env python3
"""Side-by-side gallery for the README: libmmd (left) vs mermaid-cli (right).
usage: TMPDIR=... python3 tools/gallery.py case.mmd...   -> docs/gallery/<case>.png
Needs ~/node_modules/.bin/mmdc, google-chrome, Pillow."""
import os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont
MMDC = os.path.expanduser('~/node_modules/.bin/mmdc'); PP = os.path.expanduser('~/pptr.json')
T = tempfile.mkdtemp(prefix='mmd-gal.')
try: F = ImageFont.truetype('DejaVuSans-Bold.ttf', 18)
except OSError: F = ImageFont.load_default()
def fit(im, w, h):
    s = min(w / im.width, h / im.height, 1.0)
    return im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS) if s < 1 else im
for src in sys.argv[1:]:
    n = os.path.basename(src)[:-4]; a, b = f'{T}/{n}_c.png', f'{T}/{n}_m.png'
    subprocess.run(['tools/render_png.sh', src, a], check=True, capture_output=True)
    subprocess.run([MMDC, '-p', PP, '-i', src, '-o', b, '-b', 'white'], check=True, timeout=120, capture_output=True)
    A, B = (fit(Image.open(p).convert('RGB'), 700, 640) for p in (a, b))
    W, H = A.width + B.width + 30, max(A.height, B.height) + 40
    o = Image.new('RGB', (W, H), 'white'); d = ImageDraw.Draw(o)
    d.text((10, 8), 'libmmd', fill='#1f6feb', font=F); d.text((A.width + 20, 8), 'mermaid-cli', fill='#cf222e', font=F)
    o.paste(A, (10, 36)); o.paste(B, (A.width + 20, 36)); d.line([(A.width + 15, 36), (A.width + 15, H - 4)], fill='#ccc')
    o.quantize(128, method=Image.Quantize.MEDIANCUT).save(f'docs/gallery/{n}.png', optimize=True)
    for p in (a, b): os.remove(p)
    print(n)
os.rmdir(T)
