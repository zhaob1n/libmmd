function R(e){const b=e.getBoundingClientRect();return [b.left,b.top,b.right,b.bottom];}
function inter(a,b,i){return Math.min(a[2],b[2])-Math.max(a[0],b[0])>i&&Math.min(a[3],b[3])-Math.max(a[1],b[1])>i;}
function inside(p,b,i){return p[0]>b[0]+i&&p[0]<b[2]-i&&p[1]>b[1]+i&&p[1]<b[3]-i;}
// text as a screen-space quad (rotation-aware), shrunk by i px toward its centre
function Q(e,i){let b;try{b=e.getBBox();}catch(x){return null;} const m=e.getScreenCTM(); if(!m) return null;
  const c=[[b.x,b.y],[b.x+b.width,b.y],[b.x+b.width,b.y+b.height],[b.x,b.y+b.height]].map(([x,y])=>[m.a*x+m.c*y+m.e,m.b*x+m.d*y+m.f]);
  const cx=(c[0][0]+c[2][0])/2, cy=(c[0][1]+c[2][1])/2;
  return c.map(([x,y])=>{const dx=x-cx,dy=y-cy,l=Math.hypot(dx,dy)||1,k=Math.max(0,l-i*1.414)/l;return [cx+dx*k,cy+dy*k];});}
function rotq(e){const m=e.getScreenCTM();return !!m&&(Math.abs(m.b)>1e-3||Math.abs(m.c)>1e-3);}
function sat(A,B){for(const P of [A,B])for(let k=0;k<4;k++){const p=P[k],q=P[(k+1)%4],nx=q[1]-p[1],ny=p[0]-q[0];
  let a0=1e18,a1=-1e18,b0=1e18,b1=-1e18;for(const v of A){const d=v[0]*nx+v[1]*ny;a0=Math.min(a0,d);a1=Math.max(a1,d);}
  for(const v of B){const d=v[0]*nx+v[1]*ny;b0=Math.min(b0,d);b1=Math.max(b1,d);} if(a1<=b0||b1<=a0) return false;} return true;}
