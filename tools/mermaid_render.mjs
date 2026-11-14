// Renders Mermaid sources with mermaid.js itself, all in one page of one headless browser (mermaid-cli
// starts a browser per file, about a second each), with the mermaid, zenuml plugin and puppeteer that
// mermaid-cli resolves: beside it in a project's node_modules, or nested in its own (a global install).
//
//   node tools/mermaid_render.mjs CONFIG [CSS] < jobs    CONFIG: JSON for mermaid.initialize()
//                                                        jobs: lines of SOURCE \t OUT.svg \t OUT.err
// Each job leaves one of its two files: the SVG, serialised as XML under the id mermaid-cli uses, or
// the reason mermaid refused the source. The SVGs name fonts and do not carry them: CSS, when given,
// becomes a stylesheet that imports what mermaid-cli loads into its page, for a page that shows them.
// Exit status 3: mermaid-cli is not installed.
//
//   MMDC            mermaid-cli (default: mmdc on PATH, else node_modules/.bin/mmdc in the repo or $HOME)
//   MMDC_PUPPETEER  puppeteer config file, as `mmdc -p` takes it (e.g. {"args": ["--no-sandbox"]} as root)
//   CHROME          the browser (default: google-chrome, chromium or chromium-browser on PATH, else puppeteer's own)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import url from 'node:url';
import { createRequire } from 'node:module';

const which = (...names) => {
  for (const name of names)
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      const p = path.join(dir, name);
      try { fs.accessSync(p, fs.constants.X_OK); return p; } catch {}
    }
};
const root = path.dirname(path.dirname(url.fileURLToPath(import.meta.url)));
const mmdc = process.env.MMDC || which('mmdc') ||
  [root, os.homedir()].map(d => path.join(d, 'node_modules/.bin/mmdc')).find(p => fs.existsSync(p));
if (!mmdc) { console.error('mermaid-cli not found: put mmdc on PATH or set MMDC'); process.exit(3); }
let cli = fs.realpathSync(mmdc);
while (!fs.existsSync(path.join(cli, 'dist/index.html')) && cli !== '/') cli = path.dirname(cli);
if (cli === '/') { console.error(`no mermaid-cli package above ${mmdc}`); process.exit(1); }

const req = createRequire(path.join(cli, 'package.json'));
const dir = (pkg) => { try { return path.dirname(req.resolve(pkg + '/package.json')); } catch { return null; } };
// What mmdc puts in its page before it renders. Without the fonts mermaid measures every label in a
// fallback font and sizes the nodes for it; without Font Awesome fa: icons are missing.
const fileUrl = (spec) => { try { return url.pathToFileURL(req.resolve(spec)).href; } catch { return null; } };
const css = ['@fortawesome/fontawesome-free/css/brands.css', '@fortawesome/fontawesome-free/css/regular.css',
  '@fortawesome/fontawesome-free/css/solid.css', '@fortawesome/fontawesome-free/css/fontawesome.css',
  '@fontsource-variable/recursive/index.css', '@fontsource/open-sans/400.css', '@fontsource/open-sans/400-italic.css',
  'katex/dist/katex.css'].map(fileUrl).filter(Boolean);
if (process.argv[3]) fs.writeFileSync(process.argv[3], css.map(href => `@import url("${href}");\n`).join(''));
const jobs = fs.readFileSync(0, 'utf8').split('\n').filter(Boolean);
if (!jobs.length) process.exit(0);

const opts = process.env.MMDC_PUPPETEER ? JSON.parse(fs.readFileSync(process.env.MMDC_PUPPETEER, 'utf8')) : {};
const exe = process.env.CHROME || which('google-chrome', 'chromium', 'chromium-browser');
// the page is a file: fonts and modules it loads from other files count as cross-origin
const args = ['--allow-file-access-from-files', ...(opts.args || [])];
const browser = await req('puppeteer').launch({ headless: true, ...(exe ? { executablePath: exe } : {}), ...opts, args });
const page = await browser.newPage();
await page.goto(url.pathToFileURL(path.join(cli, 'dist/index.html')).href); // mmdc's page
await page.addScriptTag({ path: path.join(dir('mermaid'), 'dist/mermaid.min.js') });
const zen = dir('@mermaid-js/mermaid-zenuml'); // as mmdc registers it
if (zen) await page.addScriptTag({ path: path.join(zen, 'dist/mermaid-zenuml.min.js') });
const tidy = dir('@mermaid-js/layout-tidy-tree'); // optional there too
await page.evaluate(async (config, css, tidyTree) => {
  await Promise.all(css.map(href => new Promise((loaded, failed) => {
    const link = Object.assign(document.createElement('link'), { rel: 'stylesheet', href, onload: loaded });
    link.onerror = () => failed(new Error(`Failed to load stylesheet: ${href}`));
    document.head.appendChild(link);
  })));
  await Promise.all(Array.from(document.fonts, font => font.load()));
  if (globalThis['mermaid-zenuml']) await mermaid.registerExternalDiagrams([globalThis['mermaid-zenuml']]);
  if (tidyTree) mermaid.registerLayoutLoaders((await import(tidyTree)).default);
  mermaid.initialize({ startOnLoad: false, ...config });
}, JSON.parse(process.argv[2] || '{}'), css,
  tidy && url.pathToFileURL(path.join(tidy, 'dist/mermaid-layout-tidy-tree.esm.min.mjs')).href);

for (const job of jobs) {
  const [src, svg, err] = job.split('\t');
  const r = await page.evaluate(async (text) => {
    try {
      await mermaid.parse(text);
      const box = document.createElement('div');
      box.innerHTML = (await mermaid.render('my-svg', text)).svg;
      // a diagram mermaid draws as its error picture did not render
      if (box.textContent.includes('Syntax error in text')) return { error: 'Syntax error in text' };
      return { svg: new XMLSerializer().serializeToString(box.firstElementChild) };
    } catch (e) {
      document.querySelectorAll('[id^="dmy-svg"]').forEach(n => n.remove()); // the error svg left behind
      return { error: String(e && e.message || e).replace(/\s+/g, ' ').slice(0, 300) };
    }
  }, fs.readFileSync(src, 'utf8'));
  fs.rmSync(r.svg === undefined ? svg : err, { force: true });
  fs.writeFileSync(r.svg === undefined ? err : svg, r.svg ?? r.error + '\n');
}
await browser.close();
