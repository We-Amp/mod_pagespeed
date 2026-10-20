#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The native pair of the ASP.NET Core sidecar (WeAmp.PageSpeed.Sidecar): a
# matched nginx, ngx_pagespeed_module.so built against exactly that nginx, and
# the pagespeed-nginx-launch shim, per architecture, plus the .NET test suite
# that runs the sidecar against it. Used by the pull request tier and the
# module release lane (docs/ci/release.md, aspnetcore/README.md).
#
#   release_sidecar.sh build   --arch x64|arm64 --workspace DIR --out DIR
#   release_sidecar.sh hygiene --arch ARCH --src DIR --pair DIR
#   release_sidecar.sh smoke   --arch ARCH --src DIR --pair DIR
#   release_sidecar.sh strip   --arch ARCH --pair DIR --debug DIR
#   release_sidecar.sh test    --arch ARCH --src DIR [--pair DIR] [--results DIR]
#                              [--framework net8.0|net10.0]...
#   release_sidecar.sh package --arch ARCH --src DIR --pair DIR --version V
#                              --source-sha SHA --out DIR
#   release_sidecar.sh --self-test
#
# `build` runs the focal recipe of install/nginx/build_module_in_container.sh
# in the pinned Ubuntu 20.04 image (FOCAL_IMAGE below): glibc 2.31 is the
# compatibility floor of the pair. The container runs as root (the recipe
# installs its toolchain with apt) and is never privileged. The image
# architecture must be the host's: the pair is built natively, never under
# emulation.
#
# The gates, in the order the callers run them:
#   hygiene  the module exports only the nginx module symbols and links no
#            OpenSSL; nginx and the shim link no OpenSSL; none of the three
#            needs libstdc++ or a glibc newer than 2.31.
#   smoke    load, serve and optimize in Debian 11 and Debian 12
#            (test/package-test/check-sidecar-pair.sh): X-Page-Speed carries
#            the version of the tree, and no X-PageSpeed-Warn appears.
#   strip    debug files split off into --debug, then the smoke again on the
#            stripped pair.
#   test     the xunit suite of aspnetcore/ in the pinned .NET SDK images. With
#            --pair the pair is placed where the sidecar finds it and the live
#            nginx tests must run (PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1): a
#            missing pair fails them instead of skipping them. Every test must
#            pass, none may be skipped, and at least SIDECAR_MIN_TESTS (default
#            201) must have run, per framework.
#
# `package` writes pagespeed-sidecar-native-<version>-linux-<arch>.tar.gz:
# nginx, ngx_pagespeed_module.so, pagespeed-nginx-launch, build-info.json
# (versions and the sha256 of each binary) and the license files.
set -euo pipefail

# The build image of the pair, pinned by digest: Ubuntu 20.04 (glibc 2.31), the
# compatibility floor of the sidecar. Ubuntu 20.04 is past its standard support
# (May 2025); it is the build base only, nothing of it ships except what glibc
# 2.31 implies for the hosts the pair runs on. Moving the floor is a decision
# about which hosts the sidecar supports, not an image refresh: see
# aspnetcore/README.md.
FOCAL_IMAGE="ubuntu:20.04@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214"
GLIBC_FLOOR="2.31"
# The smoke hosts: the oldest and a current Debian the pair supports.
SMOKE_IMAGES=(
  "debian:11@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9"
  "debian:12@sha256:2c037a04925515fdd6ea85ea14a682d0e79931f5e9f5d07b6dbfc6ba12f9e858"
)
# The .NET SDKs the test suite runs under, one per framework the package ships.
DOTNET_IMAGE_NET8="mcr.microsoft.com/dotnet/sdk:8.0-bookworm-slim@sha256:f7e28a0a3f6f9235929ec19274fd0ddf8534e9159f99fac602ac73304f1ce99f"
DOTNET_IMAGE_NET10="mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:0d99ebf1ccad15037b5c98c9fe8979d6b8dbac23812feae2277a0b277b5c5ffa"

