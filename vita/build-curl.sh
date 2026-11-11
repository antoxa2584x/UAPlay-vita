#!/bin/sh
# Builds libcurl against vdpm's "openssl-1.1.1" package. The prebuilt vdpm
# curl targets OpenSSL 1.0.2, whose TLS client fails modern handshakes
# ("SSL23_GET_SERVER_HELLO:unsupported protocol"). The vdpm OpenSSL has no
# console UI, so curl must not use its ENGINE password prompts. The newlib
# inet_pton accepts host names as IPv4, which makes curl skip SNI; curl's
# built-in parser is used instead.
# Usage: VITASDK=/usr/local/vitasdk sh vita/build-curl.sh [prefix]
set -eu
: "${VITASDK:?Set VITASDK}"
PREFIX=${1:-$HOME/uaplay-vita-deps}
VERSION=8.17.0
WORK=${TMPDIR:-/tmp}/uaplay-curl
export PATH="$VITASDK/bin:$PATH"
mkdir -p "$WORK"
cd "$WORK"
[ -f curl-$VERSION.tar.gz ] || curl -fL -o curl-$VERSION.tar.gz \
  https://github.com/curl/curl/releases/download/curl-$(echo $VERSION | tr . _)/curl-$VERSION.tar.gz
rm -rf curl-$VERSION
tar xf curl-$VERSION.tar.gz
cd curl-$VERSION
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_CURL_EXE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
  -DENABLE_IPV6=OFF -DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_FCNTL_O_NONBLOCK=OFF \
  -DENABLE_THREADED_RESOLVER=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
  -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF \
  -DCURL_USE_OPENSSL=ON -DCMAKE_REQUIRED_FLAGS=-pthread \
  -DCMAKE_C_FLAGS=-DOPENSSL_NO_UI_CONSOLE -DHAVE_INET_PTON=OFF
sed -i '/HAVE_PIPE2/d' lib/curl_config.h
make -j"$(nproc)"
make install
