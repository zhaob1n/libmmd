import subprocess,sys,os
from PIL import Image
os.makedirs('samples/sbs',exist_ok=True)
for name,case in [a.split('=') for a in sys.argv[1:]]:
    src=f'tests/cases/{case}.mmd'
    subprocess.run(['build/mmdc-c',src,'-o','/tmp/sb_c.svg'],check=True)
    import re;hd=open('/tmp/sb_c.svg').read(600);w,h=[int(float(x))+2 for x in re.search(r'width="([\d.]+)" height="([\d.]+)"',hd).groups()]
    subprocess.run(['google-chrome','--headless','--no-sandbox','--disable-gpu','--hide-scrollbars',f'--window-size={w},{h}','--screenshot=/tmp/sb_c.png','file:///tmp/sb_c.svg'],check=True,timeout=60,capture_output=True)
    subprocess.run([os.path.expanduser('~/node_modules/.bin/mmdc'),'-p',os.path.expanduser('~/pptr.json'),'-i',src,'-o','/tmp/sb_m.png','-b','white'],check=True,timeout=90,capture_output=True)
    a,b=Image.open('/tmp/sb_c.png').convert('RGB'),Image.open('/tmp/sb_m.png').convert('RGB')
    im=Image.new('RGB',(a.width+b.width+20,max(a.height,b.height)),'white');im.paste(a,(0,0));im.paste(b,(a.width+20,0))
    im.save(f'samples/sbs/{name}.png');print(name)
