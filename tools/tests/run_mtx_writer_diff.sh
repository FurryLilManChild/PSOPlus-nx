#!/bin/sh
# Builds and runs mtx_writer_diff.c on the host. Extracts the two recompiled originals from build/gen
# (game-derived, so they stay under build/, which git ignores). Usage: tools/tests/run_mtx_writer_diff.sh [cases] [seed]
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
src=$(grep -l '^void f_8038EA3C_orig(' build/gen/code_*.c)
{
    echo '#include "funcs.h"'
    awk '/^void f_8038EA3C_orig\(/,/^}/' "$src"
    awk '/^void f_8038EA70_orig\(/,/^}/' "$src"
} > $out/mtx_orig.c
CC=${CC:-clang}  # the generated code uses musttail (clang, or GCC 15+)
FLAGS="-std=gnu11 -O2 -Wall -Iruntime/include -Iruntime/src -Ibuild/gen"
$CC $FLAGS -c $out/mtx_orig.c -o $out/mtx_orig.o
$CC $FLAGS -c runtime/src/hle_gx.c -o $out/hle_gx.o
$CC $FLAGS -c runtime/src/memory.c -o $out/memory.o
$CC $FLAGS -c tools/tests/mtx_writer_diff.c -o $out/mtx_writer_diff.o
$CC $out/mtx_writer_diff.o $out/mtx_orig.o $out/hle_gx.o $out/memory.o -Wl,--wrap=f_8038EA3C_orig -Wl,--wrap=f_8038EA70_orig -lm -o $out/mtx_writer_diff
$out/mtx_writer_diff "$@"
