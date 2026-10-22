import random
for s in range(400):
    R=random.Random(s); n=R.randint(2,40); L=[]
    d=R.choice(['TD','LR','BT','RL']); L.append(f'flowchart {d}')
    ids=[f'n{i}' for i in range(n)]
    for i in ids: L.append(f'  {i}[{"x"*R.randint(1,12)} {i}]')
    ns=R.randint(0,4); used=set()
    for k in range(ns):
        mem=[x for x in R.sample(ids,R.randint(1,min(5,n))) if x not in used]; used|=set(mem)
        if mem: L.append(f'  subgraph s{k}'); L+= ['    '+m for m in mem]; L.append('  end')
    for e in range(R.randint(1,n*2)):
        a,b=R.choice(ids),R.choice(ids+['s0'] if ns and R.random()<.05 else ids)
        lab=f'|l{e}|' if R.random()<.3 else ''
        L.append(f'  {a} -->{lab} {b}')
    open(f'in/f{s:03d}.mmd','w').write('\n'.join(L)+'\n')
