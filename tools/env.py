"""Shared by the side-by-side and audit scripts: run from the repo root wherever they are started,
and find external tools without assuming one machine's layout.
  MMDC            mermaid-cli (default: mmdc on PATH, else node_modules/.bin/mmdc in the repo or $HOME)
  MMDC_PUPPETEER  puppeteer config passed as `mmdc -p` when set (e.g. {"args": ["--no-sandbox"]} as root)
  CHROME          headless Chrome (default: google-chrome, chromium or chromium-browser on PATH)
Scratch files go under tempfile.gettempdir(), so TMPDIR moves them."""
import json, os, shutil, subprocess, sys, tempfile
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

# mmdc starts a browser per file (~10 s each); this renders a list in one page with the mermaid
# mermaid-cli ships, beside it in node_modules
_BATCH = r'''
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import { createRequire } from 'node:module';
const { nm, opts, files } = JSON.parse(fs.readFileSync(0, 'utf8'));
const puppeteer = createRequire(path.join(nm, 'x.js'))('puppeteer');
const browser = await puppeteer.launch({ headless: true, ...opts });
const page = await browser.newPage();
await page.goto(url.pathToFileURL(path.join(nm, '@mermaid-js/mermaid-cli/dist/index.html')).href); // mmdc's page
await page.addScriptTag({ path: path.join(nm, 'mermaid/dist/mermaid.min.js') });
const zen = path.join(nm, '@mermaid-js/mermaid-zenuml/dist/mermaid-zenuml.min.js'); // as mmdc registers it
if (fs.existsSync(zen)) await page.addScriptTag({ path: zen });
await page.evaluate(async () => {
  if (globalThis['mermaid-zenuml']) await mermaid.registerExternalDiagrams([globalThis['mermaid-zenuml']]);
  mermaid.initialize({ startOnLoad: false });
});
let i = 0;
for (const file of files) {
  const r = await page.evaluate(async (text, id) => {
    try {
      await mermaid.parse(text);
      return { ok: true, svg: (await mermaid.render(id, text)).svg };
    } catch (e) {
      document.querySelectorAll('[id^="dm"]').forEach(n => n.remove()); // the error svg left behind
      return { ok: false, error: String(e && e.message || e).slice(0, 300) };
    }
  }, fs.readFileSync(file, 'utf8'), 'm' + i++);
  console.log(JSON.stringify({ file, ...r }));
}
await browser.close();
'''

def mermaid_render(files):
    """render each file with mermaid.js: {file: {'ok', 'svg' or 'error'}}"""
    nm = os.path.realpath(mmdc()[0])
    while os.path.basename(nm) != 'node_modules' and nm != '/': nm = os.path.dirname(nm)
    if nm == '/': sys.exit(f'no node_modules above {mmdc()[0]} (mermaid and puppeteer live there)')
    pp = os.environ.get('MMDC_PUPPETEER')
    job = json.dumps({'nm': nm, 'opts': json.load(open(pp)) if pp else {}, 'files': list(files)})
    r = subprocess.run(['node', '--input-type=module', '-e', _BATCH], input=job, capture_output=True, text=True)
    if r.returncode: sys.exit('mermaid render failed: ' + r.stderr[-2000:])
    return {o['file']: o for o in map(json.loads, r.stdout.splitlines())}

def chrome(): return _find('CHROME', ['google-chrome', 'chromium', 'chromium-browser'])

def scratch(name):
    """a per-user scratch dir that survives the run (caches, outputs worth looking at)"""
    d = os.path.join(tempfile.gettempdir(), f'mmd-{os.getuid()}', name)
    os.makedirs(d, exist_ok=True)
    return d
