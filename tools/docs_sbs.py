#!/usr/bin/env python3
"""Side-by-side (ours | mermaid-cli) PNGs for tests/docs files -> $TMPDIR/mmd-$UID/dsbs/<doc>_<NN>.png (parallel; see tools/env.py).
usage: docs_sbs.py tests/docs/x/01.mmd ...   (or a doc name: docs_sbs.py block)"""
import subprocess, sys, os, re, glob, concurrent.futures as cf
import atexit as _ax, shutil as _sh, tempfile as _tf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import env
sys.argv[1:] = env.at_root(sys.argv[1:])
D, MMDC, CHROME = env.scratch('dsbs'), env.mmdc(), env.chrome()
# chrome/puppeteer leave scoped_dir*/profile dirs in TMPDIR: confine them to one dir removed at exit
if 'MMD_TMP' not in os.environ:
    os.environ['MMD_TMP'] = os.environ['TMPDIR'] = _tf.mkdtemp(dir=env.scratch('mmdc'))
    _tf.tempdir = None
    _ax.register(_sh.rmtree, os.environ['MMD_TMP'], True)
from PIL import Image
def one(src):
    rel = os.path.relpath(os.path.realpath(src), os.path.realpath('tests/docs'))  # absolute paths must map to the same tag
    tag = (rel if not rel.startswith('..') else os.path.relpath(os.path.realpath(src)).replace('tests/cases/', ''))[:-4].replace('/', '_'); pid = os.getpid()
    cs, cp, mp = f'{D}/c_{tag}_{pid}.svg', f'{D}/c_{tag}_{pid}.png', f'{D}/m_{tag}.png'
    try:
        subprocess.run(['build/mmd', src, '-o', cs], check=True, capture_output=True)
        w, h = [int(float(x)) + 2 for x in re.search(r'width="([\d.]+)" height="([\d.]+)"', open(cs).read(600)).groups()]
        subprocess.run([CHROME, '--headless', '--no-sandbox', '--disable-gpu', '--hide-scrollbars', f'--window-size={min(w,4000)},{min(h,4000)}', '--screenshot=' + cp, 'file://' + cs], check=True, timeout=60, capture_output=True)
        if not os.path.exists(mp) or os.path.getmtime(mp) < os.path.getmtime(src):
            subprocess.run([*MMDC, '-i', src, '-o', mp, '-b', 'white'], check=True, timeout=90, capture_output=True)
        a, b = Image.open(cp).convert('RGB'), Image.open(mp).convert('RGB')
        im = Image.new('RGB', (a.width + b.width + 20, max(a.height, b.height)), 'red'); im.paste(a, (0, 0)); im.paste(b, (a.width + 20, 0))
        out = f'{D}/{tag}_{pid}.png'; im.save(out); os.remove(cp); return out
    except Exception as e: return f'ERR {src} {e}'
args = [f for a in sys.argv[1:] for f in (sorted(glob.glob(f'tests/docs/{a}/*.mmd')) if not a.endswith('.mmd') else [a])]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(one, args): print(r)