PAIR_FILES=(nginx ngx_pagespeed_module.so pagespeed-nginx-launch)

# Signatures of a failed download that a retry fixes. A compile or link error
# is not retried.
TRANSIENT_FETCH_RE='Hash Sum mismatch|Unable to fetch some archives|bad record MAC|Connection reset|Temporary failure resolving|unexpected EOF|could not download Bazel|Failed to fetch'

die() { echo "::error::release_sidecar: $*" >&2; exit 1; }

rid_of() {
  case "$1" in
    x64) echo linux-x64 ;;
    arm64) echo linux-arm64 ;;
    *) return 1 ;;
  esac
}

# The container platform of an architecture. Every container is started with
# it, so a locally cached image of another architecture is never used.
platform_of() {
  case "$1" in
    x64) echo linux/amd64 ;;
    arm64) echo linux/arm64 ;;
    *) return 1 ;;
  esac
}

# The architecture of the container engine, as x64 or arm64.
engine_arch() {
  local a
  a="$(docker info --format '{{.Architecture}}' 2>/dev/null || uname -m)"
  case "$a" in
    x86_64|amd64) echo x64 ;;
    aarch64|arm64) echo arm64 ;;
    *) echo "$a" ;;
  esac
}

# The nginx version the focal recipe builds against (its DEF_NGINX_VER), read
# from the recipe so that the pair and its manifest never disagree with it.
focal_nginx_version() {
  local recipe="$1/install/nginx/build_module_in_container.sh" v
  [ -f "$recipe" ] || { echo "release_sidecar: no recipe at $recipe" >&2; return 1; }
  v="$(awk '/^[[:space:]]*focal\)[[:space:]]*DEF_NGINX_VER=/ {
              if (match($0, /[0-9]+\.[0-9]+\.[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' "$recipe")"
  [ -n "$v" ] || { echo "release_sidecar: no focal DEF_NGINX_VER in $recipe" >&2; return 1; }
  printf '%s\n' "$v"
}

# The version the tree stamps into the module (X-Page-Speed).
tree_version() {
  local f="$1/net/instaweb/public/VERSION"
  [ -f "$f" ] || { echo "release_sidecar: no VERSION file in $1" >&2; return 1; }
  # shellcheck disable=SC1090
  ( . "$f"; printf '%s.%s.%s%s\n' "$MAJOR" "$MINOR" "$BUILD" "${PRERELEASE:+-$PRERELEASE}" )
}

ARCH=""; WS=""; OUT=""; SRC=""; PAIR=""; DEBUG_DIR=""; RESULTS=""; VERSION=""; SOURCE_SHA=""
FRAMEWORKS=()
parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) ARCH="$2"; shift 2 ;;
      --workspace) WS="$2"; shift 2 ;;
      --out) OUT="$2"; shift 2 ;;
      --src) SRC="$2"; shift 2 ;;
      --pair) PAIR="$2"; shift 2 ;;
      --debug) DEBUG_DIR="$2"; shift 2 ;;
      --results) RESULTS="$2"; shift 2 ;;
      --version) VERSION="$2"; shift 2 ;;
      --source-sha) SOURCE_SHA="$2"; shift 2 ;;
      --framework) FRAMEWORKS+=("$2"); shift 2 ;;
      *) echo "release_sidecar: unknown argument $1" >&2; return 2 ;;
    esac
  done
  if [ -n "$ARCH" ]; then
    rid_of "$ARCH" >/dev/null || { echo "release_sidecar: unknown arch '$ARCH'" >&2; return 2; }
  fi
}

abs_dir() { (cd "$1" && pwd); }

need_pair() {
  local f
  [ -n "$PAIR" ] && [ -d "$PAIR" ] || die "--pair DIR is required"
  PAIR="$(abs_dir "$PAIR")"
  for f in "${PAIR_FILES[@]}"; do
    [ -s "$PAIR/$f" ] || die "the pair in $PAIR has no $f"
  done
}

