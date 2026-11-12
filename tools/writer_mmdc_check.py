#!/usr/bin/env python3
"""Cross-check the writer against mermaid.js: build/test-writer --dump writes a corpus of every typed
helper and escape context fed with hostile labels and ids; each file must parse in mermaid (rendered
by env.mermaid_render, as docs_audit.py --mmdc does) and its SVG must show every intended label
(whitespace ignored).
usage: tools/writer_mmdc_check.py [-v] [--seeds=N] [test-writer binary]   (needs mermaid-cli: see tools/env.py)
--seeds=N checks N corpora (each ~400 diagrams, ~12 s) instead of the default one.
Skips (exit 0) when mermaid-cli is not installed."""
import html, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import env
args = env.at_root(sys.argv[1:])
sys.stdout.reconfigure(errors="replace")
verbose = '-v' in args
exe = next((a for a in args if not a.startswith('-')), 'build/test-writer')
seeds = int(next((a[8:] for a in args if a.startswith('--seeds=')), 1))
try: env.mmdc()
except SystemExit: print('writer_mmdc_check: mermaid-cli not found, skipped'); sys.exit(0)

# mermaid limits the writer can't escape around (docs/dev/writer.md): these slots are not checked
SKIP = {
    'zenuml': 'the zenuml plugin prints entities literally',
    'C4 ': 'mermaid prints entities literally',
    'architecture ': 'mermaid prints entities literally, and lexes only a few characters',
    'wardley component': 'the lexer takes no entity and few characters',
    'eventmodeling name': 'ASCII names starting with a letter only',
    'gitGraph branch': 'ASCII names only (mmd_git_* check that)',
    'xychart series': 'mermaid leaves out some series names (\"::x::\", after a category it escapes)',
}
# ... and labels mermaid turns into markup whatever the escape: such a diagram only has to parse (the
# markup can move the other labels too: xychart drops its series names after a "<br>" category)
DRAWN = ('<br>', 'fa:fa-')

def lenient(raw):
    """mermaid hyphenates long sequence and data labels, at times inside an escape it then leaves
    undecoded; it also shows '<' '>' as &lt; &gt; in gitGraph and xychart"""
    raw = raw.replace('-', '').replace('&amp;gt;', '>').replace('&amp;lt;', '<')
    raw = re.sub(r'\ufb02\u00b0\u00b0(\d+)(?:\u00b6\u00df|;)', lambda m: chr(int(m[1])), raw)
    return re.sub(r'\s+', '', html.unescape(html.unescape(raw.replace('\u00b6\u00df', ''))))

def raw_text(svg):
    svg = re.sub(r'<style[^>]*>.*?</style>', '', svg, flags=re.S)
    return re.sub(r'\s+', '', re.sub(r'<[^>]+>', '', svg))

with tempfile.TemporaryDirectory(prefix='mmd-wcheck.') as d:
    for k in range(seeds):
        os.makedirs(f'{d}/{k}')
        r = subprocess.run([exe, '--dump', f'{d}/{k}', str(k)], capture_output=True, text=True)
        if r.returncode: sys.exit('writer_mmdc_check: test-writer --dump failed\n' + r.stdout[-2000:])
    files = sorted(f'{d}/{k}/{f}' for k in range(seeds) for f in os.listdir(f'{d}/{k}') if f.endswith('.mmd'))
    res = env.mermaid_render(files)
    rejected, missing, skipped, wrapped = {}, {}, 0, 0
    for o in res.values():
        want = open(o['file'][:-4] + '.want').read().splitlines()
        name, want = want[0][2:], want[1:]
        if any(m in w for w in want for m in DRAWN): want = []
        if name == 'xychart helpers': want = [w for w in want if not w.endswith('Bar')]  # as 'xychart series'
        # markdown (and state's <br> handling): a decoded '\\' escapes what follows
        md = name.startswith(('class note', 'mindmap', 'ER', 'state'))
        md_id = name in ('state id', 'mindmap id')  # bare ids are markdown and can't be escaped
        if any(name.startswith(k) for k in SKIP):
            skipped += 1
            continue
        src = open(o['file']).read()
        if not o['ok']:
            rejected.setdefault(name, []).append((src, o['error']))
            continue
        raw = raw_text(o['svg'])
        got = re.sub(r'\s+', '', html.unescape(raw))  # &nbsp; too
        lost = [w for w in want if w not in got and w.replace('-', '') not in lenient(raw)
                and not (md and w.replace('\\', '') in got)
                and not (md_id and re.sub('[_*]', '', w) in re.sub('[_*]', '', got))]
        # mermaid hyphenated inside an escape ("&#5-8;", its placeholder "\ufb02\u00b0\u00b058\u00b6\u00df"): its bug
        if lost and re.search('[\x00-\x08\x0b-\x1f\ufb02\u00b0\u00b6]|#\\w*-|-;', raw):
            wrapped += 1
            continue
        if lost: missing.setdefault(name, []).append((src, f'{lost} in {got[:300]!r}' if verbose else lost))
    for kind, dct in (('REJECTED', rejected), ('MISSING', missing)):
        for name, xs in sorted(dct.items()):
            print(f'{kind} {name}: {len(xs)}')
            for src, why in xs[: 3 if verbose else 1]:
                print('   ', why, '\n   ', src.replace('\n', '\n    ').rstrip())
    nbad = sum(map(len, rejected.values())) + sum(map(len, missing.values()))
    print(f'writer vs mermaid.js: {len(files) - skipped} diagrams ({skipped} in mermaid-limited slots skipped), '
          f'{nbad} rejected or misrendered' + (f', {wrapped} garbled by mermaid\'s hyphenation' if wrapped else ''))
    sys.exit(nbad != 0)
