// Generates src/mtheme_gen.h: mermaid's five classic themes (default, base, dark, forest, neutral)
// as C, from the theme sources mermaid's dist carries in its sourcemaps. Each theme becomes
// <id>_ctor() and <id>_update(), statement for statement; src/mtheme.c holds the runtime they call
// and the hand-written calculate() variants, which this tool checks against the sources.
// usage: node tools/gen_mtheme.mjs [mermaid package dir] > src/mtheme_gen.h
// The default mermaid is the one mermaid-cli (mmdc on PATH) ships.
import { execSync } from 'node:child_process';
import { readdirSync, readFileSync, realpathSync } from 'node:fs';
import path from 'node:path';

const dir = process.argv[2] ?? (() => {
  const cli = realpathSync(execSync('command -v mmdc', { shell: '/bin/sh' }).toString().trim());
  for (let d = path.dirname(cli); d !== '/'; d = path.dirname(d))
    for (const p of [path.join(d, 'node_modules', 'mermaid'), path.join(d, 'mermaid')])
      try { if (JSON.parse(readFileSync(path.join(p, 'package.json'), 'utf8')).name === 'mermaid') return p; } catch {}
  throw new Error('no mermaid package above ' + cli);
})();
const version = JSON.parse(readFileSync(path.join(dir, 'package.json'), 'utf8')).version;
const chunks = path.join(dir, 'dist/chunks/mermaid.core');
const src = {};
for (const f of readdirSync(chunks).filter((f) => f.endsWith('.map'))) {
  const m = JSON.parse(readFileSync(path.join(chunks, f), 'utf8'));
  m.sources.forEach((s, i) => { const b = /src\/themes\/([\w.-]+)$/.exec(s); if (b && m.sourcesContent?.[i]) src[b[1]] ??= m.sourcesContent[i]; });
}
const need = (f) => src[f] ?? (() => { throw new Error(`${f} not in ${chunks}/*.map`); })();

// constants the themes import
const consts = {};
for (const [, k, v] of need('erDiagram-oldHardcodedValues.ts').matchAll(/export const (\w+) = '([^']*)'/g)) consts[k] = v;
if (!/mkBorder = \(col, darkMode\) =>\s*darkMode \? adjust\(col, \{ s: -40, l: 10 \}\) : adjust\(col, \{ s: -40, l: -10 \}\);/.test(need('theme-helpers.js')))
  throw new Error('theme-helpers.js: mkBorder changed; update mkborder() in src/mtheme.c');

// ---- tokenizer ----
function lex(s) {
  const out = [];
  const P = ['===', '!==', '?.', '??', '||', '&&', '<=', '>=', '++', '+=', '==', '!=', '=>'];
  for (let i = 0; i < s.length;) {
    const c = s[i];
    if (/\s/.test(c)) { i++; continue; }
    if (s.startsWith('//', i)) { i = s.indexOf('\n', i); if (i < 0) break; continue; }
    if (s.startsWith('/*', i)) { i = s.indexOf('*/', i) + 2; continue; }
    if (/[A-Za-z_$]/.test(c)) { const m = /^[\w$]+/.exec(s.slice(i))[0]; out.push({ t: 'id', v: m, i }); i += m.length; continue; }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(s[i + 1]))) { const m = /^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?/i.exec(s.slice(i))[0]; out.push({ t: 'num', v: Number(m), i }); i += m.length; continue; }
    if (c === "'" || c === '"') {
      let j = i + 1, v = '';
      for (; s[j] !== c; j++) { if (s[j] === '\\') j++; if (j >= s.length || s[j] === '\n') throw new Error('bad string at ' + i); v += s[j]; }
      out.push({ t: 'str', v, i }); i = j + 1; continue;
    }
    if (c === '`') throw new Error('template literal at ' + i);
    const p = P.find((p) => s.startsWith(p, i)) ?? c;
    if (!/^[{}()[\];,.?:=<>+\-*/!]$/.test(p) && !P.includes(p)) throw new Error(`unexpected '${c}' at ${i}`);
    out.push({ t: 'p', v: p, i }); i += p.length;
  }
  out.push({ t: 'eof', v: '', i: s.length });
  return out;
}

