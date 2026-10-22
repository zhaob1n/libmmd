#!/usr/bin/env python3
"""Extract ```mermaid / ```mermaid-example blocks from mermaid's docs/syntax/*.md into tests/docs/<doc>/NN.mmd.
usage: python3 tools/extract_docs.py <mermaid-repo>/packages/mermaid/src/docs/syntax  (flowchart blocks skipped)"""
import os, re, sys, glob
src = sys.argv[1]
out = 'tests/docs'
n = 0
for md in sorted(glob.glob(os.path.join(src, '*.md'))):
    doc = os.path.basename(md)[:-3]
    if doc == 'flowchart': continue
    k = 0
    for m in re.finditer(r'^```mermaid(?:-example)?[ \t]*\n(.*?)^```', open(md).read(), re.S | re.M):
        body = m.group(1)
        first = next((l.strip() for l in body.split('\n') if l.strip() and not l.strip().startswith('%%') and l.strip() != '---'), '')
        if re.match(r'(flowchart|graph)\b', first): continue
        k += 1
        os.makedirs(f'{out}/{doc}', exist_ok=True)
        open(f'{out}/{doc}/{k:02d}.mmd', 'w').write(body)
        n += 1
print(n, 'examples')
