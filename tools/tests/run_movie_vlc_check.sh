#!/bin/sh
# Checks the native movie decoder's VLC tables (runtime/src/movie_mpeg.c) against pl_mpeg's, exhaustively.
# pl_mpeg (MIT) is fetched at a pinned commit into build/tests/pl_mpeg (not part of the repository, not shipped).
set -e
cd "$(dirname "$0")/../.."
dir=build/tests/pl_mpeg
commit=c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
mkdir -p $dir
[ -f $dir/pl_mpeg.h ] || curl -sSfL -o $dir/pl_mpeg.h https://raw.githubusercontent.com/phoboslab/pl_mpeg/$commit/pl_mpeg.h
${CC:-cc} -std=gnu11 -O1 -w -Iruntime/include -Iruntime/src -I$dir tools/tests/movie_vlc_check.c -lm -o $dir/movie_vlc_check
$dir/movie_vlc_check
