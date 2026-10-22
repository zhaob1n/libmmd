#!/bin/bash
# Flowchart side-by-sides vs official mermaid-cli (~/node_modules/.bin/mmdc, puppeteer cfg ~/pptr.json).
# usage: bench/sbs.sh [outdir=/tmp/sbsf] [ref]   ('ref' forces re-rendering mermaid-cli PNGs)
# Both sides are rasterised by Chrome so fonts match; left = cmermaid, right = mermaid-cli.
set -u
O=${1:-/tmp/sbsf}; mkdir -p $O/ref $O/me $O/out
cd "$(dirname "$0")/.."
echo '{"flowchart":{"useMaxWidth":false}}' > $O/cfg.json
for f in bench/mmdr_fixtures/flowchart*.mmd; do n=$(basename $f .mmd)
  if [ "${2:-}" = ref ] || [ ! -f $O/ref/$n.png ]; then
    ~/node_modules/.bin/mmdc -p ~/pptr.json -c $O/cfg.json -b white -i $f -o $O/ref/$n.png >/dev/null 2>&1 || echo "ref fail $n"; fi
  build/mmdc-c $f > $O/me/$n.svg || { echo "me fail $n"; continue; }
  read W H < <(python3 -c "import re,math,sys;s=open(sys.argv[1]).read(600);print(*[math.ceil(float(re.search(k+r'=\"([\d.]+)',s).group(1))) for k in ('width','height')])" $O/me/$n.svg)
  timeout 60 google-chrome --headless --no-sandbox --hide-scrollbars --default-background-color=ffffffff \
    --screenshot=$O/me/$n.png --window-size=$W,$H file://$O/me/$n.svg >/dev/null 2>&1 || echo "png fail $n"
done
python3 - "$O" <<'P'
import glob,os,sys
from PIL import Image, ImageDraw
O=sys.argv[1]
for r in sorted(glob.glob(O+'/ref/*.png')):
    n=os.path.basename(r); m=O+'/me/'+n
    if not os.path.exists(m): continue
    a=Image.open(m).convert('RGB'); b=Image.open(r).convert('RGB')
    out=Image.new('RGB',(a.width+b.width+20,max(a.height,b.height)+24),'white')
    out.paste(a,(0,24)); out.paste(b,(a.width+20,24))
    d=ImageDraw.Draw(out); d.text((4,4),'cmermaid',fill='red'); d.text((a.width+24,4),'mermaid-cli 11',fill='red')
    out.save(O+'/out/'+n)
P
