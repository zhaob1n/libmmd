#!/usr/bin/env python3
"""Render .mmd files with mmdc-c, load the SVGs in headless chrome and report geometric defects
(ll label/label overlap, le label/edge overlap, ct cluster-title hits, ...) via quality_scan.js.
usage: tools/quality_scan.py [files...]   (default: tests/docs, tests/cases, bench/mmdr_fixtures)
env BIN=path/to/mmdc-c"""
import sys,subprocess,os,json,tempfile,html,re,glob
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN=os.environ.get('BIN',os.path.join(ROOT,'build/mmdc-c'))
files=sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT,'tests/docs/**/*.mmd'),recursive=True)+glob.glob(os.path.join(ROOT,'tests/cases/*.mmd'))+glob.glob(os.path.join(ROOT,'bench/mmdr_fixtures/*.mmd')))
js=open(os.path.join(ROOT,'tools/quality_scan.js')).read()
allr=[]
for i in range(0,len(files),40):
    parts=[]
    for f in files[i:i+40]:
        r=subprocess.run([BIN,f,'-o','-'],capture_output=True,stdin=subprocess.DEVNULL)
        if r.returncode: continue
        s=r.stdout.decode('utf8','replace'); s=re.sub(r'<\?xml[^>]*>','',s)
        m0=re.search(r'<svg[^>]*\sid="([^"]+)"',s)
        if m0:  # page holds many svgs: make the root id (and its #id style selectors) unique
            sid=m0.group(1); u=f'{sid}-q{len(parts)}'
            s=s.replace(f'id="{sid}"',f'id="{u}"').replace('#'+sid,'#'+u)
        parts.append(f'<div data-f="{html.escape(f)}" style="position:relative">{s}</div>')
    T=tempfile.mkdtemp(prefix='cmm-scan-')
    p=os.path.join(T,'p.html')
    open(p,'w').write('<html><body style="margin:0">'+''.join(parts)+'<pre id="out"></pre><script>'+js+'</script></body></html>')
    r=subprocess.run(['google-chrome','--headless','--no-sandbox','--disable-gpu','--virtual-time-budget=5000','--dump-dom','file://'+p],capture_output=True,env=dict(os.environ,TMPDIR=T),timeout=300)
    m=re.search(r'<pre id="out">(.*?)</pre>',r.stdout.decode(),re.S)
    subprocess.run(['rm','-rf',T])
    if m and m.group(1): allr+=json.loads(html.unescape(m.group(1)))
    else: print('batch fail',i,file=sys.stderr)
for r in allr:
    f=os.path.relpath(r['f'],ROOT)
    print(f, {k:(len(v),v[:3]) for k,v in r.items() if k!='f' and v})
