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
#   release_sidecar.sh live-guard --src DIR [--results DIR]
#   release_sidecar.sh check   --arch ARCH --workspace DIR --pair DIR --debug DIR
#                              --results DIR
#   release_sidecar.sh package --arch ARCH --src DIR --pair DIR --version V
#                              --source-sha SHA --out DIR [--debug DIR]
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
#   strip    debug files split off into --debug, then hygiene and smoke
#            again on the stripped pair.
#   test     the xunit suite of aspnetcore/ in the pinned .NET SDK images, with
#            a locked restore (the lock files of the test project). With --pair
#            the pair is placed where the sidecar finds it and the live nginx
#            tests must run (PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1): a missing
#            pair fails them instead of skipping them. Every test must pass,
#            none may be skipped, and at least SIDECAR_MIN_TESTS (default 201)
#            must have run, per framework. With --pair, the results must also
#            show every test of InverseModeIntegrationTests, at least
#            SIDECAR_LIVE_TESTS (default 8), passed with the line
#            `live-nginx: <path of the bundled nginx>` in its output: the proof
#            that it ran against the pair rather than returned early.
#   live-guard  the negative control of that proof: the live tests with
#            PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1 and no pair must all fail.
#
# `check` runs the whole sequence on a fresh build: build, hygiene, smoke,
# strip, hygiene, smoke, test (both frameworks, live nginx).
#
# `package` writes pagespeed-sidecar-native-<version>-linux-<arch>.tar.gz:
# nginx, ngx_pagespeed_module.so, pagespeed-nginx-launch, build-info.json
# (versions, the compilers that built the pair, and the sha256 of each binary)
# and the license files; with --debug also
# pagespeed-sidecar-native-debug-<version>-linux-<arch>.tar.gz, the debug files
# of the same binaries.
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
  local ngxver uid gid logs attempt rc mem_args=()
  ngxver="$(focal_nginx_version "$WS")"
  # A build runner's slot budget (tools/ci/slot-budget.sh: CI_SLOT_CPUS,
  # CI_SLOT_MEM_GB) caps the build's parallelism and the container's memory;
  # unset (a hosted runner, a tree without the helper) leaves both uncapped.
  if [ -f "$WS/tools/ci/slot-budget.sh" ]; then
    # shellcheck disable=SC1091
    source "$WS/tools/ci/slot-budget.sh"
    slot_budget || die "build: invalid slot budget"
    [ -z "${SLOT_CPUS:-}" ] || { BAZEL_CPUS="${BAZEL_CPUS:-$SLOT_CPUS}"; MAKE_JOBS="${MAKE_JOBS:-$SLOT_CPUS}"; }
    [ -z "${SLOT_MEM:-}" ] || mem_args=(--memory="$SLOT_MEM" --memory-swap="$SLOT_MEM")
  fi
  uid="$(id -u)"; gid="$(id -g)"
  logs="$(mktemp -d)"
  echo "sidecar pair: linux-${ARCH}, nginx ${ngxver}, module $(tree_version "$WS"), image ${FOCAL_IMAGE}"

  for attempt in 1 2 3; do
    rm -f "$OUT/nginx" "$OUT/ngx_pagespeed_module.so" "$OUT/pagespeed-nginx-launch"
    # Root inside the container: the recipe installs its toolchain with apt.
    # HOME and the Bazel output root live inside the container; the recipe's
    # nginx tree under the workspace is removed again, and the workspace (Bazel
    # adds to the repository cache under vendor/) and the outputs are handed
    # back to the runner user, so that a later checkout can clear them.
    if docker run --rm --init --platform "$(platform_of "$ARCH")" ${mem_args[@]+"${mem_args[@]}"} \
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
          chown -R "$HOST_UID:$HOST_GID" /workspace /out 2>/dev/null || true
        }
        trap cleanup EXIT
        echo "--- container arch: $(uname -m), glibc $(ldd --version | head -1 | grep -oE "[0-9]+\.[0-9]+$") ---"
        install/nginx/build_module_in_container.sh focal -o /out/ngx_pagespeed_module.so
        cd "/workspace/.ngx-module-build/focal/nginx-${NGXVER}"
        make -f objs/Makefile objs/nginx -j"${MAKE_JOBS:-${CI_SLOT_CPUS:-$(nproc)}}"
        install -m 755 objs/nginx /out/nginx
        cd /workspace
        echo "--- pagespeed-nginx-launch ---"
        gcc-13 -O2 -Wall -o /out/pagespeed-nginx-launch install/nginx/pagespeed-nginx-launch.c
        chmod 755 /out/pagespeed-nginx-launch
        chmod 644 /out/ngx_pagespeed_module.so
        # The compilers of this build, for build-info.json.
        {
          echo "clang=$(clang --version | head -1)"
          echo "gcc=$(gcc-13 --version | head -1)"
        } > /out/toolchain.txt
        cat /out/toolchain.txt
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

