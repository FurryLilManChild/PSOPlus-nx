#!/bin/sh
# Builds and runs dispatch_cache_test.c on the host with the real function table (build/gen/table.c) and one
# stub per recompiled function (generated here from build/gen/funcs.h; both game-derived, so they stay under
# build/, which git ignores). Usage: tools/tests/run_dispatch_cache_test.sh [lookups] [seed]
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
{
    echo '#include "ppc.h"'
    sed -n -E 's/^void (f_([0-9A-F]{8})(_orig)?)\(Cpu\* __restrict c\);$/void \1(Cpu* c) { c->r[0] = 0x\2u; }/p' build/gen/funcs.h
} > $out/dispatch_stubs.c
CC=${CC:-cc}
FLAGS="-std=gnu11 -O2 -Wall -Iruntime/include -Iruntime/src -Ibuild/gen"
$CC $FLAGS -c $out/dispatch_stubs.c -o $out/dispatch_stubs.o
$CC $FLAGS -w -c build/gen/table.c -o $out/dispatch_table.o
$CC $FLAGS -c runtime/src/dispatch.c -o $out/dispatch.o
$CC $FLAGS -c tools/tests/dispatch_cache_test.c -o $out/dispatch_cache_test.o
$CC $out/dispatch_cache_test.o $out/dispatch.o $out/dispatch_table.o $out/dispatch_stubs.o -lpthread -o $out/dispatch_cache_test
$out/dispatch_cache_test "$@"