need_src() {
  [ -n "$SRC" ] && [ -d "$SRC" ] || die "--src DIR is required"
  SRC="$(abs_dir "$SRC")"
}

need_native() {
  local have
  have="$(engine_arch)"
  [ "$have" = "$ARCH" ] || die "the container engine is $have, not $ARCH: the pair is built and tested natively, never emulated"
}

# --- build -------------------------------------------------------------------

build() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] && [ -n "$WS" ] && [ -d "$WS" ] && [ -n "$OUT" ] \
    || die "build: --arch, --workspace and --out are required"
  need_native
  WS="$(abs_dir "$WS")"
  mkdir -p "$OUT"; OUT="$(abs_dir "$OUT")"
  [ -d "$WS/vendor" ] || die "build: $WS has no vendor/ (tools/vendor-deps.sh vendor)"
  local ngxver uid gid logs attempt rc
  ngxver="$(focal_nginx_version "$WS")"
  uid="$(id -u)"; gid="$(id -g)"
  logs="$(mktemp -d)"
  echo "sidecar pair: linux-${ARCH}, nginx ${ngxver}, module $(tree_version "$WS"), image ${FOCAL_IMAGE}"

  local extra_docker=()
  if [ -n "${BAZEL_CACHE_HOST:-}" ]; then
    extra_docker+=(--add-host="bazel-remote-cache:${BAZEL_CACHE_HOST}")
  fi

  for attempt in 1 2 3; do
    rm -f "$OUT/nginx" "$OUT/ngx_pagespeed_module.so" "$OUT/pagespeed-nginx-launch"
    # Root inside the container: the recipe installs its toolchain with apt.
    # HOME and the Bazel output root live inside the container; the recipe's
    # nginx tree under the workspace is removed again, and the outputs are
    # handed back to the runner user.
    if docker run --rm --init --platform "$(platform_of "$ARCH")" ${extra_docker[@]+"${extra_docker[@]}"} \
      -e HOME=/root \
      -e NGXVER="$ngxver" -e HOST_UID="$uid" -e HOST_GID="$gid" \
      -e BAZEL_CPUS="${BAZEL_CPUS:-}" -e MAKE_JOBS="${MAKE_JOBS:-}" \
      -v "$WS:/workspace" -v "$OUT:/out" -w /workspace \
      "$FOCAL_IMAGE" bash -c '
        set -euo pipefail
        [ -n "$BAZEL_CPUS" ] || unset BAZEL_CPUS
        [ -n "$MAKE_JOBS" ] || unset MAKE_JOBS
        cleanup() {
          rm -rf /workspace/.ngx-module-build
          find /workspace -maxdepth 1 -name "bazel-*" -type l -delete 2>/dev/null || true
          chown -R "$HOST_UID:$HOST_GID" /out 2>/dev/null || true
        }
        trap cleanup EXIT
        echo "--- container arch: $(uname -m), glibc $(ldd --version | head -1 | grep -oE "[0-9]+\.[0-9]+$") ---"
        install/nginx/build_module_in_container.sh focal -o /out/ngx_pagespeed_module.so
        cd "/workspace/.ngx-module-build/focal/nginx-${NGXVER}"
        make -f objs/Makefile objs/nginx -j"${MAKE_JOBS:-$(nproc)}"
        install -m 755 objs/nginx /out/nginx
        cd /workspace
        echo "--- pagespeed-nginx-launch ---"
        gcc-13 -O2 -Wall -o /out/pagespeed-nginx-launch install/nginx/pagespeed-nginx-launch.c
        chmod 755 /out/pagespeed-nginx-launch
        chmod 644 /out/ngx_pagespeed_module.so
      ' 2>&1 | tee "$logs/build.log"; then
      break
    else
      rc=$?
    fi
    if [ "$attempt" -ge 3 ] || ! grep -qE "$TRANSIENT_FETCH_RE" "$logs/build.log"; then
      rm -rf "$logs"
      die "build: the focal build failed (exit $rc)"
    fi
    echo "::warning::release_sidecar: transient fetch failure, retry $((attempt + 1))/3"
    sleep $((attempt * 15))
  done
  rm -rf "$logs"
  PAIR="$OUT"; need_pair
  ls -l "$OUT"
}

