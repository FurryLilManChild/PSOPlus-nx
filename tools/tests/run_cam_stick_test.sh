#!/bin/sh
# Builds and runs cam_stick_test.c on the host: the right-stick camera maths (runtime/src/cam_stick.h).
# Usage: tools/tests/run_cam_stick_test.sh
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
CC=${CC:-cc}
$CC -std=gnu11 -O2 -Wall -Wextra -Iruntime/src tools/tests/cam_stick_test.c -lm -o $out/cam_stick_test
$out/cam_stick_test