// ---- parser: the JavaScript the theme classes use, nothing more ----
function parser(toks, text) {
  let k = 0;
  const peek = (o = 0) => toks[k + o];
  const at = (v, o = 0) => toks[k + o].v === v && toks[k + o].t !== 'str';
  const fail = (m) => { const t = peek(); const line = text.slice(0, t.i).split('\n').length; throw new Error(`line ${line}: ${m} (at '${t.v}')`); };
  const eat = (v) => { if (!at(v)) fail(`expected '${v}'`); return toks[k++]; };
  const id = () => { if (peek().t !== 'id') fail('expected a name'); return toks[k++].v; };

  function primary() {
    const t = peek();
    if (t.t === 'num') return k++, { k: 'num', v: t.v };
    if (t.t === 'str') return k++, { k: 'str', v: t.v };
    if (at('(')) { k++; const e = expr(); eat(')'); return e; }
    if (at('{')) return object();
    if (t.t === 'id') {
      k++;
      if (t.v === 'true' || t.v === 'false') return { k: 'bool', v: t.v === 'true' };
      if (t.v === 'undefined' || t.v === 'null') return { k: 'undef' };
      if (t.v === 'this') return { k: 'this' };
      return { k: 'name', v: t.v };
    }
    fail('expected an expression');
  }
  function object() {
    eat('{');
    const props = [];
    while (!at('}')) {
      const key = peek().t === 'str' ? toks[k++].v : id();
      eat(':');
      props.push([key, assign()]);
      if (!at('}')) eat(',');
    }
    eat('}');
    return { k: 'obj', props };
  }
  function postfix() {
    let e = primary();
    for (;;) {
      if (at('.') || at('?.')) { k++; e = { k: 'mem', o: e, p: { k: 'str', v: id() } }; }
      else if (at('[')) { k++; const p = expr(); eat(']'); e = { k: 'mem', o: e, p }; }
      else if (at('(')) {
        k++;
        const args = [];
        while (!at(')')) { args.push(assign()); if (!at(')')) eat(','); }
        eat(')');
        e = { k: 'call', f: e, args };
      } else return e;
    }
  }
  function unary() {
    if (at('!') || at('-') || at('+')) {
      const op = toks[k++].v, e = unary();
      return op === '-' && e.k === 'num' ? { k: 'num', v: -e.v } : { k: 'un', op, e };
    }
    if (peek().t === 'id' && peek().v === 'typeof') fail('typeof');
    return postfix();
  }
  const bin = (next, ops) => () => {
    let e = next();
    while (ops.some((o) => at(o))) { const op = toks[k++].v; e = { k: 'bin', op, a: e, b: next() }; }
    return e;
  };
  const mul = bin(unary, ['*', '/']), add = bin(mul, ['+', '-']), rel = bin(add, ['<', '>', '<=', '>=']);
  const eq = bin(rel, ['===', '!==']), and = bin(eq, ['&&']), or = bin(and, ['||', '??']);
  function cond() {
    if (at('==') || at('!=')) fail('loose equality');
    const c = or();
    if (!at('?')) return c;
    k++;
    const a = assign();
    eat(':');
    return { k: 'cond', c, a, b: assign() };
  }
  function assign() { return cond(); }
  const expr = assign;

  function stmt() {
    if (at('{')) { k++; const body = []; while (!at('}')) body.push(stmt()); k++; return { k: 'block', body }; }
    if (at(';')) { k++; return { k: 'block', body: [] }; }
    if (at('if')) {
      k++; eat('('); const c = expr(); eat(')');
      const a = stmt();
      let b = null;
      if (at('else')) { k++; b = stmt(); }
      return { k: 'if', c, a, b };
    }
    if (at('for')) {
      k++; eat('(');
      if (!at('let')) fail('for without let');
      k++;
      const v = id(); eat('='); const init = expr(); eat(';');
      const c = expr(); eat(';');
      if (id() !== v) fail('for step on another variable');
      eat('++'); eat(')');
      return { k: 'for', v, init, c, body: stmt() };
    }
    if (at('const') || at('let')) { k++; const v = id(); eat('='); const e = expr(); eat(';'); return { k: 'let', v, e }; }
    if (at('return')) fail('return');
    const lhs = expr();
    if (at('=')) {
      k++;
      if (lhs.k !== 'mem' || lhs.o.k !== 'this') fail('assignment to something but this.x / this[x]');
      const e = expr(); eat(';');
      return { k: 'set', key: lhs.p, e };
    }
    eat(';');
    if (lhs.k === 'call' && lhs.f.k === 'mem' && lhs.f.o.k === 'this' && lhs.f.p.k === 'str' && lhs.args.length === 0)
      return { k: 'method', name: lhs.f.p.v };
    fail('expression statement');
  }
  // find `name(...) {` and leave k at its '{'
  function find(name) {
    for (k = 0; k < toks.length; k++)
      if (toks[k].v === name && toks[k].t === 'id' && at('(', 1)) {
        k++;
        eat('(');
        while (!at(')')) k++;
        k++;
        if (at('{')) return;
      }
    fail(`no method ${name}`);
  }
  function method(name) { find(name); return stmt(); }
  function methodText(name) { // the braces' text, unparsed (calculate() uses arrow functions)
    find(name);
    const start = peek().i;
    for (let d = 0; ; k++) {
      if (at('{')) d++;
      else if (at('}') && --d === 0) return text.slice(start, peek().i + 1);
    }
  }
  return { method, methodText };
}

