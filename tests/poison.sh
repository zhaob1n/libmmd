#!/bin/sh
# Renders every case with the ASan build and malloc'd memory filled with 0xA5, then compares with the normal
# build: catches code that relies on arena memory being zero (fresh pages are, reused arenas are not).
set -u
fail=0
for f in tests/cases/*.mmd; do
    build/mmd "$f" -o /tmp/mmd_n.svg 2>/dev/null
    ASAN_OPTIONS=max_malloc_fill_size=1000000000:malloc_fill_byte=165 build-asan/mmd "$f" -o /tmp/mmd_p.svg >/tmp/mmd_p.err 2>&1
    if ! cmp -s /tmp/mmd_n.svg /tmp/mmd_p.svg || grep -q "runtime error\|ERROR: AddressSanitizer" /tmp/mmd_p.err; then
        echo "POISON FAIL $f"; fail=1
    fi
done
[ $fail = 0 ] && echo "poison: ok"
exit $fail
