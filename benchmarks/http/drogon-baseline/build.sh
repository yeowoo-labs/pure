#!/bin/sh
# Build the drogon reference service against Debian's drogon without
# installing anything: the packages are downloaded and unpacked under .deps
# (an old-style rpath, so drogon finds its own dependencies there too).
set -e
cd "$(dirname "$0")"
LIB=.deps/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)
if [ ! -d "$LIB" ]; then
    mkdir -p .deps/debs
    (cd .deps/debs && apt-get download libdrogon-dev libdrogon1t64 libtrantor-dev libtrantor1 libjsoncpp-dev \
        libhiredis1.1.0 libyaml-cpp0.8 && for deb in *.deb; do dpkg -x "$deb" ..; done)
fi
g++ -O2 -std=c++17 -I.deps/usr/include -I.deps/usr/include/jsoncpp -o drogon-baseline main.cc \
    -L"$LIB" -Wl,--disable-new-dtags,-rpath,"$PWD/$LIB" -Wl,-rpath-link,"$LIB" -ldrogon -ltrantor -l:libjsoncpp.so.26 \
    -lssl -lcrypto -lz -luuid -lpthread
