#!/bin/sh
# Renders every case with the ASan build and malloc'd memory filled with 0xA5, then compares with the normal
# build: catches code that relies on arena memory being zero (fresh pages are, reused arenas are not).
set -u
fail=0
for f in tests/cases/*.mmd; do
    build/mmdc-c "$f" -o /tmp/cmm_n.svg 2>/dev/null
    ASAN_OPTIONS=max_malloc_fill_size=1000000000:malloc_fill_byte=165 build-asan/mmdc-c "$f" -o /tmp/cmm_p.svg >/tmp/cmm_p.err 2>&1
    if ! cmp -s /tmp/cmm_n.svg /tmp/cmm_p.svg || grep -q "runtime error\|ERROR: AddressSanitizer" /tmp/cmm_p.err; then
        echo "POISON FAIL $f"; fail=1
    fi
done
[ $fail = 0 ] && echo "poison: ok"
exit $fail