// ---- C emission ----
let tmp = 0;
const cstr = (s) => JSON.stringify(s).replace(/\?/g, '\\?');
const S = (s) => `S(${cstr(s)})`;
const fnv1a = (s) => { // mtheme.c's hash(), over the key's bytes (the keys are ASCII)
  let h = 2166136261;
  for (const c of Buffer.from(s, 'utf8')) h = Math.imul(h ^ c, 16777619) >>> 0;
  return h;
};
const KEY = (s) => `K(${cstr(s)}, 0x${fnv1a(s).toString(16).padStart(8, '0')}u)`;
const AMT = { h: 1, s: 2, l: 4, r: 8, g: 16, b: 32 };

function key(p, locals) { // a this[...] key, as a Key expression
  if (p.k === 'str') return KEY(p.v);
  return `key_of(tstr(t, ${val(p, locals)}))`;
}
function memkey(e) { // this.a.b / this.a?.b -> "a.b", or null when not a static path under this
  if (e.k !== 'mem') return null;
  if (e.o.k === 'this') return e.p.k === 'str' ? e.p.v : null;
  const o = memkey(e.o);
  return o && e.p.k === 'str' ? o + '.' + e.p.v : null;
}
function num(e, locals) { return e.k === 'num' ? String(e.v) : `to_num(${val(e, locals)})`; }
function colour(e, locals) { return `tstr(t, ${val(e, locals)})`; }

