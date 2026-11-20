// mermaid's package and its themes object, for the tools that generate from them.
// The default mermaid is the one mermaid-cli (mmdc on PATH) ships.
import { execSync } from 'node:child_process';
import { readdirSync, readFileSync, realpathSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

export function mermaidDir(arg) {
  if (arg) return arg;
  const cli = realpathSync(execSync('command -v mmdc', { shell: '/bin/sh' }).toString().trim());
  for (let d = path.dirname(cli); d !== '/'; d = path.dirname(d))
    for (const p of [path.join(d, 'node_modules', 'mermaid'), path.join(d, 'mermaid')])
      try { if (JSON.parse(readFileSync(path.join(p, 'package.json'), 'utf8')).name === 'mermaid') return p; } catch {}
  throw new Error('no mermaid package above ' + cli);
}

// { default, base, dark, forest, neutral }, each with getThemeVariables(overrides)
export async function loadThemes(dir) {
  const chunks = path.join(dir, 'dist/chunks/mermaid.core');
  for (const f of readdirSync(chunks).filter((f) => f.endsWith('.mjs'))) {
    if (!readFileSync(path.join(chunks, f), 'utf8').includes('getThemeVariables')) continue;
    const m = await import(url.pathToFileURL(path.join(chunks, f)).href);
    const themes = Object.values(m).find((v) => v && ['base', 'dark', 'forest', 'neutral', 'default'].every((t) => v[t]?.getThemeVariables));
    if (themes) return themes;
  }
  throw new Error('no themes object in ' + chunks);
}

// a theme object flattened as mtheme_get answers: nested objects' fields dotted, undefined and
// null left out; conv(v) gives each value (mtheme_get's String(v) by default)
export function flat(o, pre = '', out = {}, conv = String) {
  for (const [k, v] of Object.entries(o)) {
    if (v === undefined || v === null || typeof v === 'function') continue;
    if (typeof v === 'object' && !Array.isArray(v)) flat(v, pre + k + '.', out, conv);
    else out[pre + k] = conv(v);
  }
  return out;
}