# --- gates -------------------------------------------------------------------

hygiene() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] || die "hygiene: --arch is required"
  need_native; need_src; need_pair
  docker run --rm --platform "$(platform_of "$ARCH")" -e GLIBC_FLOOR="$GLIBC_FLOOR" \
    -v "$SRC:/src:ro" -v "$PAIR:/p:ro" -w /src \
    "$FOCAL_IMAGE" bash -c '
      set -euo pipefail
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null && apt-get install -y -qq binutils >/dev/null
      # The module: no OpenSSL, and only the nginx module symbols exported.
      bash install/nginx/assert_symbol_hygiene.sh /p/ngx_pagespeed_module.so
      fail=0
      for b in nginx ngx_pagespeed_module.so pagespeed-nginx-launch; do
        needed="$(readelf -d "/p/$b" | awk "/\(NEEDED\)/ { gsub(/[][]/, \"\", \$NF); print \$NF }")"
        echo "### $b NEEDED: $(echo $needed)"
        if printf "%s\n" "$needed" | grep -qiE "^lib(ssl|crypto)"; then
          echo "::error::$b links OpenSSL: the pair would not load where another OpenSSL is installed"; fail=1
        fi
        if printf "%s\n" "$needed" | grep -qE "^lib(stdc\+\+|gcc_s)"; then
          echo "::error::$b needs the C++ runtime of the host: it must be linked statically"; fail=1
        fi
        max="$(objdump -T "/p/$b" 2>/dev/null | grep -oE "GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?" | sed "s/GLIBC_//" | sort -V | tail -1)"
        echo "    $b highest glibc symbol version: ${max:-none} (floor ${GLIBC_FLOOR})"
        if [ -n "$max" ] && [ "$(printf "%s\n%s\n" "$max" "$GLIBC_FLOOR" | sort -V | tail -1)" != "$GLIBC_FLOOR" ]; then
          echo "::error::$b needs glibc $max, above the floor $GLIBC_FLOOR"; fail=1
        fi
      done
      exit "$fail"
    '
  echo "release_sidecar: hygiene ok (linux-${ARCH})"
}

smoke() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] || die "smoke: --arch is required"
  need_native; need_src; need_pair
  local img
  for img in "${SMOKE_IMAGES[@]}"; do
    DOCKER_PLATFORM="$(platform_of "$ARCH")" bash "$SRC/test/package-test/check-sidecar-pair.sh" "$PAIR" "$img" \
      || die "smoke: the pair failed on ${img%%@*}"
  done
}

strip_pair() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] && [ -n "$DEBUG_DIR" ] || die "strip: --arch and --debug are required"
  need_native; need_pair
  mkdir -p "$DEBUG_DIR"; DEBUG_DIR="$(abs_dir "$DEBUG_DIR")"
  docker run --rm --platform "$(platform_of "$ARCH")" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$PAIR:/p" -v "$DEBUG_DIR:/d" "$FOCAL_IMAGE" bash -c '
      set -euo pipefail
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null && apt-get install -y -qq binutils file >/dev/null
      trap "chown -R \"$HOST_UID:$HOST_GID\" /p /d" EXIT
      cd /p
      for b in nginx ngx_pagespeed_module.so pagespeed-nginx-launch; do
        objcopy --only-keep-debug "$b" "/d/$b.debug"
        strip --strip-unneeded "$b"
        objcopy --add-gnu-debuglink="/d/$b.debug" "$b"
        chmod 644 "/d/$b.debug"
        file "$b" | grep -q ", stripped" || { echo "::error::$b is not stripped"; exit 1; }
        [ -s "/d/$b.debug" ] || { echo "::error::no debug file for $b"; exit 1; }
      done
      chmod 755 nginx pagespeed-nginx-launch
      chmod 644 ngx_pagespeed_module.so
      file nginx ngx_pagespeed_module.so pagespeed-nginx-launch | sed "s/^/  /"
    '
  ls -l "$PAIR" "$DEBUG_DIR"
}

