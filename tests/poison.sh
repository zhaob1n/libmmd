#!/bin/sh
# Renders every case with the ASan build and malloc'd memory filled with a poison byte, then compares with the
# normal build: catches code that relies on arena memory being zero (fresh pages are, reused arenas are not).
# 0xA5 reads as tiny negative doubles, which max()/min() often absorb; 0x41 reads as ~2.3e6, which they don't.
# Needs build/mmd and build-asan/mmd (make; make asan).
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
fail=0
for f in tests/cases/*.mmd tests/docs/*/*.mmd; do
    build/mmd "$f" -o "$T/n.svg" 2>/dev/null
    for b in 165 65; do
        ASAN_OPTIONS=max_malloc_fill_size=1000000000:malloc_fill_byte=$b build-asan/mmd "$f" -o "$T/p.svg" >"$T/p.err" 2>&1
        if ! cmp -s "$T/n.svg" "$T/p.svg" || grep -q "runtime error\|ERROR: AddressSanitizer" "$T/p.err"; then
            echo "POISON FAIL $f (fill $b)"; fail=1
        fi
    done
done
[ $fail = 0 ] && echo "poison: ok"
exit $fail
