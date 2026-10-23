#!/usr/bin/env python3
"""Compare mmd vs mmdr on the same inputs.
Columns: process wall-clock median (both CLIs, SVG to file), plus in-library time
(mmd: bench harness median; mmdr: its own --timing total_us)."""
import subprocess, sys, time, statistics, os, json, re
MMD = os.environ.get('MMD', 'build/mmd')
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
# library times are often a few microseconds: report them in us (ms with 2dp rounded them to 0.00)
def us(x): return f'{x*1e6:9.1f}' if x is not None else '        -'
files = sys.argv[1:]
print(f"{'input':44} {'mmd lib us':>10} {'mmd cli ms':>10} {'mmdr lib us':>11} {'mmdr cli ms':>11} {'lib x':>7} {'cli x':>7}")
for f in files:
    n = 15
    b = subprocess.run([BENCH, f, '30'], capture_output=True, text=True).stdout
    m = re.search(r'median=([\d.]+)us', b)
    clib = float(m.group(1)) / 1e6 if m else None
    c, _ = wall([MMD, f, '-o', '/tmp/_mmd.svg'], n)
    d, err = wall([MMDR, *EXTRA, '--timing', '-i', f, '-o', '/tmp/_mmdr.svg'], n)
    dlib = None
    if d is not None:
        try: dlib = json.loads(err.strip().splitlines()[-1])['total_us'] / 1e6
        except Exception: pass
    note = '' if d is not None else f'  mmdr: {err}'
    lx = f'{dlib / clib:6.0f}x' if clib and dlib else '     -'
    sp = f'{d / c:6.1f}x' if c and d else '     -'
    print(f'{os.path.basename(f)[:44]:44} {us(clib):>10} {ms(c):>10} {us(dlib):>11} {ms(d):>11} {lx:>7} {sp:>7}{note}', flush=True)
