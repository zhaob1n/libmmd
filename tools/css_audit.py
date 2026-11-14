#!/usr/bin/env python3
"""Every CSS rule libmmd can write must match something: render the test cases and the docs corpus,
collect the classes each diagram type puts on its elements, and report each class selector, in a
renderer's own <style> or in src/d_cfg.c's themeVariables table (TVS), that no output of its type
carries. Such a rule is dead weight, or a theme variable that silently has no effect.
Usage: css_audit.py [mmd binary]; exits 1 on any dead selector."""
import os, re, sys, glob, subprocess, collections, concurrent.futures as cf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import env
sys.argv[1:] = env.at_root(sys.argv[1:])
exe = sys.argv[1] if len(sys.argv) > 1 else 'build/mmd'
files = sorted(glob.glob('tests/cases/*.mmd') + glob.glob('tests/docs/*/*.mmd'))
run_env = dict(os.environ, MMD_TODAY='2014-01-08')  # inside the corpus' gantt charts: today markers drawn

def render(f):
    return subprocess.run([exe, f], capture_output=True, text=True, env=run_env, timeout=20).stdout

classes, styles = collections.defaultdict(set), collections.defaultdict(set)
with cf.ThreadPoolExecutor(8) as ex:
    for svg in ex.map(render, files):
        m = re.search(r'<svg[^>]*\bclass="([^"]*)"', svg)
        if not m: continue
        kind = m.group(1)
        for c in re.findall(r'\bclass="([^"]*)"', svg[m.end():]): classes[kind].update(c.split())
        for css in re.findall(r'<style>(.*?)</style>', svg, re.S): styles[kind].add(css)

def selectors(css):  # each '#mermaid ...' selector of each rule
    for rule in re.findall(r'([^{}]+)\{[^}]*\}', css):
        for s in rule.split(','):
            if s.strip().startswith('#mermaid'): yield s.strip()

src = open('src/d_cfg.c').read()
tvs = src[src.index('static const TV TVS[]'):]
tvs = tvs[:tvs.index('\n};')]
dead = set()
def check(kind, sel, where):
    kinds = list(classes) if kind == '*' else [kind]
    miss = [c for c in re.findall(r'\.([\w-]+)', sel) if not any(c in classes[k] for k in kinds)]
    if miss: dead.add(f'{where:7} {kind:18} {sel:50} no element has .{", .".join(miss)}')
for kind, var, tpl in re.findall(r'\{"([^"]+)", "([^"]+)", "((?:[^"\\]|\\.)*)"\}', tvs):
    if kind != '*' and kind not in classes: dead.add(f'TVS     {kind:18} {var}: no output of this type'); continue
    for sel in selectors(tpl.replace('$', 'x')): check(kind, sel, 'TVS')
for kind, csss in styles.items():
    for css in csss:
        for sel in selectors(css): check(kind, sel, 'style')
for d in sorted(dead): print(d)
print(f'css audit: {len(files)} files, {sum(map(len, classes.values()))} classes, {len(dead)} dead selectors')
sys.exit(1 if dead else 0)
