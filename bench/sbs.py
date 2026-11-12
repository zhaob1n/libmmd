"""Side-by-side PNGs of test cases vs mermaid-cli: bench/sbs.py name=case... -> samples/sbs/name.png"""
import subprocess,sys,os,re,tempfile
sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','tools')); import env
env.at_root([])
from PIL import Image
os.makedirs('samples/sbs',exist_ok=True)
T=tempfile.mkdtemp(prefix='mmd-sbs.')
for name,case in [a.split('=') for a in sys.argv[1:]]:
    src=f'tests/cases/{case}.mmd'
    cs,cp,mp=f'{T}/c_{name}.svg',f'{T}/c_{name}.png',f'{T}/m_{name}.png'  # unique names: chrome caches file:// URLs
    subprocess.run(['build/mmd',src,'-o',cs],check=True)
    hd=open(cs).read(600);w,h=[int(float(x))+2 for x in re.search(r'width="([\d.]+)" height="([\d.]+)"',hd).groups()]
    subprocess.run([env.chrome(),'--headless','--no-sandbox','--disable-gpu','--hide-scrollbars',f'--window-size={w},{h}','--screenshot='+cp,'file://'+cs+''],check=True,timeout=60,capture_output=True)
    subprocess.run([*env.mmdc(),'-i',src,'-o',mp,'-b','#333' if 'dark' in name else 'white'],check=True,timeout=90,capture_output=True)
    a,b=Image.open(cp).convert('RGB'),Image.open(mp).convert('RGB')
    im=Image.new('RGB',(a.width+b.width+20,max(a.height,b.height)),'white');im.paste(a,(0,0));im.paste(b,(a.width+20,0))
    im.save(f'samples/sbs/{name}.png');print(name)
    for f in (cs,cp,mp): os.remove(f)
os.rmdir(T)
