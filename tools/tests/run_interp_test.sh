#!/bin/sh
# Builds and runs interp_test.c on the host: the interpreter fallback (runtime/src/interp.c) with stand-ins
# for the rest of the runtime. Needs no game data. Usage: tools/tests/run_interp_test.sh
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
CC=${CC:-cc}
FLAGS="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iruntime/include -Iruntime/src -DINTERP_STEP_LIMIT=1000000ull -DINTERP_DUMP_DIR=\"$out/\""
$CC $FLAGS -c runtime/src/interp.c -o $out/interp.o
$CC $FLAGS -c tools/tests/interp_test.c -o $out/interp_test.o
$CC $out/interp_test.o $out/interp.o -lm -o $out/interp_test
$out/interp_test "$@"
