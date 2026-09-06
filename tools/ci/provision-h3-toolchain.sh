#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Provision the HTTP/3 test toolchain for the nightly protocol-matrix run
#: a QUIC-capable TLS library, an HTTP/3 curl, and an nginx
# binary built --with-http_v3_module from the SAME source tree the module is
# compiled against (module signatures are version-strict).
#
# Everything installs under $H3_ROOT (default ~/h3) from pinned, checksummed
# release tarballs — no sudo, no system packages beyond a C/C++ toolchain,
# perl and pkg-config. Idempotent: skips components whose binaries exist.
#
#   NGINX_SRC=$HOME/nginx-src ./tools/ci/provision-h3-toolchain.sh
#
# Produces:
#   $H3_ROOT/quictls            quictls (OpenSSL 3.3.0+quic), static
#   $H3_ROOT/deps               nghttp2/nghttp3/ngtcp2 static libs
#   $H3_ROOT/curl/bin/curl      curl with HTTP2 + HTTP3
#   $H3_ROOT/nginx-h3/objs/nginx  copy of $NGINX_SRC rebuilt with http_v3
set -euo pipefail

H3_ROOT="${H3_ROOT:-$HOME/h3}"
NGINX_SRC="${NGINX_SRC:-$HOME/nginx-src}"
J=$(nproc)
SRC="$H3_ROOT/src"
mkdir -p "$SRC"

QUICTLS_VER=openssl-3.3.0-quic1
QUICTLS_SHA256=78d675d94c0ac3a8b44073f0c2b373d948c5afd12b25c9e245262f563307a566
NGHTTP2_VER=1.66.0
NGHTTP2_SHA256=e178687730c207f3a659730096df192b52d3752786c068b8e5ee7aeb8edae05a
NGHTTP3_VER=1.10.1
NGHTTP3_SHA256=c86c8c6502141198ace1fb41e0293f035c6444ab1e057dc22bec8fe54f9991a2
NGTCP2_VER=1.13.0
NGTCP2_SHA256=a175a6a58313d5736256bf7978d20666f030632a5b6ba80c992d6475690633ea
CURL_VER=8.14.1
CURL_SHA256=6766ada7101d292b42b8b15681120acd68effa4a9660935853cf6d61f0d984d4

fetch() { # url sha256 dest
  local url=$1 sha=$2 dest=$3
  if [ ! -f "$dest" ]; then
    curl -fsSL --retry 3 -o "$dest.tmp" "$url"
    mv "$dest.tmp" "$dest"
  fi
  echo "$sha  $dest" | sha256sum -c - >/dev/null
}

if [ ! -x "$H3_ROOT/quictls/bin/openssl" ]; then
  echo "=== quictls $QUICTLS_VER"
  fetch "https://github.com/quictls/openssl/archive/refs/tags/$QUICTLS_VER.tar.gz" \
        "$QUICTLS_SHA256" "$SRC/quictls.tar.gz"
  rm -rf "$SRC/openssl-$QUICTLS_VER"
  tar xzf "$SRC/quictls.tar.gz" -C "$SRC"
  ( cd "$SRC/openssl-$QUICTLS_VER" \
    && ./Configure --prefix="$H3_ROOT/quictls" --libdir=lib no-shared no-tests >/dev/null \
    && make -s -j"$J" >/dev/null && make -s install_sw >/dev/null )
fi
"$H3_ROOT/quictls/bin/openssl" version

export PKG_CONFIG_PATH="$H3_ROOT/quictls/lib/pkgconfig:$H3_ROOT/deps/lib/pkgconfig"

if [ ! -f "$H3_ROOT/deps/lib/libnghttp2.a" ]; then
  echo "=== nghttp2 $NGHTTP2_VER"
  fetch "https://github.com/nghttp2/nghttp2/releases/download/v$NGHTTP2_VER/nghttp2-$NGHTTP2_VER.tar.gz" \
        "$NGHTTP2_SHA256" "$SRC/nghttp2.tar.gz"
  rm -rf "$SRC/nghttp2-$NGHTTP2_VER"; tar xzf "$SRC/nghttp2.tar.gz" -C "$SRC"
  ( cd "$SRC/nghttp2-$NGHTTP2_VER" \
    && ./configure --prefix="$H3_ROOT/deps" --enable-lib-only \
         --enable-static --disable-shared >/dev/null \
    && make -s -j"$J" >/dev/null && make -s install >/dev/null )
fi

