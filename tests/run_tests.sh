#!/usr/bin/env bash
# Golden/sanity tests: every case renders, output is well-formed XML (xmllint or python), errors report line/col.
set -u
BIN=${1:-build/mmdc-c}
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$DIR/out; mkdir -p "$OUT"
pass=0; fail=0
check_xml() {
  if command -v xmllint >/dev/null; then xmllint --noout "$1"
  else python3 -c "import sys,xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$1"; fi
}
for f in "$DIR"/cases/*.mmd; do
  name=$(basename "$f" .mmd)
  if [[ $name == err* ]]; then
    if msg=$("$BIN" "$f" 2>&1 >/dev/null); then echo "FAIL $name (expected error)"; fail=$((fail+1));
    elif [[ $msg =~ :[0-9]+:[0-9]+: ]]; then echo "ok   $name -> $msg"; pass=$((pass+1));
    else echo "FAIL $name (no line:col): $msg"; fail=$((fail+1)); fi
    continue
  fi
  if "$BIN" "$f" -o "$OUT/$name.svg" && check_xml "$OUT/$name.svg"; then
    if [[ -n ${UPDATE_GOLDEN:-} ]]; then mkdir -p "$DIR/golden"; cp "$OUT/$name.svg" "$DIR/golden/$name.svg"; fi
    if [[ -f $DIR/golden/$name.svg ]] && ! cmp -s "$OUT/$name.svg" "$DIR/golden/$name.svg"; then
      echo "FAIL $name (differs from golden)"; fail=$((fail+1))
    else echo "ok   $name"; pass=$((pass+1)); fi
  else echo "FAIL $name"; fail=$((fail+1)); fi
done
# stdin path
if echo "graph LR; a-->b" | "$BIN" | check_xml /dev/stdin; then echo "ok   stdin"; pass=$((pass+1)); else echo "FAIL stdin"; fail=$((fail+1)); fi
echo "passed=$pass failed=$fail"
[[ $fail == 0 ]]
