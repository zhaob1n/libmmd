#!/bin/sh
# usage: tools/render_png.sh in.mmd out.png [mmdc-c binary]  -> headless-chrome screenshot at the SVG size
b=${3:-build/mmdc-c}; T=$(mktemp -d /tmp/cmm-png.XXXXXX) || exit 1
trap 'rm -rf "$T"' EXIT
$b "$1" -o "$T/r.svg" || exit 1
set -- "$1" "$2" $(grep -o 'width="[0-9.]*" height="[0-9.]*"' "$T/r.svg" | head -1 | tr -dc '0-9. ' | awk '{print int($1)+2, int($2)+2}')
TMPDIR=$T google-chrome --headless --no-sandbox --disable-gpu --hide-scrollbars --window-size=$3,$4 --screenshot="$(realpath -m "$2")" "file://$T/r.svg" >/dev/null 2>&1