function inq(p,A){let s=0;for(let k=0;k<4;k++){const a=A[k],b=A[(k+1)%4],c=(b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0]);if(c===0)return false;s+=c>0?1:-1;}return Math.abs(s)===4;}
const EDGE=/edge|link|relation|transition|message|arrow|flowchart-link|branch|commit-line|messageLine|loopLine|er relationshipLine|path|(^|\s)ae(\s|$)/i;
const out=[];
for(const wrap of document.querySelectorAll('div[data-f]')){
  const f=wrap.dataset.f, svg=wrap.querySelector('svg'); if(!svg) continue;
  const res={f,ll:[],le:[],ov:[],ct:[],cl:[],en:[]};
  let SB=R(svg); {const vb=svg.viewBox&&svg.viewBox.baseVal, m=svg.getScreenCTM(); if(vb&&vb.width&&m) SB=[m.a*vb.x+m.e, m.d*vb.y+m.f, m.a*(vb.x+vb.width)+m.e, m.d*(vb.y+vb.height)+m.f];}
  const texts=[];
  for(const t of svg.querySelectorAll('text')){
    if(t.closest('defs,marker')) continue;
    const s=t.textContent.trim(); if(!s) continue;
    const b=R(t); if(b[2]-b[0]<1) continue;
    texts.push({t,s,b,rot:rotq(t),q:Q(t,2),anc:!!t.closest('.wardley-node--anchor'),cl:!!t.closest('.cluster,.cluster-label,[class*=cluster]')});
  }
  // also html labels
  for(const t of svg.querySelectorAll('foreignObject')){const s=t.textContent.trim(); if(s) texts.push({t,s,b:R(t.querySelector('span,div,p')||t),cl:!!t.closest('.cluster,.cluster-label')});}
  for(let i=0;i<texts.length;i++)for(let j=i+1;j<texts.length;j++){
    const a=texts[i],c=texts[j]; if(a.t.contains(c.t)||c.t.contains(a.t)) continue;
    if((a.rot||c.rot)&&a.q&&c.q?sat(a.q,c.q):inter(a.b,c.b,2)) res.ll.push([a.s.slice(0,18),c.s.slice(0,18)]);
  }
  const edges=[];
  for(const p of svg.querySelectorAll('path,line,polyline')){
    if(p.closest('defs,marker,.node,.cluster,[class*=node]')) continue;
    const cs=getComputedStyle(p); if(cs.fill!=='none'&&p.tagName==='path') continue;
    if(cs.stroke==='none'||cs.visibility==='hidden'||cs.display==='none') continue;
    const cls=(p.getAttribute('class')||'')+' '+(p.parentNode.getAttribute&&p.parentNode.getAttribute('class')||'');
    if(!EDGE.test(cls)) continue;
    let L=0; try{L=p.getTotalLength();}catch(e){continue;}
    if(L<5) continue;
    const m=p.getScreenCTM(); const pts=[]; const n=Math.min(400,Math.ceil(L/3));
    for(let k=0;k<=n;k++){const q=p.getPointAtLength(L*k/n);pts.push([m.a*q.x+m.c*q.y+m.e,m.b*q.x+m.d*q.y+m.f]);}
    edges.push({p,pts,id:p.id||cls.trim().slice(0,30)});
  }
  for(const t of texts){
    // edge label backgrounds: an opaque rect sibling behind the text
    let bg=false; const g=t.t.parentNode;
    for(const r of (g.parentNode||g).querySelectorAll('rect')){const rb=R(r);if(rb[0]<=t.b[0]+1&&rb[2]>=t.b[2]-1&&rb[1]<=t.b[1]+1&&rb[3]>=t.b[3]-1&&getComputedStyle(r).fill!=='none'&&(rb[2]-rb[0])<(t.b[2]-t.b[0])+60){bg=true;break;}}
    for(const e of edges){
      if(t.anc) continue; // wardley anchors sit centred on their own links by design
      const h=e.pts.filter(p=>t.rot&&t.q?inq(p,t.q):inside(p,t.b,2)).length;
      if(h>=2){ if(t.cl) res.ct.push([t.s.slice(0,18),e.id]); else if(!bg) res.le.push([t.s.slice(0,18),e.id]); }
    }
  }
  // overflow: text vs shapes of its node group
  for(const t of texts){
    const g=t.t.closest('.node,[class*=node],.actor,.task,.section,.classGroup,.er.entityBox');
    if(!g||t.cl) continue;
    let best=null;
    for(const s of g.querySelectorAll('rect,polygon,circle,ellipse,path')){
      if(s.closest('defs,marker,.label')) continue;
      const cs=getComputedStyle(s); if(cs.fill==='none'&&cs.stroke==='none') continue;
      const b=R(s); const cx=(t.b[0]+t.b[2])/2, cy=(t.b[1]+t.b[3])/2;
      if(!(cx>b[0]&&cx<b[2]&&cy>b[1]&&cy<b[3])) continue;
      if(!best||(b[2]-b[0])*(b[3]-b[1])>(best[2]-best[0])*(best[3]-best[1])) best=b;
    }
    if(best&&(t.b[0]<best[0]-2||t.b[2]>best[2]+2||t.b[1]<best[1]-3||t.b[3]>best[3]+3)) res.ov.push([t.s.slice(0,18),Math.round(Math.max(best[0]-t.b[0],t.b[2]-best[2],best[1]-t.b[1],t.b[3]-best[3]))]);
  }
  // clipping: text outside the svg viewport
  for(const t of texts) if(t.b[0]<SB[0]-1||t.b[1]<SB[1]-1||t.b[2]>SB[2]+1||t.b[3]>SB[3]+1) res.cl.push([t.s.slice(0,18),Math.round(Math.max(SB[0]-t.b[0],SB[1]-t.b[1],t.b[2]-SB[2],t.b[3]-SB[3]))]);
  // edge through a node that is not one of its ends
  const nodes=[...svg.querySelectorAll('.node,[class*=em-frame],.classGroup,.er.entityBox,.architecture-service')].map(g=>({g,b:R(g)})).filter(n=>n.b[2]-n.b[0]>8&&n.b[3]-n.b[1]>8);
  for(const e of edges){const a=e.pts[0],z=e.pts[e.pts.length-1];
    for(const n of nodes){const B=n.b; const near=p=>p[0]>B[0]-10&&p[0]<B[2]+10&&p[1]>B[1]-10&&p[1]<B[3]+10;
      if(near(a)||near(z)||n.g.contains(e.p)) continue;
      if(e.pts.filter(p=>inside(p,B,4)).length>=3){res.en.push([e.id,(n.g.textContent||'').trim().slice(0,18)]);}}}
  for(const k of ['ll','le','ov','ct','cl','en']) if(res[k].length){out.push(res);break;}
}
document.getElementById('out').textContent=JSON.stringify(out);
