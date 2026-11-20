// Generates src/mtheme_gen.h: mermaid's five classic themes (default, base, dark, forest, neutral)
// as C, from mermaid's own package:
//  - every variable a theme can hold as a slot, X_<name>, and the tables to find one by name;
//  - per theme, the variables its constructor leaves (<id>_init) and those getThemeVariables()
//    gives without overrides (<id>_preset), both read off mermaid's theme objects, so neither the
//    constructor nor a preset costs anything at run time;
//  - per theme, updateColors() as <id>_update(), statement for statement from the theme sources
//    mermaid's dist carries in its sourcemaps.
// src/mtheme.c holds the runtime they call and the hand-written calculate() variants, which this
// tool checks against the sources.
// usage: node tools/gen_mtheme.mjs [mermaid package dir] > src/mtheme_gen.h
import { readdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import { flat, loadThemes, mermaidDir } from './mermaid_themes.mjs';

const dir = mermaidDir(process.argv[2]);
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

// ---- what the themes hold ----
const IDS = ['default', 'base', 'dark', 'forest', 'neutral'];
// the calculate() each theme runs, as the variant src/mtheme.c implements; checked against the source
const norm = (s) => s.replace(/\/\*[\s\S]*?\*\/|\/\/.*$/gm, '').replace(/\s+/g, '');
const KNOWN = { default: 'RESET_COPY', base: 'COPY_GRADIENT', dark: 'COPY', forest: 'COPY', neutral: 'COPY' };
const EXPECT = { // the calculate() bodies those variants were written from (mermaid 12.0.0)
  COPY: 'if(typeofoverrides!==\'object\'){this.updateColors();return;}constkeys=Object.keys(overrides);keys.forEach((k)=>{this[k]=overrides[k];});this.updateColors();keys.forEach((k)=>{this[k]=overrides[k];});',
};
EXPECT.RESET_COPY = 'Object.keys(this).forEach((k)=>{if(this[k]===\'calculated\'){this[k]=undefined;}});' + EXPECT.COPY;
EXPECT.COPY_GRADIENT = EXPECT.COPY + 'if(Object.hasOwn(overrides,\'nodeBorder\')&&!Object.hasOwn(overrides,\'useGradient\')){this.useGradient=false;}';

const themes = await loadThemes(dir);
// the theme as its constructor leaves it: getThemeVariables() with calculate() stubbed out
function constructed(id) {
  const proto = Object.getPrototypeOf(themes[id].getThemeVariables());
  const calc = proto.calculate;
  proto.calculate = function () {};
  try { return themes[id].getThemeVariables(); } finally { proto.calculate = calc; }
}
const typed = (v) => {
  if (!['string', 'number', 'boolean'].includes(typeof v) || (typeof v === 'number' && !Number.isFinite(v)))
    throw new Error('theme value ' + String(v));
  return v;
};

const T = {};
const keys = new Set();
for (const id of IDS) {
  const text = need(`theme-${id}.js`);
  const p = parser(lex(text), text);
  const got = norm(p.methodText('calculate')).slice(1, -1);
  if (got !== EXPECT[KNOWN[id]])
    throw new Error(`theme-${id}.js: calculate() changed; update its variant in src/mtheme.c and EXPECT here:\n${got}`);
  const obj = constructed(id);
  if (KNOWN[id] === 'RESET_COPY') { // calculate()'s first step, on what the constructor left
    for (const k of Object.keys(obj)) if (obj[k] === 'calculated') obj[k] = undefined;
    if (Object.values(flat(obj)).includes('calculated')) throw new Error(`theme-${id}.js: a nested 'calculated'`);
  }
  T[id] = { update: p.method('updateColors'), init: flat(obj, '', {}, typed), preset: flat(themes[id].getThemeVariables()) };
  for (const k of [...Object.keys(T[id].init), ...Object.keys(T[id].preset)]) keys.add(k);
}

// every this.<key> and this['prefix' + n] the updates name
const walk = (n, f) => {
  if (!n || typeof n !== 'object') return;
  if (Array.isArray(n)) { n.forEach((x) => walk(x, f)); return; }
  f(n);
  for (const v of Object.values(n)) walk(v, f);
};
const famkey = (p) => p.k === 'bin' && p.op === '+' && p.a.k === 'str' ? p.a.v : null;
// a this[...] key the source spells out: 'a' or 'prefix' + 1
const litkey = (p) => (p.k === 'str' ? p.v : famkey(p) && p.b.k === 'num' ? p.a.v + p.b.v : null);
function memkey(e) { // this.a.b / this.a?.b -> "a.b", or null when not a static path under this
  if (e.k !== 'mem') return null;
  if (e.o.k === 'this') return litkey(e.p);
  const o = memkey(e.o);
  return o && e.p.k === 'str' ? o + '.' + e.p.v : null;
}
function leaves(e, pre, out) { // an object literal's leaves, dotted, in source order
  for (const [p, x] of e.props) {
    if (x.k === 'obj') leaves(x, pre + '.' + p, out);
    else out.push([pre + '.' + p, x]);
  }
  return out;
}
const prefixes = new Set();
for (const id of IDS) walk(T[id].update, (n) => {
  const mk = memkey(n);
  if (mk) keys.add(mk);
  if (n.k === 'mem' && n.o.k === 'this' && !litkey(n.p) && famkey(n.p)) prefixes.add(famkey(n.p));
  if (n.k === 'set' && litkey(n.key)) {
    keys.add(litkey(n.key));
    if (n.e.k === 'obj') leaves(n.e, n.key.v, []).forEach(([k]) => keys.add(k));
  }
  if (n.k === 'set' && !litkey(n.key) && famkey(n.key)) prefixes.add(famkey(n.key));
});
for (const k of ['nodeBorder', 'useGradient']) keys.add(k); // src/mtheme.c's calculate() variants
for (const k of [...keys]) if (k.includes('.')) keys.add(k.slice(0, k.indexOf('.')));

const NAMES = [...keys].sort();
const SLOT = new Map();
const ids = new Set();
NAMES.forEach((k, i) => {
  const c = 'X_' + k.replace(/[^A-Za-z0-9_]/g, '_');
  if (ids.has(c)) throw new Error('two keys map to ' + c);
  ids.add(c);
  SLOT.set(k, { c, i });
});
const X = (k) => SLOT.get(k)?.c ?? (() => { throw new Error('no slot for ' + k); })();

// ---- C emission ----
const cstr = (s) => JSON.stringify(s).replace(/\?/g, '\\?');
const S = (s) => `S(${cstr(s)})`;
const cnum = (v) => (Object.is(v, -0) ? '-0.0' : v < 0 ? `(${v})` : String(v));
const AMT = { h: 1, s: 2, l: 4, r: 8, g: 16, b: 32 };
let tmp = 0;

// static types: 'num' (a double), 'bool', 'str' (a literal), 'val' (a Val)
function ty(e, L) {
  switch (e.k) {
  case 'num': return 'num';
  case 'str': return 'str';
  case 'bool': return 'bool';
  case 'name': return L.has(e.v) ? L.get(e.v) : e.v in consts ? 'str' : (() => { throw new Error('unknown name ' + e.v); })();
  case 'un': return e.op === '!' ? 'bool' : 'num';
  case 'cond': { const a = ty(e.a, L); return a === ty(e.b, L) && a !== 'str' ? a : 'val'; }
  case 'bin':
    if (['*', '/', '-'].includes(e.op)) return 'num';
    if (e.op === '+') return ty(e.a, L) === 'num' && ty(e.b, L) === 'num' ? 'num' : 'val';
    if (['===', '!==', '<', '>', '<=', '>='].includes(e.op)) return 'bool';
    return 'val';
  case 'call': return e.f.v === 'isDark' ? 'bool' : 'val';
  default: return 'val';
  }
}
const strof = (e) => (e.k === 'str' ? e.v : consts[e.v]);
// the value of a numeric expression the generator can work out, or undefined
function konst(e, L) {
  if (e.k === 'num') return e.v;
  if (e.k === 'un' && e.op !== '!') { const x = konst(e.e, L); return x === undefined ? x : e.op === '-' ? -x : x; }
  if (e.k === 'bin' && ty(e, L) === 'num') {
    const a = konst(e.a, L), b = konst(e.b, L);
    if (a === undefined || b === undefined) return undefined;
    return { '*': a * b, '/': a / b, '-': a - b, '+': a + b }[e.op];
  }
  return undefined;
}

function V(e, L) { // a Val
  switch (ty(e, L)) {
  case 'num': return `vnum(${N(e, L)})`;
  case 'bool': return `vbool(${B(e, L)})`;
  case 'str': return `vstr(${S(strof(e))})`;
  }
  switch (e.k) {
  case 'undef': return 'VUNDEF';
  case 'name': return `l_${e.v}`;
  case 'mem': {
    const mk = memkey(e);
    if (mk) return `G(${X(mk)})`;
    if (e.o.k === 'this') return `getk(t, ${KEY(e.p, L)})`;
    throw new Error('member access on a non-this object');
  }
  case 'cond': return `(${B(e.c, L)} ? ${V(e.a, L)} : ${V(e.b, L)})`;
  case 'bin': {
    const a = V(e.a, L), b = V(e.b, L), v = `_v${tmp++}`;
    switch (e.op) {
    case '||': return `({ Val ${v} = ${a}; truthy(${v}) ? ${v} : ${b}; })`;
    case '??': return `({ Val ${v} = ${a}; ${v}.k != V_UNDEF ? ${v} : ${b}; })`;
    case '&&': return `({ Val ${v} = ${a}; truthy(${v}) ? ${b} : ${v}; })`;
    case '+': throw new Error('string + in updateColors(): src/mtheme.c has no js_add()');
    }
    throw new Error('operator ' + e.op);
  }
  case 'call': {
    if (e.f.k !== 'name') throw new Error('call of a non-function');
    const A = e.args, n = A.length, want = (lo, hi = lo) => { if (n < lo || n > hi) throw new Error(`${e.f.v}: ${n} arguments`); };
    switch (e.f.v) {
    case 'adjust': {
      want(2);
      if (A[1].k !== 'obj') throw new Error('adjust without an object literal');
      let mask = 0;
      const amt = { h: '0', s: '0', l: '0', r: '0', g: '0', b: '0' };
      for (const [p, x] of A[1].props) { if (!(p in AMT)) throw new Error('adjust ' + p); mask |= AMT[p]; amt[p] = N(x, L); }
      return `cadjust(t, ${C(A[0], L)}, ${mask}, ${Object.values(amt).join(', ')})`;
    }
    case 'lighten': want(1, 2); return `clighten(t, ${C(A[0], L)}, ${n > 1 ? N(A[1], L) : 'NAN'})`;
    case 'darken': want(1, 2); return `clighten(t, ${C(A[0], L)}, ${n > 1 ? `-${N(A[1], L)}` : 'NAN'})`;
    case 'invert': want(1); return `cinvert(t, ${C(A[0], L)})`;
    case 'mkBorder': want(2); return `mkborder(t, ${C(A[0], L)}, ${B(A[1], L)})`;
    default: throw new Error('unknown function ' + e.f.v);
    }
  }
  case 'obj': throw new Error('object literal outside an assignment');
  default: throw new Error('unexpected ' + e.k);
  }
}
function N(e, L) { // a double
  const k = konst(e, L);
  if (k !== undefined) return cnum(k);
  switch (ty(e, L)) {
  case 'bool': return `(double)${B(e, L)}`;
  case 'str': return cnum(Number(strof(e)));
  case 'val': return `to_num(${V(e, L)})`;
  }
  switch (e.k) {
  case 'name': return `l_${e.v}`;
  case 'un': return e.op === '-' ? `(-${N(e.e, L)})` : N(e.e, L);
  case 'cond': return `(${B(e.c, L)} ? ${N(e.a, L)} : ${N(e.b, L)})`;
  case 'bin': return `(${N(e.a, L)} ${e.op} ${N(e.b, L)})`;
  }
  throw new Error('number ' + e.k);
}
function B(e, L) { // JavaScript's truthiness, as a C bool
  switch (ty(e, L)) {
  case 'num': return `truthy_num(${N(e, L)})`;
  case 'str': return strof(e) ? 'true' : 'false';
  case 'val': return `truthy(${V(e, L)})`;
  }
  switch (e.k) {
  case 'bool': return e.v ? 'true' : 'false';
  case 'un': return `!${B(e.e, L)}`;
  case 'cond': return `(${B(e.c, L)} ? ${B(e.a, L)} : ${B(e.b, L)})`;
  case 'call': return `mc_is_dark(${C(e.args[0], L)})`;
  case 'bin': {
    if (e.op === '===' || e.op === '!==') return `${e.op === '!==' ? '!' : ''}js_seq(t, ${V(e.a, L)}, ${V(e.b, L)})`;
    const [a, b] = e.op[0] === '<' ? [e.a, e.b] : [e.b, e.a], eq = e.op.length > 1;
    if (ty(a, L) === 'num' || ty(b, L) === 'num') return `(${N(a, L)} ${eq ? '<=' : '<'} ${N(b, L)})`; // a number: compared as numbers
    throw new Error('a comparison of two values that are not numbers: src/mtheme.c has no js_lt()');
  }
  }
  throw new Error('condition ' + e.k);
}
function C(e, L) { // a colour khroma reads
  const mk = memkey(e);
  return mk ? `col(t, ${X(mk)})` : `colv(t, ${V(e, L)})`;
}
function KEY(p, L) { // a computed this[...] key
  const f = famkey(p);
  if (f && prefixes.has(f) && ty(p.b, L) === 'num') return `fam(t, F_${f}, ${N(p.b, L)})`;
  throw new Error('this[...] with a key that is not \'prefix\' + a number: src/mtheme.c has no key_str()');
}

function stmts(s, L, ind) {
  const pad = '    '.repeat(ind);
  switch (s.k) {
  case 'block': {
    const L2 = new Map(L);
    return s.body.map((x) => stmts(x, L2, ind)).join('');
  }
  case 'let': { const t = ty(s.e, L) === 'num' ? 'num' : 'val'; const c = t === 'num' ? `${pad}double l_${s.v} = ${N(s.e, L)};\n` : `${pad}Val l_${s.v} = ${V(s.e, L)};\n`; L.set(s.v, t); return c; }
  case 'if': {
    let c = `${pad}if (${B(s.c, L)}) {\n${stmts(s.a, new Map(L), ind + 1)}${pad}}`;
    if (s.b) c += ` else {\n${stmts(s.b, new Map(L), ind + 1)}${pad}}`;
    return c + '\n';
  }
  case 'for': {
    const L2 = new Map(L);
    L2.set(s.v, 'num');
    return `${pad}for (double l_${s.v} = ${N(s.init, L)}; ${B(s.c, L2)} && t->ok; l_${s.v}++) {\n`
      + stmts(s.body, L2, ind + 1) + `${pad}}\n`;
  }
  case 'set': {
    if (s.e.k === 'obj') { // the whole nested object is replaced, after every value is evaluated
      if (s.key.k !== 'str') throw new Error('object assigned to this[...]');
      const base = s.key.v, lv = leaves(s.e, base, []);
      let c = `${pad}{\n`;
      lv.forEach(([, x], i) => { c += `${pad}    Val o${i} = ${V(x, L)};\n`; });
      c += `${pad}    drop(t, ${X(base)});\n`;
      lv.forEach(([k], i) => { c += `${pad}    putf(t, ${X(k)}, ${X(base)}, o${i});\n`; });
      return c + `${pad}}\n`;
    }
    // this.k = this.k || v (|| w ...): a value that is there stays, and it is not an object (which
    // has none), so nothing is stored
    const self = JSON.stringify({ k: 'mem', o: { k: 'this' }, p: s.key });
    const rest = (e) => {
      if (e.k !== 'bin' || (e.op !== '||' && e.op !== '??')) return null;
      if (JSON.stringify(e.a) === self) return { op: e.op, v: e.b };
      const r = e.op === '||' ? rest(e.a) : null;
      return r && r.op === '||' ? { op: '||', v: { k: 'bin', op: '||', a: r.v, b: e.b } } : null;
    };
    const keep = rest(s.e);
    const test = (v) => (keep.op === '||' ? `!truthy(${v})` : `${v}.k == V_UNDEF`);
    if (litkey(s.key)) {
      if (litkey(s.key).includes('.')) throw new Error('this[\'a.b\'] = ...');
      const x = X(litkey(s.key));
      return keep ? `${pad}if (${test(`G(${x})`)}) put(t, ${x}, ${V(keep.v, L)});\n` : `${pad}put(t, ${x}, ${V(s.e, L)});\n`;
    }
    if (!keep) return `${pad}putk(t, ${KEY(s.key, L)}, ${V(s.e, L)});\n`;
    return `${pad}{\n${pad}    Key k = ${KEY(s.key, L)};\n${pad}    if (${test('getk(t, k)')}) putk(t, k, ${V(keep.v, L)});\n${pad}}\n`;
  }
  case 'method': throw new Error('call of this.' + s.name + ' in updateColors()');
  default: throw new Error('statement ' + s.k);
  }
}

// ---- the header ----
const fnv1a = (s) => { // mtheme.c's hash(), over the key's bytes (the keys are ASCII)
  let h = 2166136261;
  for (const c of Buffer.from(s, 'utf8')) h = Math.imul(h ^ c, 16777619) >>> 0;
  return h;
};
let hbits = 1;
while (1 << hbits < 2 * NAMES.length) hbits++;
const HT = new Array(1 << hbits).fill(0);
NAMES.forEach((k, i) => { let j = fnv1a(k) & ((1 << hbits) - 1); while (HT[j]) j = (j + 1) & ((1 << hbits) - 1); HT[j] = i + 1; });
const wrap = (items, ind = '    ', w = 100) => {
  const lines = [];
  let cur = '';
  for (const it of items) { if (cur && cur.length + it.length + 2 > w) { lines.push(cur); cur = ''; } cur += (cur ? ' ' : '') + it + ','; }
  if (cur) lines.push(cur);
  return lines.map((l) => ind + l).join('\n');
};

let out = `/* generated by tools/gen_mtheme.mjs from mermaid ${version}: do not edit */\n\n`;
out += `enum : uint16_t {\n${wrap(NAMES.map((k) => X(k)))}\n    NSLOT\n};\n`;
out += `static const Str NAMES[NSLOT] = {\n${wrap(NAMES.map(S))}\n};\n`;
out += `/* slot + 1 at FNV-1a(name) or the next free entry */\nenum { HBITS = ${hbits} };\nstatic const uint16_t HT[1 << HBITS] = {\n${wrap(HT.map(String))}\n};\n`;
// the objects: their fields are the slots of names starting with "<root>."
const objs = NAMES.filter((k) => NAMES.some((f) => f.startsWith(k + '.')));
out += `static const Obj OBJS[] = {\n${objs.map((k) => {
  const f = NAMES.filter((n) => n.startsWith(k + '.'));
  return `    {${X(k)}, ${X(f[0])}, ${X(f[f.length - 1])} + 1},`;
}).join('\n')}\n};\n`;
// this['prefix' + n]: the slot of each n there is one for
const fams = [...prefixes].sort();
out += `enum { ${fams.map((f) => `F_${f}`).join(', ')} };\n`;
out += `static const Fam FAMS[] = {\n${fams.map((f) => {
  const idx = NAMES.filter((k) => k.startsWith(f) && /^(0|[1-9]\d*)$/.test(k.slice(f.length))).map((k) => +k.slice(f.length));
  const n = Math.max(-1, ...idx) + 1;
  const sl = Array.from({ length: n }, (_, i) => (SLOT.has(f + i) ? X(f + i) : 'NONE'));
  return `    [F_${f}] = {${S(f)}, ${n}, (const uint16_t[]){${sl.join(', ')}}},`;
}).join('\n')}\n};\n`;

const val = (v) => (typeof v === 'string' ? `VS(${cstr(v)})` : typeof v === 'number' ? `VN(${cnum(v)})` : `VB(${v})`);
for (const id of IDS) {
  const t = T[id];
  out += `\nstatic const Val ${id}_init[NSLOT] = {\n${Object.entries(t.init).map(([k, v]) => `    [${X(k)}] = ${val(v)},`).join('\n')}\n};\n`;
  out += `static const Val ${id}_preset[NSLOT] = {\n${Object.entries(t.preset).map(([k, v]) => `    [${X(k)}] = ${val(v)},`).join('\n')}\n};\n`;
  tmp = 0;
  out += `static void ${id}_update(Th t[static 1]) {\n${stmts(t.update, new Map(), 1)}}\n`;
}
out += '\nstatic const ThemeDef THEMES[] = {\n';
for (const id of IDS) {
  const roots = [...new Set(Object.keys(T[id].init).filter((k) => k.includes('.')).map((k) => k.slice(0, k.indexOf('.'))))].sort();
  out += `    [MT_${id.toUpperCase()}] = {${id}_init, ${id}_update, CALC_${KNOWN[id] === 'COPY_GRADIENT' ? 'COPY_GRADIENT' : 'COPY'}, {.v = ${id}_preset},\n`
    + `        ${roots.length}, (const uint16_t[]){${roots.map(X).join(', ')}}},\n`;
}
out += '};\n';
process.stdout.write(out);
