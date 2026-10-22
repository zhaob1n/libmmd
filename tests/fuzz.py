#!/usr/bin/env python3
"""Mutation fuzzer: feeds mutated test cases to an (ASAN) binary, reports crashes."""
import random, subprocess, sys, glob
BIN = sys.argv[1] if len(sys.argv) > 1 else 'build-asan/mmdc-c'
N = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
import os
seeds = [open(f, 'rb').read() for f in glob.glob(os.environ.get('FUZZ_GLOB', 'tests/cases/*.mmd'))]
toks = [b'-->', b'---', b'-.-', b'==>', b'[', b']', b'(', b')', b'{', b'}', b'|', b'"', b'subgraph x', b'end', b'\n', b'&', b':::', b'%%', b'<br>', b'@{', b'->>', b'Note over A', b'loop', b'#', b';', b'\xe4\xbd\xa0', b'\xff', b'[*]', b'state ', b'<<fork>>', b'--', b'note left of ', b'end note', b'class ', b'<|--', b'*--', b'o--', b'..>', b'()--', b'~T~', b'namespace N {', b'||--o{', b'}|..|{', b' to ', b'PK, FK', b'"x" : ', b'showData', b'create participant ', b'destroy ', b'box Aqua G', b'par_over', b'@{ "type": "queue" }', b'@{ "type": "database" }', b'"type": "boundary"', b'collections',
        b'dateFormat ', b'after a1', b'until ', b'crit, ', b'milestone, ', b', 3d', b'excludes weekends', b'axisFormat %b', b'section S', b': 5: ', b': 0.5', b'[0.3, 0.9]', b'radius: 9', b'x-axis ', b'y-axis ', b' --> ', b'bar [', b'line [', b'horizontal', b'quadrant-1 ', b'commit ', b'id: "', b'branch b', b'checkout ', b'merge ', b'cherry-pick ', b'tag: "', b'type: REVERSE', b'order: ', b'((', b'))', b'{{', b'::icon(x)', b'  ', b'\t', b'packet-beta\n', b'0-15: "F"', b'+8: ', b'axis a, b', b'curve c{1,2', b'max 10', b'treemap-beta\n', b'"Leaf": 5', b'@{ priority: ', b'ticket: ', b'sankey-beta\n', b'A,B,10\n', b'"q,""x",', b'columns 3', b'space:2', b'block:g["G"]:2', b'a:2', b'id[("db")]', b'-- "t" -->', b'requirement r {', b'id: 1', b'risk: high', b' - satisfies -> ', b' <- copies - ', b'element e {', b'Person(p, "P")', b'System_Boundary(b, "B") {', b'Rel(a, b, "u", "t")', b'ContainerDb(d, "D", "t", "x")', b'$tags=', b'service s(server)[S]', b'group g(cloud)[G]', b' in g', b'junction j', b':R -- L:', b':B --> T:', b'-[lbl]-', b'{group}', b'UpdateLayoutConfig($c4ShapeInRow="1", $c4BoundaryInRow="0")', b'UpdateElementStyle(p, $bgColor="red", $fontColor=', b'UpdateRelStyle(a, b, $offsetX="-1e9", $lineColor="', b'Rel_Back(', b'(logos:github)', b'(logos:aws)', b'(fa:', b'::icon(fa fa-book)', b'::icon(fas fa-', b'::icon(fa-solid fa-heart)']
r = random.Random(1); bad = 0
for i in range(N):
    s = bytearray(r.choice(seeds))
    for _ in range(r.randint(1, 8)):
        op = r.random(); p = r.randint(0, len(s))
        if op < 0.4: s[p:p] = r.choice(toks)
        elif op < 0.7: del s[p:p + r.randint(1, 10)]
        else: s[p:p] = s[r.randint(0, len(s)):][:r.randint(1, 40)]
    pr = subprocess.run([BIN], input=bytes(s), capture_output=True)
    if pr.returncode not in (0, 1) or b'ERROR: AddressSanitizer' in pr.stderr or b'runtime error' in pr.stderr:
        bad += 1; open(f'/tmp/crash{bad}.mmd', 'wb').write(s); print('CRASH', pr.returncode, pr.stderr[:300])
print(f'{N} cases, {bad} crashes')
