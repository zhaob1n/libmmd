// Cases for tests/d3fmt_test.c from the real d3-format: spec \t value (hex bits) \t d3.format(spec)(value),
// "!" where d3 rejects the spec. Usage: node tools/d3fmt_cases.mjs [d3-format dir] > tests/d3fmt_cases.tsv
// The default d3-format is the one mermaid-cli (mmdc on PATH) ships, which mermaid's treemap uses.
import { execSync } from 'node:child_process';
import { realpathSync } from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const dir = process.argv[2] ?? (() => {
  const cli = realpathSync(execSync('command -v mmdc', { shell: '/bin/sh' }).toString().trim());
  for (let d = path.dirname(cli); d !== '/'; d = path.dirname(d)) {
    const p = path.join(d, 'node_modules', 'd3-format');
    try { return realpathSync(p); } catch {}
  }
  throw new Error('no d3-format above ' + cli);
})();
const { format } = await import(url.pathToFileURL(path.join(dir, 'src/index.js')).href);

let seed = 12345; // deterministic: the file is checked in
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) % 2147483648) % n);
const pick = (a) => a[rnd(a.length)];

const specs = [',', '', '$0,0', ',.2f', '.1%', '$.1%', '.2s', '.3~s', 'd', ',d', '.0f', '.2e', '.3g', '.3r', '.2p',
  '+,', '(,.2f', ' .2f', '$,.2f', '#x', '#b', '#o', 'X', 'c', 'n', '.4n', '010,.2f', '>10', '<10', '^10', '*^12,.1f',
  '=+10.2f', '0=8', 'é>9', '~f', '~e', '.0e', '.21g', '.20f', '.25f', 'F', 'z', '.f', '1.', '>>>', '<<', ',,', '08,d',
  '012,.3e', '0^10', '-$,.2f', '+$', '$010,.2f', '.1~%', '~s', '.5~r', '20,', '(012,'];
const fills = ['', '', '', ' ', '*', '0', 'é', '∑', '>'];
const curated = specs.length;
for (let i = 0; i < 300; i++) {
  const fa = rnd(3) ? '' : pick(fills) + pick(['<', '>', '=', '^']);
  const s = fa + pick(['', '', '+', '-', '(', ' ']) + pick(['', '', '$', '#']) + pick(['', '', '0']) +
    pick(['', '', '', '5', '12', '1']) + pick(['', ',']) + pick(['', '', '.0', '.1', '.2', '.3', '.6', '.12', '.22']) +
    pick(['', '', '~']) + pick(['', 'd', 'e', 'f', 'g', 'r', 's', '%', 'p', 'b', 'o', 'x', 'X', 'c', 'n', 'q', 'E']);
  specs.push(s);
}
const values = [0, -0, 1, -1, 0.5, 1.5, 2.5, -2.5, 0.125, 0.375, 12.5, 1234567, 1234.5678, -1234.5678, 0.000123456,
  1e-7, 9.995, 0.0005, 999999.5, 1e21, 1.5e21, 2 ** 70, 1e300, 1.7e308, 5e-324, 1e-30, 0.35, 0.15, 99.95, 1 / 3, 2 / 3,
  -0.0004, 123456789.987, 1e15, 4.35, 8.5, 0.045, Infinity, -Infinity, NaN, 255, -255, 1e6, 314159.26535];
for (let i = 0; i < 40; i++) values.push((rnd(2000001) - 1000000) / 10 ** rnd(8) * 10 ** (rnd(5) * 3 - 3));

const hex = (v) => { const b = new DataView(new ArrayBuffer(8)); b.setFloat64(0, v); return b.getBigUint64(0).toString(16); };
const out = [];
for (const [i, s] of specs.entries()) {
  let f = null;
  try { f = format(s); } catch {}
  if (!f) { out.push(`${s}\t0\t!`); continue; }
  const keep = s === ',' || s === '' ? () => true : i < curated ? () => rnd(3) === 0 : () => rnd(14) === 0;
  for (const v of values.filter(keep)) out.push(`${s}\t${hex(v)}\t${f(v)}`);
}
console.log(out.join('\n'));