# --- tests -------------------------------------------------------------------

# trx_counter FILE NAME: one attribute of the <Counters> element of a trx file.
trx_counter() {
  sed -n 's/.*<Counters \(.*\)\/>.*/\1/p' "$1" | head -1 | grep -oE "(^| )$2=\"[0-9]+\"" | grep -oE '[0-9]+' || echo ""
}

# check_trx FILE MIN: every test passed, none was skipped, at least MIN ran.
check_trx() {
  local f="$1" min="$2" total executed passed failed
  [ -s "$f" ] || { echo "::error::no test results in $f"; return 1; }
  total="$(trx_counter "$f" total)"
  executed="$(trx_counter "$f" executed)"
  passed="$(trx_counter "$f" passed)"
  failed="$(trx_counter "$f" failed)"
  echo "tests: total=${total:-?} executed=${executed:-?} passed=${passed:-?} failed=${failed:-?} ($(basename "$f"))"
  [ -n "$total" ] && [ -n "$executed" ] && [ -n "$passed" ] || { echo "::error::unreadable test counters in $f"; return 1; }
  [ "${failed:-0}" = 0 ] || { echo "::error::${failed} test(s) failed"; return 1; }
  [ "$executed" = "$total" ] && [ "$passed" = "$total" ] \
    || { echo "::error::not every test ran and passed (${passed} of ${total} passed, ${executed} executed)"; return 1; }
  [ "$total" -ge "$min" ] || { echo "::error::only ${total} tests ran, expected at least ${min}"; return 1; }
}

