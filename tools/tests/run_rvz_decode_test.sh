#!/bin/sh
# Builds tools/tests/rvz_decode_test.c with the Switch's RVZ/WIA decoder (runtime/src/platform/switch/rvz_decode.c)
# and checks it on synthetic discs (tools/tests/make_synthetic_gc.py, no game data):
#   - every RVZ/WIA in $RVZ_TEST_DIR (default build/tests/rvz) whose name is <disc>_<anything>.rvz/.wia is
#     decoded and compared byte for byte with <disc>.iso, and with tools/disc/rvz2iso.py's output;
#   - with NODTOOL set (nodtool 2.x, github.com/encounter/nod, MIT/Apache-2.0), the synthetic disc is first
#     converted to RVZ (zstd, none, LZMA, LZMA2) and WIA (LZMA, LZMA2) by that independent writer (its bzip2
#     writer fails in 2.0.0-alpha.9, so bzip2 is not covered).
# zstd headers: ZSTD_INC (a folder with zstd.h), else the system's, else devkitPro's portlibs copy.
# rvz2iso.py needs Python 3.14 or the 'zstandard' package for zstd images (ZSTD_PY_DIR: an unpacked wheel).
set -e
cd "$(dirname "$0")/../.."
out=build/tests
dir=${RVZ_TEST_DIR:-$out/rvz}
mkdir -p $out "$dir"
CC=${CC:-cc}
inc=""
if [ -n "$ZSTD_INC" ]; then inc="-I$ZSTD_INC"
elif [ ! -f /usr/include/zstd.h ] && [ -n "$DEVKITPRO" ] && [ -f "$DEVKITPRO/portlibs/switch/include/zstd.h" ]; then
    mkdir -p $out/zstd_inc
    cp "$DEVKITPRO/portlibs/switch/include/zstd.h" "$DEVKITPRO/portlibs/switch/include/zstd_errors.h" $out/zstd_inc/
    inc="-I$out/zstd_inc"
fi
zlib="-lzstd"
if ! printf 'int main(void){return 0;}' | $CC -x c - -lzstd -o /dev/null 2>/dev/null; then
    for p in /usr/lib/x86_64-linux-gnu/libzstd.so.1 /usr/lib/aarch64-linux-gnu/libzstd.so.1 /usr/lib/libzstd.so.1; do
        [ -f $p ] && zlib=$p && break
    done
fi
$CC -std=gnu11 -O2 -Wall -Wextra $inc -Iruntime/src/platform/switch tools/tests/rvz_decode_test.c \
    runtime/src/platform/switch/rvz_decode.c $zlib -llzma -lbz2 -o $out/rvz_decode_test

if [ ! -f "$dir/syn.iso" ]; then python3 -I tools/tests/make_synthetic_gc.py "$dir/syn.iso"; fi
if [ -n "$NODTOOL" ]; then
    for c in zstd:19 none lzma lzma2; do
        [ -f "$dir/syn_${c%%:*}.rvz" ] || "$NODTOOL" convert -c $c "$dir/syn.iso" "$dir/syn_${c%%:*}.rvz" >/dev/null
    done
    for c in lzma lzma2; do
        [ -f "$dir/syn_$c.wia" ] || "$NODTOOL" convert -c $c "$dir/syn.iso" "$dir/syn_$c.wia" >/dev/null
    done
fi
pyz=""
[ -n "$ZSTD_PY_DIR" ] && pyz="--zstd-dir $ZSTD_PY_DIR"
pass=0
fail=0
for img in "$dir"/*_*.rvz "$dir"/*_*.wia; do
    [ -f "$img" ] || continue
    base=$(basename "$img")
    src="$dir/${base%%_*}.iso"
    [ -f "$src" ] || continue
    if ! $out/rvz_decode_test "$img" $out/rvz_c.iso; then echo "FAIL $base: the C decoder stopped"; fail=$((fail+1)); continue; fi
    if ! cmp -s $out/rvz_c.iso "$src"; then echo "FAIL $base: C output differs from $src"; fail=$((fail+1)); continue; fi
    rm -f $out/rvz_py.iso
    if python3 -I tools/disc/rvz2iso.py "$img" $out/rvz_py.iso $pyz >/dev/null 2>$out/rvz_py.err; then
        if cmp -s $out/rvz_c.iso $out/rvz_py.iso; then echo "ok   $base: same as the source and as rvz2iso.py"
        else echo "FAIL $base: C output differs from rvz2iso.py's"; fail=$((fail+1)); continue; fi
    else
        echo "ok   $base: same as the source (rvz2iso.py could not run: $(tail -1 $out/rvz_py.err))"
    fi
    pass=$((pass+1))
done
# damaged images must stop with an error, never produce output that looks complete
for img in "$dir"/syn_zstd.rvz; do
    [ -f "$img" ] || continue
    head -c $(( $(wc -c < "$img") - 1000 )) "$img" > $out/rvz_trunc.rvz
    if $out/rvz_decode_test $out/rvz_trunc.rvz $out/rvz_bad.iso 2>/dev/null; then echo "FAIL a truncated image was accepted"; fail=$((fail+1))
    else echo "ok   truncated image refused"; pass=$((pass+1)); fi
    cp "$img" $out/rvz_flip.rvz
    printf '\377' | dd of=$out/rvz_flip.rvz bs=1 seek=$(( $(wc -c < "$img") / 2 )) conv=notrunc 2>/dev/null
    if $out/rvz_decode_test $out/rvz_flip.rvz $out/rvz_bad.iso 2>/dev/null && cmp -s $out/rvz_bad.iso "$dir/syn.iso"; then
        echo "FAIL a damaged image decoded to the source (the byte flip landed nowhere?)"; fail=$((fail+1))
    else echo "ok   damaged image refused or differs (caught by the SHA-1 check on the console)"; pass=$((pass+1)); fi
done
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] && [ $pass -gt 0 ]
