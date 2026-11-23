#!/bin/sh
# Builds and runs net_native_test.c on the host (Linux or macOS): the native network layer against loopback
# servers. Uses no game data. Usage: tools/tests/run_net_native_test.sh [-v]
set -e
cd "$(dirname "$0")/../.."
out=build/tests
mkdir -p $out
CC=${CC:-cc}
FLAGS="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iruntime/include -Iruntime/src"
$CC $FLAGS tools/tests/net_native_test.c runtime/src/net_native.c runtime/src/net_dns.c runtime/src/memory.c \
    -lpthread -o $out/net_native_test
$out/net_native_test "$@"