function val(e, L) {
  switch (e.k) {
  case 'num': return `vnum(${e.v})`;
  case 'str': return `vstr(${S(e.v)})`;
  case 'bool': return `vbool(${e.v})`;
  case 'undef': return 'VUNDEF';
  case 'name':
    if (L.has(e.v)) return `l_${e.v}`;
    if (e.v in consts) return `vstr(${S(consts[e.v])})`;
    throw new Error('unknown name ' + e.v);
  case 'mem': {
    const mk = memkey(e);
    if (mk) return `get(t, ${KEY(mk)})`;
    if (e.o.k === 'this') return `get(t, ${key(e.p, L)})`;
    throw new Error('member access on a non-this object');
  }
  case 'un':
    if (e.op === '!') return `vbool(!truthy(${val(e.e, L)}))`;
    return `vnum(${e.op}to_num(${val(e.e, L)}))`;
  case 'cond': return `(truthy(${val(e.c, L)}) ? ${val(e.a, L)} : ${val(e.b, L)})`;
  case 'bin': {
    const a = val(e.a, L), b = val(e.b, L), v = `_v${tmp++}`;
    switch (e.op) {
    case '||': return `({ Val ${v} = ${a}; truthy(${v}) ? ${v} : ${b}; })`;
    case '??': return `({ Val ${v} = ${a}; ${v}.k != V_UNDEF ? ${v} : ${b}; })`;
    case '&&': return `({ Val ${v} = ${a}; truthy(${v}) ? ${b} : ${v}; })`;
    case '===': return `vbool(js_seq(${a}, ${b}))`;
    case '!==': return `vbool(!js_seq(${a}, ${b}))`;
    case '<': return `vbool(js_lt(${a}, ${b}, false))`;
    case '<=': return `vbool(js_lt(${a}, ${b}, true))`;
    case '>': return `vbool(js_lt(${b}, ${a}, false))`;
    case '>=': return `vbool(js_lt(${b}, ${a}, true))`;
    case '+': return `js_add(t, ${a}, ${b})`;
    default: return `vnum(to_num(${a}) ${e.op} to_num(${b}))`;
    }
  }
  case 'call': {
    if (e.f.k !== 'name') throw new Error('call of a non-function');
    const A = e.args, n = A.length, want = (lo, hi = lo) => { if (n < lo || n > hi) throw new Error(`${e.f.v}: ${n} arguments`); };
    switch (e.f.v) {
    case 'adjust': {
      want(2);
      if (A[1].k !== 'obj') throw new Error('adjust without an object literal');
      let mask = 0;
      const f = A[1].props.map(([p, x]) => { if (!(p in AMT)) throw new Error('adjust ' + p); mask |= AMT[p]; return `.${p} = ${num(x, L)}`; });
      return `cadjust(t, ${colour(A[0], L)}, (MCAmounts){.mask = ${mask}, ${f.join(', ')}})`;
    }
    case 'lighten': case 'darken': want(1, 2); return `c${e.f.v}(t, ${colour(A[0], L)}, ${n > 1 ? num(A[1], L) : 'NAN'})`;
    case 'invert': want(1); return `cinvert(t, ${colour(A[0], L)})`;
    case 'isDark': want(1); return `cis_dark(t, ${colour(A[0], L)})`;
    case 'rgba': want(4); return `crgba(t, ${A.map((x) => num(x, L)).join(', ')})`;
    case 'mkBorder': want(2); return `mkborder(t, ${colour(A[0], L)}, truthy(${val(A[1], L)}))`;
    default: throw new Error('unknown function ' + e.f.v);
    }
  }
  case 'obj': throw new Error('object literal outside an assignment');
  default: throw new Error('unexpected ' + e.k);
  }
}

// an object literal: its leaves, dotted, values evaluated in source order
function leaves(e, pre, L, out) {
  for (const [p, x] of e.props) {
    if (x.k === 'obj') leaves(x, pre + '.' + p, L, out);
    else out.push([pre + '.' + p, val(x, L)]);
  }
  return out;
}

