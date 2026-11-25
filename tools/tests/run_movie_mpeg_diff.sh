#!/bin/sh
# Builds and runs movie_mpeg_diff.c on the host: the native MPEG-1 slice decoder (runtime/src/movie_mpeg.c)
# against the game's own Sofdec code. Extracts movie.rel's recompiled functions from build/gen (game-derived,
# so they stay under build/, which git ignores). Needs baserom/movie.rel and a movie (.sfd) from the disc.
# Usage: tools/tests/run_movie_mpeg_diff.sh MOVIE.sfd [compare|native|orig] [max_pictures] [out.yuv]
set -e
cd "$(dirname "$0")/../.."
out=build/tests/movie
mkdir -p $out
python3 - <<'PY'
import glob, re
out = ['#include "funcs.h"']
names = []
for f in sorted(glob.glob('build/gen/code_*.c')):
    t = open(f).read()
    for m in re.finditer(r'^void (f_818[0-9A-F]{5}(?:_orig)?)\(Cpu\* __restrict c\) \{.*?^\}\n', t, re.S | re.M):
        out.append(m.group(0))
        names.append(m.group(1))
# the game's maths library (0x803A0000-0x803A2FFF: cos, sin, floor, ...), which movie.rel's table set-up
# calls, comes along: the tables then hold exactly the game's values
bodies = {}
for f in sorted(glob.glob('build/gen/code_*.c')):
    t = open(f).read()
    for m in re.finditer(r'^void (f_803A[0-2][0-9A-F]{3})\(Cpu\* __restrict c\) \{.*?^\}\n', t, re.S | re.M):
        bodies[m.group(1)] = m.group(0)
want = set(re.findall(r'\b(f_803A[0-2][0-9A-F]{3})\b', '\n'.join(out)))
done = set()
while want - done:
    n = sorted(want - done)[0]
    done.add(n)
    if n in bodies:
        out.append(bodies[n])
        want |= set(re.findall(r'\b(f_803A[0-2][0-9A-F]{3})\b', bodies[n]))
src = '\n'.join(out)
def write(path, text):  # unchanged files keep their time stamps (the 1271 functions take a while to compile)
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    open(path, 'w').write(text)
write('build/tests/movie/movie_rel.c', src)
# indirect calls land on function entries: the table of movie.rel's functions
fns = sorted(n for n in names if not n.endswith('_orig'))
tab = ['#include "ppc.h"', 'typedef struct { uint32_t addr; PpcFunc fn; } FnEnt;']
tab += ['void %s(Cpu* c);' % n for n in fns]
tab.append('const FnEnt g_movie_fns[] = {')
tab += ['    {0x%su, %s},' % (n[2:], n) for n in fns]
tab.append('};\nconst unsigned g_movie_fn_count = %d;' % len(fns))
tab.append('const uint32_t g_rel_base = 0x81800000u, g_rel_bss = 0x81845C80u, g_rel_bss_size = 0x00053D34u;')
# the game's own functions movie.rel calls: memset and memcpy natively, the rest must not be reached here
defined = set(re.findall(r'^void (f_80[0-9A-F]{6})\(Cpu', src, re.M))
dol = sorted(set(re.findall(r'\b(f_80[0-9A-F]{6})\b', src)) - defined)
tab.append('#include <string.h>\n#include <stdlib.h>\n#include <stdio.h>')
for n in dol:
    if n == 'f_8000E334':
        tab.append('void f_8000E334(Cpu* c) { memset(ppc_ptr(c->r[3]), (int)c->r[4], c->r[5]); }  /* memset */')
    elif n == 'f_8000E41C':
        tab.append('void f_8000E41C(Cpu* c) { memmove(ppc_ptr(c->r[3]), ppc_ptr(c->r[4]), c->r[5]); }  /* memcpy */')
    else:
        tab.append('void %s(Cpu* c) { (void)c; fprintf(stderr, "game function %s reached\\n"); exit(3); }' % (n, n[2:]))
write('build/tests/movie/movie_rel_table.c', '\n'.join(tab) + '\n')
PY
CC=${CC:-clang}  # the generated code uses musttail (clang, or GCC 15+)
INC="-Iruntime/include -Iruntime/src -Ibuild/gen"
GEN="-std=gnu11 -O2 -ffp-contract=off -fno-strict-aliasing -fwrapv -w $INC"
FLAGS="-std=gnu11 -O2 -Wall $INC"
[ $out/movie_rel.o -nt $out/movie_rel.c ] || $CC $GEN -c $out/movie_rel.c -o $out/movie_rel.o
$CC $GEN -c $out/movie_rel_table.c -o $out/movie_rel_table.o
# the decoder as configure.py builds it (MOVIE_CFLAGS adds or overrides flags)
$CC $FLAGS -O3 -ffp-contract=off ${MOVIE_CFLAGS:-} -DMOVIE_MPEG_TEST -Dhle_movie_slice=native_movie_slice -Df_8180E498_orig=test_slice_orig \
    -c runtime/src/movie_mpeg.c -o $out/movie_mpeg.o
$CC $FLAGS -c runtime/src/memory.c -o $out/memory.o
$CC $FLAGS -c runtime/src/gekko_fp.c -o $out/gekko_fp.o
$CC $FLAGS -c tools/tests/movie_mpeg_diff.c -o $out/movie_mpeg_diff.o
$CC $out/movie_mpeg_diff.o $out/movie_mpeg.o $out/movie_rel.o $out/movie_rel_table.o $out/memory.o $out/gekko_fp.o -lm -o $out/movie_mpeg_diff
$out/movie_mpeg_diff baserom/movie.rel "$@"