if [ ! -f "$H3_ROOT/deps/lib/libnghttp3.a" ]; then
  echo "=== nghttp3 $NGHTTP3_VER"
  fetch "https://github.com/ngtcp2/nghttp3/releases/download/v$NGHTTP3_VER/nghttp3-$NGHTTP3_VER.tar.gz" \
        "$NGHTTP3_SHA256" "$SRC/nghttp3.tar.gz"
  rm -rf "$SRC/nghttp3-$NGHTTP3_VER"; tar xzf "$SRC/nghttp3.tar.gz" -C "$SRC"
  ( cd "$SRC/nghttp3-$NGHTTP3_VER" \
    && ./configure --prefix="$H3_ROOT/deps" --enable-lib-only \
         --enable-static --disable-shared >/dev/null \
    && make -s -j"$J" >/dev/null && make -s install >/dev/null )
fi

if [ ! -f "$H3_ROOT/deps/lib/libngtcp2.a" ]; then
  echo "=== ngtcp2 $NGTCP2_VER"
  fetch "https://github.com/ngtcp2/ngtcp2/releases/download/v$NGTCP2_VER/ngtcp2-$NGTCP2_VER.tar.gz" \
        "$NGTCP2_SHA256" "$SRC/ngtcp2.tar.gz"
  rm -rf "$SRC/ngtcp2-$NGTCP2_VER"; tar xzf "$SRC/ngtcp2.tar.gz" -C "$SRC"
  ( cd "$SRC/ngtcp2-$NGTCP2_VER" \
    && OPENSSL_CFLAGS="-I$H3_ROOT/quictls/include" \
       OPENSSL_LIBS="-L$H3_ROOT/quictls/lib -lssl -lcrypto -ldl -lpthread" \
       ./configure --prefix="$H3_ROOT/deps" --enable-lib-only \
         --enable-static --disable-shared --with-openssl >/dev/null \
    && make -s -j"$J" >/dev/null && make -s install >/dev/null )
fi

if [ ! -x "$H3_ROOT/curl/bin/curl" ]; then
  echo "=== curl $CURL_VER"
  fetch "https://curl.se/download/curl-$CURL_VER.tar.gz" \
        "$CURL_SHA256" "$SRC/curl.tar.gz"
  rm -rf "$SRC/curl-$CURL_VER"; tar xzf "$SRC/curl.tar.gz" -C "$SRC"
  ( cd "$SRC/curl-$CURL_VER" \
    && LIBS="-lpthread -ldl" \
       ./configure --prefix="$H3_ROOT/curl" --with-openssl --with-ngtcp2 \
         --with-nghttp3 --with-nghttp2="$H3_ROOT/deps" --disable-shared \
         --without-libpsl --disable-ldap --disable-manual >/dev/null \
    && make -s -j"$J" >/dev/null && make -s install >/dev/null )
fi
"$H3_ROOT/curl/bin/curl" --version | head -1
"$H3_ROOT/curl/bin/curl" --version | grep -q HTTP3

if [ ! -x "$H3_ROOT/nginx-h3/objs/nginx" ]; then
  echo "=== nginx-h3 (from $NGINX_SRC)"
  [ -d "$NGINX_SRC" ] || { echo "NGINX_SRC $NGINX_SRC not found"; exit 1; }
  rm -rf "$H3_ROOT/nginx-h3"
  cp -r "$NGINX_SRC" "$H3_ROOT/nginx-h3"
  ( cd "$H3_ROOT/nginx-h3" \
    && ./configure --with-http_v2_module --with-http_v3_module \
         --with-http_ssl_module --with-threads --with-compat \
         --with-cc-opt="-I$H3_ROOT/quictls/include" \
         --with-ld-opt="-L$H3_ROOT/quictls/lib -ldl -lpthread" >/dev/null \
    && make -s -j"$J" >/dev/null )
fi
"$H3_ROOT/nginx-h3/objs/nginx" -V 2>&1 | grep -q http_v3_module

# QUIC under concurrent load needs real UDP socket buffers; the kernel
# default (~208KB) drops handshake packets during the smoke's storms and
# shows up as client-side connect failures. Needs root once:
#   sysctl -w net.core.rmem_max=8388608 net.core.wmem_max=8388608 (+ sysctl.d)
RMEM=$(sysctl -n net.core.rmem_max 2>/dev/null || echo 0)
if [ "$RMEM" -lt 1048576 ]; then
  echo "WARNING: net.core.rmem_max=$RMEM is too small for the h3 smoke's"
  echo "         concurrency; expect spurious QUIC connect failures (see"
  echo "         header comment for the sysctl bump)."
fi
echo "H3 toolchain ready under $H3_ROOT"
