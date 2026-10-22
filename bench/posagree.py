import re,sys,glob,os,subprocess,itertools
def pos(s):
    d={}
    for m in re.finditer(r'<g class="node[^"]*" id="(?:my-svg-)?flowchart-(.+?)-\d+"[^>]*?transform="translate\(([-\d.e]+), ?([-\d.e]+)\)',s): d[m.group(1)]=(float(m.group(2)),float(m.group(3)))
    return d
B=sys.argv[1] if len(sys.argv)>1 else '/workspace/cmermaid/build/mmdc-c'
T=[0,0,0,0];rows=[]
for f in sorted(glob.glob('/workspace/sbsf/refsvg/*.svg')):
    n=os.path.basename(f)[:-4]; src=f'/workspace/cmermaid/bench/mmdr_fixtures/{n}.mmd'
    ref=pos(open(f).read()); me=pos(subprocess.run([B,src],capture_output=True,text=True).stdout)
    lr=bool(re.search(r'^\s*(flowchart|graph)\s+(LR|RL)',open(src).read(),re.M)); X=1 if lr else 0
    k=[v for v in ref if v in me]; ok=t=0; okr=0
    sg=lambda a,b:(a>b+1)-(a<b-1)
    for u,v in itertools.combinations(k,2):
        t+=1; ok+= sg(me[u][X],me[v][X])==sg(ref[u][X],ref[v][X]); okr+= sg(me[u][1-X],me[v][1-X])==sg(ref[u][1-X],ref[v][1-X])
    T[0]+=ok;T[1]+=okr;T[2]+=t
    print(f'{n:45s} cross {ok}/{t}  rankaxis {okr}/{t}')
print('TOTAL cross %.4f rankaxis %.4f pairs %d'%(T[0]/T[2],T[1]/T[2],T[2]))
