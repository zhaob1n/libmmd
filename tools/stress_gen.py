#!/usr/bin/env python3
"""Generate larger random diagrams of each type for stress-testing layout quality.
usage: tools/stress_gen.py OUTDIR [per_type=3] [seed=1] [scale=1]   then: tools/quality_scan.py OUTDIR/*.mmd"""
import sys,random,os
out=sys.argv[1]; N=int(sys.argv[2]) if len(sys.argv)>2 else 3; R=random.Random(int(sys.argv[3]) if len(sys.argv)>3 else 1)
SC=float(sys.argv[4]) if len(sys.argv)>4 else 1.0
_ri=R.randint
R.randint=lambda a,b:_ri(a,max(a,int(b*SC)) if b>=6 else b)  # scale size ranges (upper bounds >= 6)
W=['alpha','beta','gamma','delta','load config','parse input','validate','retry later','send mail','x','long label with words','ok','db','cache miss','user']
w=lambda:R.choice(W)
def flow():
    n=R.randint(12,30); s=[f'flowchart {R.choice(["TD","LR","BT","RL"])}']
    s=[s[0]]+[f'  n{i}["{w()}"]' if R.random()<.6 else f'  n{i}{{"{w()}"}}' for i in range(n)]
    for _ in range(int(n*1.4)):
        a,b=R.randrange(n),R.randrange(n); s.append(f'  n{a} {R.choice(["-->","-.->","==>","---"])}'+(f'|{w()}|' if R.random()<.4 else '')+f' n{b}')
    if R.random()<.7:
        k=R.randint(2,5); s.append('  subgraph S1 [group one]'); s+= [f'    n{i}' for i in range(k)]; s.append('  end')
    return s
def cls():
    n=R.randint(6,14); s=['classDiagram']
    for i in range(n): s+= [f'  class C{i} {{',f'    +{w().replace(" ","_")} int',f'    +m{i}() void','  }']
    for _ in range(n+4): s.append(f'  C{R.randrange(n)} "{R.choice(["1","0..*","many"])}" {R.choice(["<|--","*--","o--","-->","..>"])} "{R.choice(["1","*"])}" C{R.randrange(n)} : {w()}')
    return s
def state():
    n=R.randint(8,18); s=['stateDiagram-v2','  [*] --> s0']
    for _ in range(n*2): s.append(f'  s{R.randrange(n)} --> s{R.randrange(n)}'+(f' : {w()}' if R.random()<.5 else ''))
    s.append(f'  s{n-1} --> [*]'); return s
def er():
    n=R.randint(5,12); s=['erDiagram']
    for _ in range(n+3): s.append(f'  E{R.randrange(n)} {R.choice(["||--o{","}|..|{","|o--||"])} E{R.randrange(n)} : "{w()}"')
    for i in range(n): s+= [f'  E{i} {{','    int id PK',f'    string {w().replace(" ","_")}','  }']
    return s
def seq():
    n=R.randint(3,7); s=['sequenceDiagram']
    for _ in range(25):
        r=R.random(); a,b=R.randrange(n),R.randrange(n)
        if r<.1: s.append(f'  Note over P{a},P{b}: {w()} {w()}')
        elif r<.15: s+= [f'  loop {w()}',f'    P{a}->>P{b}: {w()}','  end']
        else: s.append(f'  P{a}{R.choice(["->>","-->>","-x","-)"])}P{b}: {w()}')
    return s
