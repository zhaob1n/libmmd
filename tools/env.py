"""Shared by the side-by-side and audit scripts: run from the repo root wherever they are started,
and find external tools without assuming one machine's layout.
  MMDC            mermaid-cli (default: mmdc on PATH, else node_modules/.bin/mmdc in the repo or $HOME)
  MMDC_PUPPETEER  puppeteer config passed as `mmdc -p` when set (e.g. {"args": ["--no-sandbox"]} as root)
  CHROME          headless Chrome (default: google-chrome, chromium or chromium-browser on PATH; mermaid_render
                  falls back to puppeteer's own when none is found)
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

# mmdc starts a browser per file (~1 s each); this renders a list in one page with the mermaid,
# zenuml plugin and puppeteer mermaid-cli resolves: beside it in a project's node_modules, or nested
# in its own (a global install)
_BATCH = r'''
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import { createRequire } from 'node:module';
const { cli, exe, opts, files } = JSON.parse(fs.readFileSync(0, 'utf8'));
const req = createRequire(path.join(cli, 'package.json'));
const dir = (pkg) => { try { return path.dirname(req.resolve(pkg + '/package.json')); } catch { return null; } };
const puppeteer = req('puppeteer');
const browser = await puppeteer.launch({ headless: true, ...(exe ? { executablePath: exe } : {}), ...opts });
const page = await browser.newPage();
await page.goto(url.pathToFileURL(path.join(cli, 'dist/index.html')).href); // mmdc's page
await page.addScriptTag({ path: path.join(dir('mermaid'), 'dist/mermaid.min.js') });
const zen = dir('@mermaid-js/mermaid-zenuml'); // as mmdc registers it
if (zen) await page.addScriptTag({ path: path.join(zen, 'dist/mermaid-zenuml.min.js') });
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
    cli = os.path.realpath(mmdc()[0])
    while not os.path.exists(os.path.join(cli, 'dist/index.html')) and cli != '/': cli = os.path.dirname(cli)
    if cli == '/': sys.exit(f'no mermaid-cli package above {mmdc()[0]}')
    pp = os.environ.get('MMDC_PUPPETEER')
    exe = os.environ.get('CHROME') or next(filter(None, map(shutil.which, ['google-chrome', 'chromium', 'chromium-browser'])), None)
    job = json.dumps({'cli': cli, 'exe': exe, 'opts': json.load(open(pp)) if pp else {}, 'files': list(files)})
    r = subprocess.run(['node', '--input-type=module', '-e', _BATCH], input=job, capture_output=True, text=True)
    if r.returncode: sys.exit('mermaid render failed: ' + r.stderr[-2000:])
    return {o['file']: o for o in map(json.loads, r.stdout.splitlines())}

def chrome(): return _find('CHROME', ['google-chrome', 'chromium', 'chromium-browser'])

def scratch(name):
    """a per-user scratch dir that survives the run (caches, outputs worth looking at)"""
    d = os.path.join(tempfile.gettempdir(), f'mmd-{os.getuid()}', name)
    os.makedirs(d, exist_ok=True)
    return d
