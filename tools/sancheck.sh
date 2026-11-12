#!/bin/sh
# usage: [CC=clang-23] tools/sancheck.sh   (MSan needs clang; default CC=clang)
# Golden tests under ASan+UBSan; allocation-failure injection (make oom); the locale and writer tests under ASan+UBSan and MemorySanitizer; every
# corpus file through an MSan build of the CLI. MSan needs every object instrumented: own build-msan.
set -u
cd "$(dirname "$0")/.." || exit 1
CORPUS="tests/cases/*.mmd tests/docs/*/*.mmd"
CCARG=CC=${CC:-clang}
rc=0
step() { out=$("$@" 2>&1) || { echo "$out" | tail -20; echo "FAIL: $*"; rc=1; return; }; echo "$out" | tail -1; }

make -s $CCARG BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
    LDLIBS="-lm -fsanitize=address,undefined" -j8 all build-asan/test-locale build-asan/test-writer || exit 1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
step ./tests/run_tests.sh build-asan/mmd
step build-asan/test-locale $CORPUS
step build-asan/test-writer 100
step make -s $CCARG -j8 oom

make -s $CCARG BUILD=build-msan OPT="-O1 -g -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer" \
    LDLIBS="-lm -fsanitize=memory" -j8 build-msan/mmd build-msan/test-locale build-msan/test-writer || exit 1
step build-msan/test-locale $CORPUS
step build-msan/test-writer 50
n=0
for f in $CORPUS; do
    if build-msan/mmd "$f" 2>&1 >/dev/null | grep -q MemorySanitizer; then echo "MSAN $f"; n=$((n + 1)); fi
done
echo "msan: $n reports over the corpus"
[ $n = 0 ] || rc=1
exit $rc
