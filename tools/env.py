"""Shared by the side-by-side and audit scripts: run from the repo root wherever they are started,
and find external tools without assuming one machine's layout.
  MMDC            mermaid-cli (default: mmdc on PATH, else node_modules/.bin/mmdc in the repo or $HOME)
  MMDC_PUPPETEER  puppeteer config passed as `mmdc -p` when set (e.g. {"args": ["--no-sandbox"]} as root)
  CHROME          headless Chrome (default: google-chrome, chromium or chromium-browser on PATH)
Scratch files go under tempfile.gettempdir(), so TMPDIR moves them."""
import os, shutil, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def at_root(args):
    """make existing path arguments absolute, then chdir to the repo root"""
    args = [os.path.abspath(a) if os.path.exists(a) else a for a in args]
    os.chdir(ROOT)
    return args

def _find(var, names, extra=()):
    if os.environ.get(var): return os.environ[var]
    for p in [shutil.which(n) for n in names] + [p for p in extra if os.access(p, os.X_OK)]:
        if p: return p
    sys.exit(f'{names[0]} not found: put it on PATH or set {var}')

def mmdc():
    exe = _find('MMDC', ['mmdc'], [os.path.join(d, 'node_modules/.bin/mmdc') for d in (ROOT, os.path.expanduser('~'))])
    pp = os.environ.get('MMDC_PUPPETEER')
    return [exe, '-p', pp] if pp else [exe]

def chrome(): return _find('CHROME', ['google-chrome', 'chromium', 'chromium-browser'])

def scratch(name):
    """a per-user scratch dir that survives the run (caches, outputs worth looking at)"""
    d = os.path.join(tempfile.gettempdir(), f'mmd-{os.getuid()}', name)
    os.makedirs(d, exist_ok=True)
    return d