# check_live FILE RID MIN: every InverseModeIntegrationTests result passed and
# wrote the live marker naming the bundled nginx of RID, and there are at
# least MIN of them.
check_live() {
  python3 -I - "$1" "$2" "$3" <<'PY'
import sys
import xml.etree.ElementTree as ET
path, rid, minimum = sys.argv[1], sys.argv[2], int(sys.argv[3])
want = "runtimes/%s/native/nginx" % rid
live, other = [], []
for r in ET.parse(path).getroot().iter():
    if not r.tag.endswith("}UnitTestResult"):
        continue
    name = r.get("testName", "")
    if ".InverseModeIntegrationTests." not in name:
        continue
    out = "".join(e.text or "" for e in r.iter() if e.tag.endswith("}StdOut"))
    marked = any(l.strip().startswith("live-nginx: ") and l.strip().endswith(want) for l in out.splitlines())
    (live if r.get("outcome") == "Passed" and marked else other).append(name.rsplit(".", 1)[-1])
print("live nginx tests: %d ran against the bundled pair (%s)" % (len(live), ", ".join(sorted(live))))
if other:
    print("::error::live tests that did not run against the bundled pair: " + ", ".join(sorted(other)))
    sys.exit(1)
if len(live) < minimum:
    print("::error::only %d live nginx tests ran, expected at least %d" % (len(live), minimum))
    sys.exit(1)
PY
}

# run_suite WORK IMAGE FW PROPS RID REQUIRE FILTER TRX: build and run the test
# project of the aspnetcore copy in WORK in the SDK image, as the runner user.
# Returns the status of dotnet test.
run_suite() {
  local work="$1" image="$2" fw="$3" props="$4" rid="$5" require="$6" filter="$7" trx="$8" platform=""
  [ -z "$ARCH" ] || platform="$(platform_of "$ARCH")"
  docker run --rm --init ${platform:+--platform "$platform"} \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e FW="$fw" -e PROPS="$props" -e RID="$rid" -e REQUIRE="$require" -e FILTER="$filter" -e TRX="$trx" \
    -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 -e DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    -v "$work:/w" -v "$RESULTS:/results" -w /w \
    "$image" bash -c '
      set -euo pipefail
      if [ -n "$RID" ]; then
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
      exec setpriv --reuid="$HOST_UID" --regid="$HOST_GID" --clear-groups \
        env HOME=/tmp/home FW="$FW" PROPS="$PROPS" RID="$RID" REQUIRE="$REQUIRE" FILTER="$FILTER" TRX="$TRX" bash -c "
          set -euo pipefail
          proj=WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj
          # A package that differs from the lock file fails the restore.
          dotnet build \$proj -c Release -f \$FW -p:RestoreLockedMode=true \$PROPS
          if [ -n \"\$RID\" ]; then
            dest=WeAmp.PageSpeed.AspNetCore.Tests/bin/Release/\$FW/runtimes/\$RID/native
            mkdir -p \"\$dest\"
            cp -a .pair/. \"\$dest/\"
            ls -l \"\$dest\"
          fi
          if [ \"\$REQUIRE\" = 1 ]; then export PAGESPEED_SIDECAR_REQUIRE_LIVE_NGINX=1; fi
          set -- --logger \"trx;LogFileName=\$TRX\" --logger \"console;verbosity=normal\" --results-directory /results
          if [ -n \"\$FILTER\" ]; then set -- \"\$@\" --filter \"\$FILTER\"; fi
          dotnet test \$proj -c Release -f \$FW --no-build \$PROPS \"\$@\"
        "
    '
}

# framework_setup FW: sets IMAGE and PROPS for a framework.
framework_setup() {
  case "$1" in
    net8.0) IMAGE="$DOTNET_IMAGE_NET8"; PROPS="" ;;
    # The projects target net8.0 unless IncludeNet10 adds net10.0.
    net10.0) IMAGE="$DOTNET_IMAGE_NET10"; PROPS="-p:IncludeNet10=true" ;;
    *) die "test: unknown framework '$1' (net8.0 or net10.0)" ;;
  esac
}

