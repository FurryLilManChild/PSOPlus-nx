#!/bin/sh
# Builds and runs pso_wide_test.c on the host. Extracts the game's own task tree walk (f_8022A87C_orig) from
# build/gen (game-derived, so it stays under build/, which git ignores). Usage: tools/tests/run_pso_wide_test.sh [trees] [seed]
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
# after the hle.txt entry the game's code is f_8022A87C_orig; before it (an older build/gen), f_8022A87C
if src=$(grep -l '^void f_8022A87C_orig(' build/gen/code_*.c 2>/dev/null); then
    { echo '#include "funcs.h"'; awk '/^void f_8022A87C_orig\(/,/^}/' "$src"; } > $out/walk_orig.c
else
    src=$(grep -l '^void f_8022A87C(' build/gen/code_*.c)
    { echo '#include "funcs.h"'; echo 'void f_8022A87C_orig(Cpu* __restrict c);'
      awk '/^void f_8022A87C\(/,/^}/' "$src" | sed '1s/f_8022A87C(/f_8022A87C_orig(/'; } > $out/walk_orig.c
fi
CC=${CC:-clang}  # the generated code uses musttail (clang, or GCC 15+)
FLAGS="-std=gnu11 -O2 -Wall -Iruntime/include -Iruntime/src -Ibuild/gen"
$CC $FLAGS -c $out/walk_orig.c -o $out/walk_orig.o
$CC $FLAGS -c runtime/src/memory.c -o $out/memory.o
$CC $FLAGS -c tools/tests/pso_wide_test.c -o $out/pso_wide_test.o
$CC $out/pso_wide_test.o $out/walk_orig.o $out/memory.o -lm -o $out/pso_wide_test
$out/pso_wide_test "$@"