run_tests() {
  parse_args "$@" || return $?
  need_src
  [ -d "$SRC/aspnetcore" ] || die "test: $SRC has no aspnetcore/"
  if [ -n "$PAIR" ]; then
    [ -n "$ARCH" ] || die "test: --pair needs --arch"
    need_native; need_pair
  fi
  [ "${#FRAMEWORKS[@]}" -gt 0 ] || FRAMEWORKS=(net8.0 net10.0)
  local min="${SIDECAR_MIN_TESTS:-201}" rid="" platform="" work fw image props require=0 fail=0
  [ -z "$ARCH" ] || { rid="$(rid_of "$ARCH")"; platform="$(platform_of "$ARCH")"; }
  [ -z "$PAIR" ] || require=1
  [ -n "$RESULTS" ] || RESULTS="$(mktemp -d)"
  mkdir -p "$RESULTS"; RESULTS="$(abs_dir "$RESULTS")"

  for fw in "${FRAMEWORKS[@]}"; do
    case "$fw" in
      net8.0) image="$DOTNET_IMAGE_NET8"; props="" ;;
      # The projects target net8.0 unless told otherwise; one framework for
      # every project of the graph, restore included.
      net10.0) image="$DOTNET_IMAGE_NET10"; props="-p:TargetFramework=net10.0 -p:TargetFrameworks=net10.0" ;;
      *) die "test: unknown framework '$fw' (net8.0 or net10.0)" ;;
    esac
    # A fresh copy per framework: no build output of the tree or of another
    # framework is reused, and the tree itself is never written to.
    work="$(mktemp -d)"
    cp -a "$SRC/aspnetcore/." "$work/"
    if [ -n "$PAIR" ]; then
      mkdir -p "$work/.pair"
      cp -a "$PAIR/." "$work/.pair/"
    fi
    echo "=== sidecar tests: ${fw}${rid:+, live nginx linux-${ARCH}} (${image%%@*}) ==="
    if ! docker run --rm --init ${platform:+--platform "$platform"} \
      -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
      -e FW="$fw" -e PROPS="$props" -e RID="$rid" -e REQUIRE="$require" \
      -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 -e DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
      -v "$work:/w" -v "$RESULTS:/results" -w /w \
      "$image" bash -c '
        set -euo pipefail
        if [ "$REQUIRE" = 1 ]; then
          # The bundled nginx links PCRE; a consumer host provides it.
          export DEBIAN_FRONTEND=noninteractive
          apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends libpcre3 >/dev/null
        fi
        # dotnet needs an account for the user it runs as.
        getent group "$HOST_GID" >/dev/null || groupadd -o -g "$HOST_GID" sidecar
        getent passwd "$HOST_UID" >/dev/null || useradd -o -M -u "$HOST_UID" -g "$HOST_GID" -d /tmp/home sidecar
        mkdir -p /tmp/home
        chown -R "$HOST_UID:$HOST_GID" /w /results /tmp/home
        # The suite runs as the runner user, never as root: the sidecar is
        # meant to run unprivileged and switches nginx to root otherwise.
        # shellcheck disable=SC2086
        exec setpriv --reuid="$HOST_UID" --regid="$HOST_GID" --clear-groups \
          env HOME=/tmp/home FW="$FW" PROPS="$PROPS" RID="$RID" REQUIRE="$REQUIRE" bash -c "
            set -euo pipefail
            dotnet build WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj -c Release \$PROPS
            if [ \"\$REQUIRE\" = 1 ]; then
              dest=WeAmp.PageSpeed.AspNetCore.Tests/bin/Release/\$FW/runtimes/\$RID/native
              mkdir -p \"\$dest\"
              cp -a .pair/. \"\$dest/\"
              ls -l \"\$dest\"
              export PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1
            fi
            dotnet test WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj -c Release --no-build \$PROPS \
              --logger \"trx;LogFileName=sidecar-\$FW.trx\" --logger \"console;verbosity=normal\" \
              --results-directory /results
          "
      '; then
      echo "::error::the sidecar tests failed (${fw})"
      fail=1
    fi
    # The build ran as the runner user, so the copy can be removed.
    rm -rf "$work"
    check_trx "$RESULTS/sidecar-${fw}.trx" "$min" || fail=1
  done
  [ "$fail" = 0 ] || die "test: the sidecar test suite did not pass"
  echo "release_sidecar: tests ok (${FRAMEWORKS[*]}${rid:+, live nginx linux-${ARCH}})"
}

# --- package -----------------------------------------------------------------

package() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] && [ -n "$VERSION" ] && [ -n "$SOURCE_SHA" ] && [ -n "$OUT" ] \
    || die "package: --arch, --version, --source-sha and --out are required"
  need_src; need_pair
  printf '%s' "$SOURCE_SHA" | grep -Eq '^[0-9a-f]{40}$' || die "package: --source-sha is not a commit id"
  printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || die "package: bad --version '$VERSION'"
  local rid ngxver tree name stage
  rid="$(rid_of "$ARCH")"
  ngxver="$(focal_nginx_version "$SRC")"
  tree="$(tree_version "$SRC")"
  [ "$tree" = "$VERSION" ] || die "package: the tree is stamped $tree, not $VERSION"
  mkdir -p "$OUT"; OUT="$(abs_dir "$OUT")"
  name="pagespeed-sidecar-native-${VERSION}-${rid}"
  stage="$(mktemp -d)"
  mkdir -p "$stage/$name"
  cp -a "$PAIR/nginx" "$PAIR/ngx_pagespeed_module.so" "$PAIR/pagespeed-nginx-launch" "$stage/$name/"
  cp "$SRC/aspnetcore/LICENSE" "$SRC/aspnetcore/NOTICE" "$SRC/aspnetcore/THIRD-PARTY-NOTICES" "$stage/$name/"
  # The tarball is written in a container: GNU tar for a reproducible archive
  # wherever the script runs. The commit time is the time of every entry.
  docker run --rm --platform "$(platform_of "$ARCH")" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e NAME="$name" -e RID="$rid" -e VERSION="$VERSION" -e NGXVER="$ngxver" \
    -e SOURCE_SHA="$SOURCE_SHA" -e IMAGE="$FOCAL_IMAGE" -e GLIBC_FLOOR="$GLIBC_FLOOR" \
    -e EPOCH="${SOURCE_DATE_EPOCH:-0}" \
    -v "$stage:/s" -v "$OUT:/o" -w /s "$FOCAL_IMAGE" bash -c '
      set -euo pipefail
      trap "chown -R \"$HOST_UID:$HOST_GID\" /o /s" EXIT
      sha() { sha256sum "$NAME/$1" | cut -d" " -f1; }
      printf "{\n" > "$NAME/build-info.json"
      {
        printf "  \"schema\": 1,\n"
        printf "  \"package_version\": \"%s\",\n" "$VERSION"
        printf "  \"mod_pagespeed_version\": \"%s\",\n" "$VERSION"
        printf "  \"rid\": \"%s\",\n" "$RID"
        printf "  \"nginx_version\": \"%s\",\n" "$NGXVER"
        printf "  \"glibc_floor\": \"%s\",\n" "$GLIBC_FLOOR"
        printf "  \"build_image\": \"%s\",\n" "$IMAGE"
        printf "  \"source_sha\": \"%s\",\n" "$SOURCE_SHA"
        printf "  \"nginx_sha256\": \"%s\",\n" "$(sha nginx)"
        printf "  \"module_sha256\": \"%s\",\n" "$(sha ngx_pagespeed_module.so)"
        printf "  \"launch_sha256\": \"%s\"\n" "$(sha pagespeed-nginx-launch)"
        printf "}\n"
      } >> "$NAME/build-info.json"
      chmod 644 "$NAME"/build-info.json "$NAME"/LICENSE "$NAME"/NOTICE "$NAME"/THIRD-PARTY-NOTICES "$NAME"/ngx_pagespeed_module.so
      chmod 755 "$NAME"/nginx "$NAME"/pagespeed-nginx-launch
      tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@${EPOCH}" \
        -cf - "$NAME" | gzip -9n > "/o/${NAME}.tar.gz"
      tar -tvzf "/o/${NAME}.tar.gz"
      cat "$NAME/build-info.json"
    '
  rm -rf "$stage"
  ls -l "$OUT/${name}.tar.gz"
}

# --- self-test ---------------------------------------------------------------

