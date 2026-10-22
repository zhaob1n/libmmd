#!/usr/bin/env python3
"""Generate deterministic flowchart benchmark inputs."""
import random, sys, os
SH = ['[{}]', '({})', '{{{}}}', '([{}])', '[[{}]]', '(({}))', '{{{{{}}}}}', '[({})]']
def gen(n, fan, label_rate, seed, subgraphs=0):
    r = random.Random(seed)
    out = ['flowchart TD']
    for i in range(n):
        out.append(f'  n{i}' + r.choice(SH).format(f'Node {i}'))
    sg = []
    if subgraphs:
        per = n // (subgraphs * 3)
        for s in range(subgraphs):
            out.append(f'  subgraph s{s} [Group {s}]')
            for k in range(per):
                out.append(f'    n{s*per+k}')
            out.append('  end')
    for i in range(1, n):
        for _ in range(r.randint(1, fan)):
            j = r.randint(max(0, i - 30), i - 1)
            arrow = r.choice(['-->', '-->', '-.->', '==>', '---'])
            if r.random() < label_rate:
                out.append(f'  n{j} {arrow}|edge {j}-{i}| n{i}')
            else:
                out.append(f'  n{j} {arrow} n{i}')
    for _ in range(n // 50):  # a few back edges (cycles)
        a = r.randint(0, n - 1); b = r.randint(0, a)
        out.append(f'  n{a} --> n{b}')
    return '\n'.join(out) + '\n'
d = sys.argv[1] if len(sys.argv) > 1 else 'bench/inputs'
os.makedirs(d, exist_ok=True)
for name, args in {'small': (10, 2, 0.3, 1), 'medium': (100, 2, 0.2, 2, 3),
                   'large': (1000, 2, 0.1, 3), 'xlarge': (3000, 2, 0.05, 4)}.items():
    open(f'{d}/{name}.mmd', 'w').write(gen(*args))