# fresh_copy: a copy of aspnetcore/ (and of the pair, if any) to build in: no
# build output of the tree or of another framework is reused, and the tree
# itself is never written to.
fresh_copy() {
  local work
  work="$(mktemp -d)"
  cp -a "$SRC/aspnetcore/." "$work/"
  rm -rf "$work"/*/bin "$work"/*/obj
  if [ -n "$PAIR" ]; then
    mkdir -p "$work/.pair"
    cp -a "$PAIR/." "$work/.pair/"
  fi
  printf '%s\n' "$work"
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
  local min="${SIDECAR_MIN_TESTS:-201}" live_min="${SIDECAR_LIVE_TESTS:-8}" rid="" work fw require=0 fail=0
  [ -z "$PAIR" ] || { rid="$(rid_of "$ARCH")"; require=1; }
  [ -n "$RESULTS" ] || RESULTS="$(mktemp -d)"
  mkdir -p "$RESULTS"; RESULTS="$(abs_dir "$RESULTS")"

  for fw in "${FRAMEWORKS[@]}"; do
    framework_setup "$fw"
    work="$(fresh_copy)"
    echo "=== sidecar tests: ${fw}${rid:+, live nginx linux-${ARCH}} (${IMAGE%%@*}) ==="
    if ! run_suite "$work" "$IMAGE" "$fw" "$PROPS" "$rid" "$require" "" "sidecar-${fw}.trx"; then
      echo "::error::the sidecar tests failed (${fw})"
      fail=1
    fi
    # The build ran as the runner user, so the copy can be removed.
    rm -rf "$work"
    check_trx "$RESULTS/sidecar-${fw}.trx" "$min" || fail=1
    if [ -n "$rid" ] && [ -s "$RESULTS/sidecar-${fw}.trx" ]; then
      check_live "$RESULTS/sidecar-${fw}.trx" "$rid" "$live_min" || fail=1
    fi
  done
  [ "$fail" = 0 ] || die "test: the sidecar test suite did not pass"
  echo "release_sidecar: tests ok (${FRAMEWORKS[*]}${rid:+, live nginx linux-${ARCH}})"
}

# The negative control of check_live: with the live tier required and no pair,
# every live test must fail. A run where they pass would mean a live test can
# pass without nginx, and the marker count would prove nothing.
live_guard() {
  parse_args "$@" || return $?
  need_src
  [ -z "$PAIR" ] || die "live-guard: runs without a pair"
  local live_min="${SIDECAR_LIVE_TESTS:-8}" work total passed failed rc=0
  [ -n "$RESULTS" ] || RESULTS="$(mktemp -d)"
  mkdir -p "$RESULTS"; RESULTS="$(abs_dir "$RESULTS")"
  framework_setup net8.0
  work="$(fresh_copy)"
  echo "=== sidecar live-guard: the live tests, required, with no pair (expected to fail) ==="
  run_suite "$work" "$IMAGE" net8.0 "$PROPS" "" 1 "FullyQualifiedName~InverseModeIntegrationTests" \
    sidecar-live-guard.trx || rc=$?
  rm -rf "$work"
  local f="$RESULTS/sidecar-live-guard.trx"
  [ -s "$f" ] || die "live-guard: no test results (the build failed?)"
  total="$(trx_counter "$f" total)"; passed="$(trx_counter "$f" passed)"; failed="$(trx_counter "$f" failed)"
  echo "live-guard: total=${total:-?} passed=${passed:-?} failed=${failed:-?} (dotnet test exit ${rc})"
  [ "$rc" -ne 0 ] || die "live-guard: the live tests passed without a pair"
  [ "${passed:-1}" = 0 ] && [ -n "$total" ] && [ "${failed:-0}" = "$total" ] && [ "$total" -ge "$live_min" ] \
    || die "live-guard: expected all of at least ${live_min} live tests to fail"
  echo "release_sidecar: live-guard ok (${failed} live tests fail without the pair)"
}