def block():
    c=R.randint(2,5); s=['block-beta',f'  columns {c}']
    n=R.randint(6,16)
    for i in range(n): s.append(f'  b{i}["{w()}"]'+(f':{R.randint(1,2)}' if R.random()<.2 else ''))
    for _ in range(n//2): s.append(f'  b{R.randrange(n)} -- "{w()}" --> b{R.randrange(n)}' if R.random()<.5 else f'  b{R.randrange(n)} --> b{R.randrange(n)}')
    return s
def mind():
    s=['mindmap','  root((center))']
    def rec(d,ind):
        for _ in range(R.randint(1,4 if d<2 else 2)):
            s.append(' '*ind+w()); 
            if d<3 and R.random()<.6: rec(d+1,ind+2)
    rec(0,4); return s
def gitg():
    s=['gitGraph']; br=['main']; cur='main'
    for i in range(25):
        r=R.random()
        if r<.15: b=f'b{i}'; s.append(f'  branch {b}'); br.append(b); cur=b
        elif r<.3: cur=R.choice(br); s.append(f'  checkout {cur}')
        elif r<.4 and len(br)>1: o=R.choice([x for x in br if x!=cur]); s.append(f'  merge {o}')
        else: s.append(f'  commit id: "{w()} {i}"'+(f' tag: "v{i}"' if R.random()<.15 else ''))
    return s
def kanban():
    s=['kanban']
    for c in range(R.randint(3,5)):
        s.append(f'  col{c}[{w()}]')
        for t in range(R.randint(1,5)): s.append(f'    t{c}_{t}[{w()} {w()} {w()}]'+(f'@{{ assigned: "{w()}", priority: "High" }}' if R.random()<.4 else ''))
    return s
def timeline():
    s=['timeline','  title '+w()+' '+w()]
    for i in range(R.randint(4,9)): s.append(f'  {2000+i} : {w()}'+''.join(f' : {w()}' for _ in range(R.randint(0,3))))
    return s
def pie(): return ['pie title '+w()]+[f'  "{w()} {i}" : {R.randint(1,50)}' for i in range(R.randint(3,10))]
def gantt():
    s=['gantt','  dateFormat YYYY-MM-DD','  title '+w()]
    for i in range(R.randint(2,4)):
        s.append(f'  section {w()}')
        for j in range(R.randint(2,5)): s.append(f'  {w()} {i}{j} :{R.choice(["","milestone, ","crit, "])}2024-0{R.randint(1,6)}-{R.randint(10,28)}, {R.randint(0 if "m" else 1,20)}d')
    return s
def venn():
    s=['venn-beta','  title '+w()]; S=['A','B','C'][:R.randint(2,3)]
    for x in S: s.append(f'  set {x}["{w()}"]')
    s.append(f'  union {",".join(S[:2])}["{w()}"]'); return s
def em():
    s=['eventmodeling']
    for i in range(R.randint(6,14)): s.append(f'tf {i+1:02d} {R.choice(["cmd","evt","rmo","ui"])} {w().title().replace(" ","")}{i}')
    return s
def journey():
    s=['journey','  title '+w()]
    for i in range(3):
        s.append(f'  section {w()}')
        for j in range(R.randint(2,4)): s.append(f'    {w()} {w()}: {R.randint(1,5)}: Me, {w().title()}')
    return s
def quadrant():
    s=['quadrantChart','  title '+w(),'  x-axis Low --> High','  y-axis Low --> High']
    for i in range(R.randint(5,15)): s.append(f'  {w()} {i}: [{R.random():.2f}, {R.random():.2f}]')
    return s
def treemap():
    s=['treemap-beta']
    for i in range(R.randint(2,4)):
        s.append(f'"{w()} {i}"')
        for j in range(R.randint(1,5)): s.append(f'    "{w()} {i}{j}": {R.randint(1,100)}')
    return s
def c4():
    n=R.randint(5,12); s=['C4Context','  title '+w()]
    for i in range(n): s.append(f'  {R.choice(["Person","System","System_Ext","SystemDb"])}(e{i}, "{w()} {i}", "{w()} {w()}")')
    if R.random()<.6: s+= ['  Enterprise_Boundary(b0, "'+w()+'") {']+[f'    System(eb{i}, "{w()}")' for i in range(R.randint(1,3))]+['  }']
    for _ in range(n): s.append(f'  Rel(e{R.randrange(n)}, e{R.randrange(n)}, "{w()}")')
    return s
def wardley():
    n=R.randint(6,14); s=['wardley-beta','title '+w()]; names=[f'{w().title()} {i}' for i in range(n)]
    for x in names: s.append(f'component {x} [{R.uniform(.05,.95):.2f}, {R.uniform(.05,.95):.2f}]')
    for _ in range(n): s.append(f'{R.choice(names)} -> {R.choice(names)}')
    return s
def sankey():
    n=R.randint(5,12); s=['sankey-beta']; names=[f'{w()} {i}' for i in range(n)]
    for i in range(n*2):
        a=R.randrange(n-1); b=R.randrange(a+1,n); s.append(f'"{names[a]}","{names[b]}",{R.uniform(1,100):.1f}')
    return s
def requirement():
    n=R.randint(3,7); s=['requirementDiagram']
    for i in range(n): s+= [f'  requirement r{i} {{',f'    id: {i}',f'    text: {w()} {w()} {w()}','    risk: high','    verifymethod: test','  }']
    for i in range(R.randint(1,3)): s+= [f'  element el{i} {{',f'    type: {w()}','  }']
    for i in range(n+2): s.append(f'  {R.choice([f"r{R.randrange(n)}","el0"])} - {R.choice(["satisfies","traces","contains","derives","verifies"])} -> r{R.randrange(n)}')
    return s
def arch():
    n=R.randint(5,10); s=['architecture-beta','  group g0(cloud)['+w()+']']
    for i in range(n): s.append(f'  service s{i}({R.choice(["database","server","disk","internet","cloud"])})[{w()}]'+(' in g0' if R.random()<.4 else ''))
    D='LRTB'
    for _ in range(n): a,b=R.sample(range(n),2); s.append(f'  s{a}:{R.choice(D)} {R.choice(["--","-->"])} {R.choice(D)}:s{b}')
    return s
G=dict(flow=flow,cls=cls,state=state,er=er,seq=seq,block=block,mind=mind,git=gitg,kanban=kanban,timeline=timeline,pie=pie,gantt=gantt,venn=venn,em=em,journey=journey,quadrant=quadrant,treemap=treemap,c4=c4,wardley=wardley,sankey=sankey,req=requirement,arch=arch)
os.makedirs(out,exist_ok=True)
for k,f in G.items():
    for i in range(N): open(os.path.join(out,f'{k}_{i}.mmd'),'w').write('\n'.join(f())+'\n')
