#!/usr/bin/env python3
"""Mutation fuzzer: feeds mutated test cases to an (ASAN) binary, reports crashes."""
import random, subprocess, sys, glob
BIN = sys.argv[1] if len(sys.argv) > 1 else 'build-asan/mmdc-c'
N = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
seeds = [open(f, 'rb').read() for f in glob.glob('tests/cases/*.mmd')]
toks = [b'-->', b'---', b'-.-', b'==>', b'[', b']', b'(', b')', b'{', b'}', b'|', b'"', b'subgraph x', b'end', b'\n', b'&', b':::', b'%%', b'<br>', b'@{', b'->>', b'Note over A', b'loop', b'#', b';', b'\xe4\xbd\xa0', b'\xff', b'[*]', b'state ', b'<<fork>>', b'--', b'note left of ', b'end note', b'class ', b'<|--', b'*--', b'o--', b'..>', b'()--', b'~T~', b'namespace N {', b'||--o{', b'}|..|{', b' to ', b'PK, FK', b'"x" : ', b'showData', b'create participant ', b'destroy ', b'box Aqua G', b'par_over']
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
