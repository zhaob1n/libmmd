#!/usr/bin/env python3
"""Node-order agreement of state diagrams with mermaid-cli (look: classic, layout: dagre).
usage: bench/stateagree.py [BIN] [files.mmd ...]   (default: tests/docs/stateDiagram, tests/cases/*state*)
States, notes and composites are matched by label; a pair agrees on an axis when both renderers put
the two in the same order along it (1px slack). mermaid's SVGs are cached in $TMPDIR/mmd-$UID/stateagree."""
import concurrent.futures as cf, glob, itertools, json, os, re, subprocess, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')); import env
args = env.at_root(sys.argv[1:])
B = args.pop(0) if args and not args[0].endswith('.mmd') else 'build/mmd'
files = args or sorted(glob.glob('tests/docs/stateDiagram/*.mmd')) + sorted(glob.glob('tests/cases/*state*.mmd'))
D, NS = env.scratch('stateagree'), '{http://www.w3.org/2000/svg}'
cfg = os.path.join(D, 'cfg.json')
json.dump({'look': 'classic', 'layout': 'dagre', 'theme': 'default'}, open(cfg, 'w'))

def ref(src):
    out = os.path.join(D, src.replace('/', '_')[:-4] + '.svg')
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
        subprocess.run([*env.mmdc(), '-c', cfg, '-i', src, '-o', out], capture_output=True, timeout=120)
    return out if os.path.exists(out) else None

def label(e):
    t = ' '.join(s.strip() for s in e.itertext() if s.strip())
    return t.split(' ')[0] if t else ''

def mermaid_pos(path):
    d = {}
    def walk(e, ox, oy):
        m = re.search(r'translate\(\s*([-\d.e]+)[ ,]+([-\d.e]+)', e.get('transform', ''))
        if m: ox, oy = ox + float(m.group(1)), oy + float(m.group(2))
        cl = (e.get('class') or '').split()
        if e.tag == NS + 'g' and 'node' in cl:
            m = re.match(r'my-svg-state-(.+)-\d+$', e.get('id', ''))
            if m and not re.search(r'_(start|end)$', m.group(1)):
                d[('NOTE:' + ' '.join(e.itertext()).split()[0]) if 'statediagram-note' in cl else label(e) or m.group(1)] = (ox, oy)
            return
        if e.tag == NS + 'g' and 'statediagram-cluster' in cl and 'statediagram-cluster-alt' not in cl and e.get('data-id'):
            r = e.find(NS + 'rect')
            if r is not None:
                d['C:' + e.get('data-id')] = (ox + float(r.get('x')) + float(r.get('width')) / 2, oy + float(r.get('y')) + float(r.get('height')) / 2)
        for c in e: walk(c, ox, oy)
    walk(ET.parse(path).getroot(), 0, 0)
    return d

def our_pos(svg):
    d, num = {}, r'([-\d.]+)'
    for m in re.finditer(rf'<rect class="(st|note)" x="{num}" y="{num}" width="{num}" height="{num}"[^>]*/>(?:<text[^>]*>(.*?)</text>)?', svg):
        x, y, w, h = map(float, m.group(2, 3, 4, 5))
        t = re.sub(r'<[^>]+>', ' ', m.group(6) or '').split()
        d[('NOTE:' if m.group(1) == 'note' else '') + (t[0] if t else '')] = (x + w / 2, y + h / 2)
    for m in re.finditer(rf'<rect class="comp" x="{num}" y="{num}" width="{num}" height="{num}"[^>]*/><path class="comp-t"[^>]*/><text[^>]*>([^<]*)', svg):
        x, y, w, h = map(float, m.group(1, 2, 3, 4))
        d['C:' + m.group(5)] = (x + w / 2, y + h / 2)
    return d

with cf.ThreadPoolExecutor(6) as ex: refs = list(ex.map(ref, files))
sg = lambda a, b: (a > b + 1) - (a < b - 1)
T = [0, 0, 0]
for src, r in zip(files, refs):
    if not r: print(f'{src}: mermaid-cli failed'); continue
    theirs, ours = mermaid_pos(r), our_pos(subprocess.run([B, src], capture_output=True, text=True).stdout)
    k = [v for v in theirs if v in ours]
    agree = [0, 0]
    for u, v in itertools.combinations(k, 2):
        for a in range(2): agree[a] += sg(ours[u][a], ours[v][a]) == sg(theirs[u][a], theirs[v][a])
    n = len(k) * (len(k) - 1) // 2
    T[0] += agree[0]; T[1] += agree[1]; T[2] += n
    print(f'{src:40s} x {agree[0]}/{n} y {agree[1]}/{n} matched {len(k)}/{len(theirs)}')
print('TOTAL x %.4f y %.4f pairs %d' % (T[0] / max(T[2], 1), T[1] / max(T[2], 1), T[2]))