self_test() {
  local fail=0 t root
  t="$(mktemp -d)"
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  chk() { [ "$2" = "$3" ] || { echo "FAIL: $1: got '$2', want '$3'"; fail=1; }; }

  chk "rid x64" "$(rid_of x64)" linux-x64
  chk "rid arm64" "$(rid_of arm64)" linux-arm64
  if rid_of mips >/dev/null 2>&1; then echo "FAIL: unknown arch accepted"; fail=1; fi
  chk "platform x64" "$(platform_of x64)" linux/amd64
  chk "platform arm64" "$(platform_of arm64)" linux/arm64

  # the images are pinned by digest
  local img
  for img in "$FOCAL_IMAGE" "${SMOKE_IMAGES[@]}" "$DOTNET_IMAGE_NET8" "$DOTNET_IMAGE_NET10"; do
    printf '%s' "$img" | grep -Eq '@sha256:[0-9a-f]{64}$' || { echo "FAIL: image not pinned by digest: $img"; fail=1; }
  done
  case "$FOCAL_IMAGE" in ubuntu:20.04@*) ;; *) echo "FAIL: the build image is not Ubuntu 20.04: $FOCAL_IMAGE"; fail=1 ;; esac

  # the nginx version comes from the recipe's focal row
  local v
  v="$(focal_nginx_version "$root")" || v=""
  printf '%s' "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "FAIL: focal nginx version from the recipe: '$v'"; fail=1; }
  mkdir -p "$t/r/install/nginx"
  # shellcheck disable=SC2016 # the recipe text, written literally
  printf '%s\n' 'case "${DISTRO}" in' \
    '  noble) DEF_NGINX_VER="1.24.0"' \
    '  focal) DEF_NGINX_VER="1.30.9"' \
    '         DEF_NGINX_SHA256="x" ;;' > "$t/r/install/nginx/build_module_in_container.sh"
  chk "focal row" "$(focal_nginx_version "$t/r")" 1.30.9
  printf 'noble) DEF_NGINX_VER="1.24.0"\n' > "$t/r/install/nginx/build_module_in_container.sh"
  if focal_nginx_version "$t/r" >/dev/null 2>&1; then echo "FAIL: a recipe without a focal row accepted"; fail=1; fi

  # the version the tree stamps
  mkdir -p "$t/r/net/instaweb/public"
  printf 'MAJOR=2\nMINOR=5\nBUILD=1\nPATCH=0\nPRERELEASE=\n' > "$t/r/net/instaweb/public/VERSION"
  chk "tree version" "$(tree_version "$t/r")" 2.5.1
  printf 'MAJOR=2\nMINOR=6\nBUILD=0\nPATCH=0\nPRERELEASE=rc.1\n' > "$t/r/net/instaweb/public/VERSION"
  chk "tree version rc" "$(tree_version "$t/r")" 2.6.0-rc.1

  # the test result gate
  local trx="$t/x.trx"
  printf '<TestRun><ResultSummary outcome="Completed"><Counters total="166" executed="166" passed="166" failed="0" error="0" timeout="0" aborted="0" inconclusive="0" passedButRunAborted="0" notRunnable="0" notExecuted="0" disconnected="0" warning="0" completed="0" inProgress="0" pending="0" /></ResultSummary></TestRun>\n' > "$trx"
  check_trx "$trx" 166 >/dev/null 2>&1 || { echo "FAIL: a green run refused"; fail=1; }
  check_trx "$trx" 167 >/dev/null 2>&1 && { echo "FAIL: too few tests accepted"; fail=1; }
  sed 's/executed="166" passed="166"/executed="160" passed="160"/' "$trx" > "$t/skip.trx"
  check_trx "$t/skip.trx" 1 >/dev/null 2>&1 && { echo "FAIL: skipped tests accepted"; fail=1; }
  sed 's/passed="166" failed="0"/passed="165" failed="1"/' "$trx" > "$t/red.trx"
  check_trx "$t/red.trx" 1 >/dev/null 2>&1 && { echo "FAIL: a failed test accepted"; fail=1; }
  check_trx "$t/missing.trx" 1 >/dev/null 2>&1 && { echo "FAIL: missing results accepted"; fail=1; }
  printf '<TestRun/>\n' > "$t/empty.trx"
  check_trx "$t/empty.trx" 0 >/dev/null 2>&1 && { echo "FAIL: results without counters accepted"; fail=1; }

  # argument handling
  ( parse_args --arch sparc >/dev/null 2>&1 ) && { echo "FAIL: unknown arch accepted by the parser"; fail=1; }
  ( parse_args --bogus x >/dev/null 2>&1 ) && { echo "FAIL: unknown argument accepted"; fail=1; }

  rm -rf "$t"
  [ "$fail" = 0 ] && echo "release_sidecar self-test: ok"
  return "$fail"
}

cmd="${1:-}"
case "$cmd" in
  --self-test) self_test ;;
  build) shift; build "$@" ;;
  hygiene) shift; hygiene "$@" ;;
  smoke) shift; smoke "$@" ;;
  strip) shift; strip_pair "$@" ;;
  test) shift; run_tests "$@" ;;
  package) shift; package "$@" ;;
  *) echo "usage: $0 build|hygiene|smoke|strip|test|package ... | --self-test" >&2; exit 2 ;;
esac
