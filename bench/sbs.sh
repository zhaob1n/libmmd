#!/bin/bash
# Flowchart side-by-sides vs official mermaid-cli (MMDC, default mmdc on PATH; MMDC_PUPPETEER: its -p config).
# usage: bench/sbs.sh [outdir=$TMPDIR/sbsf] [ref]   ('ref' forces re-rendering mermaid-cli PNGs)
# Both sides are rasterised by Chrome so fonts match; left = libmmd, right = mermaid-cli.
set -u
O=$(realpath -m "${1:-${TMPDIR:-/tmp}/sbsf}"); mkdir -p $O/ref $O/me $O/out
cd "$(dirname "$0")/.."
MMDC=${MMDC:-mmdc}; CHROME=${CHROME:-google-chrome}
command -v "$MMDC" >/dev/null || { echo "mermaid-cli not found: set MMDC" >&2; exit 1; }
echo '{"flowchart":{"useMaxWidth":false}}' > $O/cfg.json
for f in bench/mmdr_fixtures/flowchart*.mmd; do n=$(basename $f .mmd)
  if [ "${2:-}" = ref ] || [ ! -f $O/ref/$n.png ]; then
    "$MMDC" ${MMDC_PUPPETEER:+-p "$MMDC_PUPPETEER"} -c $O/cfg.json -b white -i $f -o $O/ref/$n.png >/dev/null 2>&1 || echo "ref fail $n"; fi
  build/mmd $f > $O/me/$n.svg || { echo "me fail $n"; continue; }
  read W H < <(python3 -c "import re,math,sys;s=open(sys.argv[1]).read(600);print(*[math.ceil(float(re.search(k+r'=\"([\d.]+)',s).group(1))) for k in ('width','height')])" $O/me/$n.svg)
  timeout 60 "$CHROME" --headless --no-sandbox --hide-scrollbars --default-background-color=ffffffff \
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
    d=ImageDraw.Draw(out); d.text((4,4),'libmmd',fill='red'); d.text((a.width+24,4),'mermaid-cli 11',fill='red')
    out.save(O+'/out/'+n)
P