# check: the whole sequence on a fresh build, as the callers run it.
check_all() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] && [ -n "$WS" ] && [ -n "$PAIR" ] && [ -n "$DEBUG_DIR" ] && [ -n "$RESULTS" ] \
    || die "check: --arch, --workspace, --pair, --debug and --results are required"
  local self a="$ARCH" w="$WS" p="$PAIR" d="$DEBUG_DIR" r="$RESULTS"
  self="${BASH_SOURCE[0]}"
  step() { echo "::group::$1"; shift; bash "$self" "$@" || { echo "::endgroup::"; die "check: $* failed"; }; echo "::endgroup::"; }
  step "Build the pair" build --arch "$a" --workspace "$w" --out "$p"
  step "Symbol and glibc gate" hygiene --arch "$a" --src "$w" --pair "$p"
  step "Load, serve and optimize on Debian 11 and 12" smoke --arch "$a" --src "$w" --pair "$p"
  step "Strip the pair" strip --arch "$a" --pair "$p" --debug "$d"
  step "Symbol and glibc gate, stripped" hygiene --arch "$a" --src "$w" --pair "$p"
  step "Load, serve and optimize on Debian 11 and 12, stripped" smoke --arch "$a" --src "$w" --pair "$p"
  step "Sidecar tests with the live nginx (.NET 8 and 10)" test --arch "$a" --src "$w" --pair "$p" --results "$r"
  echo "release_sidecar: check ok (linux-${a})"
}

# --- package -----------------------------------------------------------------

