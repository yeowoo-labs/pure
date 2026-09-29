#!/bin/sh
# ngtcp2 1.25.0 with its OpenSSL 3.5 backend, into build/third (not kept in
# the repository), and drain.c against it: build/third/ngtcp2-drain.
set -e
root=$(cd "$(dirname "$0")/../../../.." && pwd)
third=$root/build/third
if [ ! -f "$third/prefix/lib/libngtcp2.a" ]; then
    mkdir -p "$third"
    [ -d "$third/ngtcp2" ] || git clone -q --depth 1 --branch v1.25.0 https://github.com/ngtcp2/ngtcp2 "$third/ngtcp2"
    mkdir -p "$third/ngtcp2/b"
    (cd "$third/ngtcp2/b" && cmake -DENABLE_LIB_ONLY=ON -DENABLE_OPENSSL=ON -DENABLE_SHARED_LIB=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$third/prefix" .. > /dev/null && make -s -j4 > /dev/null &&
        make -s install > /dev/null)
fi
cc -O2 -g -std=c17 -Wall -Wextra -Werror -I"$third/prefix/include" "$(dirname "$0")/drain.c" \
    "$third/prefix/lib/libngtcp2_crypto_ossl.a" "$third/prefix/lib/libngtcp2.a" -lssl -lcrypto -o "$third/ngtcp2-drain"