function stmts(s, L, id, ind) {
  const pad = '    '.repeat(ind);
  switch (s.k) {
  case 'block': {
    const L2 = new Set(L);
    return s.body.map((x) => stmts(x, L2, id, ind)).join('');
  }
  case 'let': { const c = `${pad}Val l_${s.v} = ${val(s.e, L)};\n`; L.add(s.v); return c; }
  case 'if': {
    let c = `${pad}if (truthy(${val(s.c, L)})) {\n${stmts(s.a, new Set(L), id, ind + 1)}${pad}}`;
    if (s.b) c += ` else {\n${stmts(s.b, new Set(L), id, ind + 1)}${pad}}`;
    return c + '\n';
  }
  case 'for': {
    const L2 = new Set(L);
    L2.add(s.v);
    const v = `l_${s.v}`;
    return `${pad}for (Val ${v} = ${val(s.init, L)}; truthy(${val(s.c, L2)}); ${v} = vnum(to_num(${v}) + 1)) {\n`
      + stmts(s.body, L2, id, ind + 1) + `${pad}}\n`;
  }
  case 'set': {
    if (s.e.k === 'obj') { // the whole nested object is replaced, after every value is evaluated
      const base = s.key.k === 'str' ? s.key.v : null;
      if (!base) throw new Error('object assigned to this[...]');
      const lv = leaves(s.e, base, L, []);
      let c = `${pad}{\n`;
      lv.forEach(([, x], i) => { c += `${pad}    Val o${i} = ${x};\n`; });
      c += `${pad}    drop(t, ${KEY(base)});\n`;
      lv.forEach(([k], i) => { c += `${pad}    put(t, ${KEY(k)}, o${i});\n`; });
      return c + `${pad}}\n`;
    }
    return `${pad}put(t, ${key(s.key, L)}, ${val(s.e, L)});\n`;
  }
  case 'method':
    if (s.name !== 'updateColors') throw new Error('call of this.' + s.name);
    return `${pad}${id}_update(t);\n`;
  default: throw new Error('statement ' + s.k);
  }
}

// the calculate() each theme runs, as the variant src/mtheme.c implements; checked against the source
const norm = (s) => s.replace(/\/\*[\s\S]*?\*\/|\/\/.*$/gm, '').replace(/\s+/g, '');
const KNOWN = {
  default: 'CALC_RESET_COPY', base: 'CALC_COPY_GRADIENT', dark: 'CALC_COPY', forest: 'CALC_COPY', neutral: 'CALC_COPY',
};
const EXPECT = { // the calculate() bodies those variants were written from (mermaid 12.0.0)
  CALC_COPY: 'if(typeofoverrides!==\'object\'){this.updateColors();return;}constkeys=Object.keys(overrides);keys.forEach((k)=>{this[k]=overrides[k];});this.updateColors();keys.forEach((k)=>{this[k]=overrides[k];});',
};
EXPECT.CALC_RESET_COPY = 'Object.keys(this).forEach((k)=>{if(this[k]===\'calculated\'){this[k]=undefined;}});' + EXPECT.CALC_COPY;
EXPECT.CALC_COPY_GRADIENT = EXPECT.CALC_COPY + 'if(Object.hasOwn(overrides,\'nodeBorder\')&&!Object.hasOwn(overrides,\'useGradient\')){this.useGradient=false;}';

let out = `/* generated by tools/gen_mtheme.mjs from mermaid ${version}'s src/themes: do not edit */\n`;
for (const id of ['default', 'base', 'dark', 'forest', 'neutral']) {
  const text = need(`theme-${id}.js`);
  const p = parser(lex(text), text);
  const got = norm(p.methodText('calculate')).slice(1, -1);
  if (got !== EXPECT[KNOWN[id]])
    throw new Error(`theme-${id}.js: calculate() changed; update its variant in src/mtheme.c and EXPECT here:\n${got}`);
  out += `\nstatic void ${id}_update(MTheme *t);\n`;
  for (const m of ['constructor', 'updateColors']) {
    const body = p.method(m);
    tmp = 0;
    out += `static void ${id}_${m === 'constructor' ? 'ctor' : 'update'}(MTheme *t) {\n${stmts(body, new Set(), id, 1)}}\n`;
  }
  out += `static constexpr Calc ${id}_calc = ${KNOWN[id]};\n`;
}
process.stdout.write(out);
