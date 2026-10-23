#!/bin/bash
# Byte-identity check. usage: bench/eq/cmp.sh BIN   (REF=1 bench/eq/cmp.sh BIN rewrites ref.sha)
D=$(cd "$(dirname "$0")" && pwd); R=$(cd "$D/../.." && pwd); B=${1:-$R/build/mmd}
cd "$R"; tmp=$(mktemp)
for f in bench/eq/in/*.mmd bench/mmdr_fixtures/*.mmd bench/inputs/*.mmd tests/cases/*.mmd; do
  printf '%s %s\n' "$($B $f 2>/dev/null | sha256sum | cut -c1-16)" "$f"; done > $tmp
if [ -n "$REF" ]; then mv $tmp $D/ref.sha; echo "ref written: $(wc -l < $D/ref.sha)"; exit 0; fi
diff $D/ref.sha $tmp | grep '^>' | sed 's/^> [^ ]* /DIFF /'; bad=$(diff $D/ref.sha $tmp | grep -c '^>'); rm $tmp; echo bad=$bad
