// Cases for tests/mtheme_test.c from mermaid's own themes: blocks of
//   = <theme> <n>                       then n lines  <key> \t <value>  (the overrides), then
//   lines <key> \t <value> of every variable getThemeVariables(overrides) yields (flattened, dotted
//   for nested objects, String(v)), or the single line "!" where it throws.
// Nested overrides (xyChart.titleColor) are the one place libmmd departs from mermaid on purpose:
// mermaid's calculate() replaces the theme's whole object with the user's, libmmd sets the field.
// No theme derives a field of such an object from another one, so libmmd's answer is mermaid's for
// the flat overrides with the user's fields laid over it.
// To keep the file small, a block lists only the variables that differ from the same theme's first
// block (no overrides), plus "-<key>" for variables that disappear.
// usage: node tools/mtheme_cases.mjs [mermaid package dir] > tests/mtheme_cases.tsv
import { execSync } from 'node:child_process';
import { readdirSync, readFileSync, realpathSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const dir = process.argv[2] ?? (() => {
  const cli = realpathSync(execSync('command -v mmdc', { shell: '/bin/sh' }).toString().trim());
  for (let d = path.dirname(cli); d !== '/'; d = path.dirname(d))
    for (const p of [path.join(d, 'node_modules', 'mermaid'), path.join(d, 'mermaid')])
      try { if (JSON.parse(readFileSync(path.join(p, 'package.json'), 'utf8')).name === 'mermaid') return p; } catch {}
  throw new Error('no mermaid package above ' + cli);
})();
const chunks = path.join(dir, 'dist/chunks/mermaid.core');
let themes;
for (const f of readdirSync(chunks).filter((f) => f.endsWith('.mjs'))) {
  if (!readFileSync(path.join(chunks, f), 'utf8').includes('getThemeVariables')) continue;
  const m = await import(url.pathToFileURL(path.join(chunks, f)).href);
  themes = Object.values(m).find((v) => v && ['base', 'dark', 'forest', 'neutral', 'default'].every((t) => v[t]?.getThemeVariables));
  if (themes) break;
}
if (!themes) throw new Error('no themes object in ' + chunks);

let seed = 4242;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) % 2147483648) % n);
const pick = (a) => a[rnd(a.length)];
const hex2 = () => rnd(256).toString(16).padStart(2, '0');
const colours = () => [
  '#' + hex2() + hex2() + hex2(), '#' + rnd(4096).toString(16).padStart(3, '0'), '#' + hex2() + hex2() + hex2() + hex2(),
  `rgb(${rnd(256)}, ${rnd(256)}, ${rnd(256)})`, `rgba(${rnd(256)},${rnd(256)},${rnd(256)},0.${rnd(10)})`,
  `hsl(${rnd(360)}, ${rnd(101)}%, ${rnd(101)}%)`, `hsla(${rnd(360)},${rnd(101)}%,${rnd(101)}%,0.${rnd(10)})`,
  pick(['red', 'lightgrey', 'navy', 'white', 'black', 'transparent', 'rebeccapurple']),
];
const BASE = ['primaryColor', 'secondaryColor', 'tertiaryColor', 'background', 'mainBkg', 'primaryTextColor', 'lineColor',
  'textColor', 'primaryBorderColor', 'noteBkgColor', 'nodeBorder', 'clusterBkg', 'actorBkg', 'quadrant1Fill', 'cScale0', 'git0',
  'pie1', 'xyChart.titleColor', 'xyChart.plotColorPalette', 'radar.axisColor'];

// flattened, as mtheme_get answers: strings as is, numbers and booleans as String(v), arrays String(arr)
function flat(o, pre, out) {
  for (const [k, v] of Object.entries(o)) {
    if (v === undefined || v === null || typeof v === 'function') continue;
    if (typeof v === 'object' && !Array.isArray(v)) flat(v, pre + k + '.', out);
    else out[pre + k] = String(v);
  }
  return out;
}
// a YAML scalar as mermaid's config types it
const typed = (v) => v === 'true' ? true : v === 'false' ? false : v.trim() !== '' && !Number.isNaN(Number(v)) ? Number(v) : v;
// the flat overrides -> the object mermaid's config would hand over
const object = (ov) => Object.fromEntries(ov.map(([k, v]) => [k, typed(v)]));

const out = [];
for (const theme of ['default', 'base', 'dark', 'forest', 'neutral']) {
  const sets = [[]];
  for (const k of BASE) for (const c of colours()) sets.push([[k, c]]);
  for (const v of ['true', 'false']) sets.push([['darkMode', v]]), sets.push([['darkMode', v], ['primaryColor', '#336699']]);
  sets.push([['fontFamily', 'Georgia, serif']], [['fontSize', '18px']], [['fontSize', '14']], [['nodeBorder', '#123456'], ['useGradient', 'true']]);
  sets.push([['primaryColor', 'bogus']], [['background', 'notacolor']], [['mainBkg', 'true']], [['lineColor', '#12']], [['xyChart.titleColor', 'nope']]);
  for (const c of ['hsl(1e+1, 50%, 50%)', 'hsl(1e-1, 5E1%, 50%)', 'rgb(1e2, 0, 0)', 'rgb(1e-2, 0, 0)', 'HSL(10turn, 50%, 50%)'])
    sets.push([['primaryColor', c]]);
  for (let i = 0; i < 25; i++) {
    const s = new Map();
    for (let j = 0, m = 1 + rnd(4); j < m; j++) s.set(pick(BASE.concat(['darkMode'])), pick(colours().concat(['true', 'false'])));
    sets.push([...s]);
  }
  let ref = null;
  for (const ov of sets) {
    let vars;
    const nested = ov.filter(([k]) => k.includes('.'));
    try { vars = flat(themes[theme].getThemeVariables(object(ov.filter(([k]) => !k.includes('.')))), '', {}); } catch { vars = null; }
    if (vars) for (const [k, v] of nested) vars[k] = String(typed(v));
    out.push(`= ${theme} ${ov.length}`);
    for (const [k, v] of ov) out.push(`${k}\t${v}`);
    if (!vars) { out.push('!'); continue; }
    if (!ref) { ref = vars; for (const [k, v] of Object.entries(vars)) out.push(`${k}\t${v}`); continue; }
    for (const [k, v] of Object.entries(vars)) if (ref[k] !== v) out.push(`${k}\t${v}`);
    for (const k of Object.keys(ref)) if (!(k in vars)) out.push(`-${k}`);
  }
}
console.log(out.join('\n'));