package() {
  parse_args "$@" || return $?
  [ -n "$ARCH" ] && [ -n "$VERSION" ] && [ -n "$SOURCE_SHA" ] && [ -n "$OUT" ] \
    || die "package: --arch, --version, --source-sha and --out are required"
  need_src; need_pair
  printf '%s' "$SOURCE_SHA" | grep -Eq '^[0-9a-f]{40}$' || die "package: --source-sha is not a commit id"
  printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || die "package: bad --version '$VERSION'"
  local rid ngxver tree name stage clang gcc
  rid="$(rid_of "$ARCH")"
  [ -s "$PAIR/toolchain.txt" ] || die "package: the pair has no toolchain.txt (built by an older build?)"
  clang="$(sed -n 's/^clang=//p' "$PAIR/toolchain.txt")"
  gcc="$(sed -n 's/^gcc=//p' "$PAIR/toolchain.txt")"
  [ -n "$clang" ] && [ -n "$gcc" ] || die "package: toolchain.txt names no clang or gcc"
  case "$clang$gcc" in *\"*|*\\*) die "package: unexpected characters in toolchain.txt" ;; esac
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
  # wherever the script runs. Every entry carries the time SOURCE_DATE_EPOCH
  # (default 0, the epoch), so the archive depends on its contents only.
  docker run --rm --platform "$(platform_of "$ARCH")" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e NAME="$name" -e RID="$rid" -e VERSION="$VERSION" -e NGXVER="$ngxver" \
    -e SOURCE_SHA="$SOURCE_SHA" -e IMAGE="$FOCAL_IMAGE" -e GLIBC_FLOOR="$GLIBC_FLOOR" \
    -e CLANG="$clang" -e GCC="$gcc" -e EPOCH="${SOURCE_DATE_EPOCH:-0}" \
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
        printf "  \"clang_version\": \"%s\",\n" "$CLANG"
        printf "  \"gcc_version\": \"%s\",\n" "$GCC"
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
  ls -l "$OUT/${name}.tar.gz"
  if [ -n "$DEBUG_DIR" ]; then
    # The debug files of the same binaries, released beside them so that a
    # crash of a released pair can be symbolized after the run is gone.
    local dname="pagespeed-sidecar-native-debug-${VERSION}-${rid}" f
    mkdir -p "$stage/$dname"
    for f in "${PAIR_FILES[@]}"; do
      [ -s "$DEBUG_DIR/$f.debug" ] || die "package: no debug file $DEBUG_DIR/$f.debug"
      cp "$DEBUG_DIR/$f.debug" "$stage/$dname/"
    done
    docker run --rm --platform "$(platform_of "$ARCH")" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
      -e NAME="$dname" -e EPOCH="${SOURCE_DATE_EPOCH:-0}" \
      -v "$stage:/s" -v "$OUT:/o" -w /s "$FOCAL_IMAGE" bash -c '
        set -euo pipefail
        trap "chown -R \"$HOST_UID:$HOST_GID\" /o /s" EXIT
        chmod 644 "$NAME"/*
        tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@${EPOCH}" \
          -cf - "$NAME" | gzip -9n > "/o/${NAME}.tar.gz"
        tar -tvzf "/o/${NAME}.tar.gz"
      '
    ls -l "$OUT/${dname}.tar.gz"
  fi
  rm -rf "$stage"
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

  # the live proof: every live test passed and wrote the marker of the pair
  mk_live() { # FILE OUTCOME2 MARKER2
    {
      printf '<TestRun xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010"><Results>\n'
      printf '<UnitTestResult testName="X.InverseModeIntegrationTests.A" outcome="Passed"><Output><StdOut>live-nginx: /w/bin/runtimes/linux-x64/native/nginx</StdOut></Output></UnitTestResult>\n'
      printf '<UnitTestResult testName="X.InverseModeIntegrationTests.B" outcome="%s"><Output><StdOut>%s</StdOut></Output></UnitTestResult>\n' "$2" "$3"
      printf '<UnitTestResult testName="X.OtherTests.C" outcome="Passed"></UnitTestResult>\n'
      printf '</Results></TestRun>\n'
    } > "$1"
  }
  mk_live "$t/live.trx" Passed "live-nginx: /w/bin/runtimes/linux-x64/native/nginx"
  check_live "$t/live.trx" linux-x64 2 >/dev/null 2>&1 || { echo "FAIL: a live run refused"; fail=1; }
  check_live "$t/live.trx" linux-x64 3 >/dev/null 2>&1 && { echo "FAIL: too few live tests accepted"; fail=1; }
  check_live "$t/live.trx" linux-arm64 1 >/dev/null 2>&1 && { echo "FAIL: the pair of another arch accepted"; fail=1; }
  mk_live "$t/nomark.trx" Passed ""
  check_live "$t/nomark.trx" linux-x64 1 >/dev/null 2>&1 && { echo "FAIL: a live test without the marker accepted"; fail=1; }
  mk_live "$t/host.trx" Passed "live-nginx: /usr/sbin/nginx"
  check_live "$t/host.trx" linux-x64 1 >/dev/null 2>&1 && { echo "FAIL: a host nginx accepted as the pair"; fail=1; }
  mk_live "$t/red-live.trx" Failed "live-nginx: /w/bin/runtimes/linux-x64/native/nginx"
  check_live "$t/red-live.trx" linux-x64 1 >/dev/null 2>&1 && { echo "FAIL: a failed live test accepted"; fail=1; }

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
  live-guard) shift; live_guard "$@" ;;
  check) shift; check_all "$@" ;;
  package) shift; package "$@" ;;
  *) echo "usage: $0 build|hygiene|smoke|strip|test|live-guard|check|package ... | --self-test" >&2; exit 2 ;;
esac
