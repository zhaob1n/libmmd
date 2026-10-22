#!/usr/bin/env python3
"""Compare mmdc-c vs mmdr on the same inputs.
Columns: process wall-clock median (both CLIs, SVG to file), plus in-library time
(mmdc-c: bench harness median; mmdr: its own --timing total_us)."""
import subprocess, sys, time, statistics, os, json, re
CMM = os.environ.get('CMM', 'build/mmdc-c')
BENCH = os.environ.get('BENCH', 'build/bench')
MMDR = os.environ.get('MMDR', '/workspace/mmdr/target/release/mmdr')
TIMEOUT = float(os.environ.get('TIMEOUT', '120'))
EXTRA = os.environ.get('MMDR_ARGS', '').split()
def wall(cmd, n):
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        try:
            p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return None, f'timeout>{TIMEOUT:.0f}s'
        ts.append(time.perf_counter() - t)
        if p.returncode: return None, 'ERR ' + p.stderr.decode()[:80]
        err = p.stderr.decode()
        if ts[-1] > 5: break
    return statistics.median(ts), err
def ms(x): return f'{x*1000:9.2f}' if x is not None else '        -'
files = sys.argv[1:]
print(f"{'input':44} {'cmm lib ms':>10} {'cmm cli ms':>10} {'mmdr lib ms':>11} {'mmdr cli ms':>11} {'cli x':>7}")
for f in files:
    n = 15
    b = subprocess.run([BENCH, f, '30'], capture_output=True, text=True).stdout
    m = re.search(r'median=([\d.]+)us', b)
    clib = float(m.group(1)) / 1e6 if m else None
    c, _ = wall([CMM, f, '-o', '/tmp/_cmm.svg'], n)
    d, err = wall([MMDR, *EXTRA, '--timing', '-i', f, '-o', '/tmp/_mmdr.svg'], n)
    dlib = None
    if d is not None:
        try: dlib = json.loads(err.strip().splitlines()[-1])['total_us'] / 1e6
        except Exception: pass
    note = '' if d is not None else f'  mmdr: {err}'
    sp = f'{d / c:6.1f}x' if c and d else '     -'
    print(f'{os.path.basename(f)[:44]:44} {ms(clib):>10} {ms(c):>10} {ms(dlib):>11} {ms(d):>11} {sp:>7}{note}', flush=True)
