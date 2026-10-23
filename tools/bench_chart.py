#!/usr/bin/env python3
"""Regenerate docs/bench.svg and the README CLI table from bench/cli_times.csv.
usage: python3 tools/bench_chart.py   (PNG copy: cairosvg docs/bench.svg -o docs/bench.png)"""
import csv, math, re
from html import escape
rows = [r for r in csv.DictReader(l for l in open('bench/cli_times.csv') if not l.startswith('#'))]
TOOLS = [('libmmd', '#1f77b4'), ('mmdr', '#ff7f0e'), ('mermaid-cli', '#d62728')]
def fmt(ms): return f'{ms:.2f} ms' if ms < 10 else f'{ms:.1f} ms' if ms < 100 else f'{ms:.0f} ms'
def sp(a, b): x = b / a; return f'{x:.0f}×' if x >= 10 else f'{x:.1f}×'
# chart
L, R, T, BH, GAP = 210, 140, 50, 14, 16
W = 900; PW = W - L - R; lo, hi = math.log10(0.3), math.log10(5000)
X = lambda ms: L + (math.log10(ms) - lo) / (hi - lo) * PW
H = T + len(rows) * (3 * BH + GAP) + 50
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Helvetica,Arial,sans-serif" font-size="12">',
     f'<rect width="{W}" height="{H}" fill="white"/>',
     f'<text x="{W/2}" y="20" text-anchor="middle" font-size="15" font-weight="bold" fill="#222">CLI time per diagram (log scale, lower is better)</text>']
for k, (t, c) in enumerate(TOOLS):
    o.append(f'<rect x="{L + k*130}" y="30" width="12" height="12" fill="{c}"/><text x="{L + k*130 + 17}" y="40" fill="#222">{t}</text>')
y0 = T + 6; yb = H - 40
for e in range(0, 4):
    for m in (1, 2, 5):
        v = m * 10 ** (e - 0)
        if v / 1 > 5000: continue
        x = X(v); maj = m == 1
        o.append(f'<line x1="{x:.1f}" y1="{y0}" x2="{x:.1f}" y2="{yb}" stroke="{"#bbb" if maj else "#e6e6e6"}"/>')
        o.append(f'<text x="{x:.1f}" y="{yb+15}" text-anchor="middle" fill="#444">{f'{v/1000:g} s' if v >= 1000 else f'{v:g} ms'}</text>' if maj else '')
for v in (0.5,):
    o.append(f'<line x1="{X(v):.1f}" y1="{y0}" x2="{X(v):.1f}" y2="{yb}" stroke="#e6e6e6"/>')
y = y0
for r in rows:
    o.append(f'<text x="{L-8}" y="{y + 1.5*BH + 4}" text-anchor="end" fill="#222">{escape(r["input"])}</text>')
    a = float(r['libmmd'])
    for k, (t, c) in enumerate(TOOLS):
        v = float(r[t]); x = X(v); yy = y + k * BH
        o.append(f'<rect x="{L}" y="{yy+1}" width="{x-L:.1f}" height="{BH-2}" fill="{c}"/>')
        lab = fmt(v) + ('' if k == 0 else f'  ({sp(a, v)} slower)')
        o.append(f'<text x="{x+4:.1f}" y="{yy+BH-3}" font-size="11" fill="#222">{lab}</text>')
    y += 3 * BH + GAP
o.append(f'<text x="{L + PW/2}" y="{H-8}" text-anchor="middle" fill="#444">wall-clock per process run, best of N; 8-vCPU Xeon VM</text></svg>')
open('docs/bench.svg', 'w').write('\n'.join(s for s in o if s) + '\n')
# README table between markers
tab = ['| input | libmmd | mmdr 0.3.1 | mermaid-cli 11.17 |', '|---|---:|---:|---:|']
for r in rows:
    a = float(r['libmmd'])
    tab.append(f"| {r['input']} ({r['source']}) | {fmt(a)} | {fmt(float(r['mmdr']))} ({sp(a, float(r['mmdr']))}) | {fmt(float(r['mermaid-cli']))} ({sp(a, float(r['mermaid-cli']))}) |")
s = open('README.md').read()
s = re.sub(r'(<!-- bench-table -->)\n.*?(<!-- /bench-table -->)', lambda m: m[1] + '\n' + '\n'.join(tab) + '\n' + m[2], s, flags=re.S)
open('README.md', 'w').write(s)
