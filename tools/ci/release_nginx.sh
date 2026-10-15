#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The nginx distro packages of the module release lane (docs/ci/release.md).
#
#   release_nginx.sh build   --arch x64|arm64 --set main|el10 --workspace DIR --out DIR
#   release_nginx.sh symbol-leak --arch ARCH --src DIR --pkgdir DIR
#   release_nginx.sh routing     --arch ARCH --pkgdir DIR [--set main|el10]
#   release_nginx.sh load-check  --arch ARCH --src DIR --pkgdir DIR [--set main|el10]
#   release_nginx.sh suite       --arch x64  --src DIR --pkgdir DIR
#   release_nginx.sh --self-test
#
# `build` compiles the nginx module once per target distro INSIDE a container
# of that distro (the module's glibc and libstdc++ ABI must match the target)
# and wraps the result into a deb or rpm. The legs run as concurrent docker
# processes, each in its own copy of the prepared workspace (concurrent Bazel
# and nginx builds in one workspace would collide), bounded by a core and
# memory budget. The packages of all legs end up in --out.
#
# Sizing (environment, all optional):
#   CORES_PER_LEG    cores one leg may use (default 8 on x64, 6 on arm64,
#                    never more than the host has)
#   MEM_PER_CORE_GB  expected peak memory per core (default 1.5)
#   RAM_BUDGET_GB    memory the legs may use together (default 90% of the host)
# The number of concurrent legs is the smaller of what the memory budget and
# the core count allow. On a small runner that is one leg at a time.
#
# The gates (symbol-leak, routing, load-check, suite) work on the packages in
# --pkgdir and on the tree in --src, which holds test/package-test/.
set -euo pipefail

# Distro legs: name|image|nginx version|nginx source sha256|deb or rpm.
# The nginx version is the distro's stock nginx; a dynamic module only loads
# into the exact nginx version it was built against.
legs_for() {
  local arch="$1" set="$2"
  case "$set" in
    main)
      if [ "$arch" = x64 ]; then
        cat <<'EOF'
noble|ubuntu:24.04|1.24.0|77a2541637b92a621e3ee76776c8b7b40cf6d707e69ba53a940283e30ff2f55d|deb
bookworm|debian:12|1.22.1|9ebb333a9e82b952acd3e2b4aeb1d4ff6406f72491bab6cd9fe69f0dea737f31|deb
trixie|debian:13|1.26.3|69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa|deb
jammy|ubuntu:22.04|1.18.0|4c373e7ab5bf91d34a4f11a0c9496561061ba5eee6020db272a17a7228d35f99|deb
el9|almalinux:9|1.20.1|e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49|rpm
EOF
      else
        cat <<'EOF'
bookworm|debian:12|1.22.1|9ebb333a9e82b952acd3e2b4aeb1d4ff6406f72491bab6cd9fe69f0dea737f31|deb
trixie|debian:13|1.26.3|69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa|deb
jammy|ubuntu:22.04|1.18.0|4c373e7ab5bf91d34a4f11a0c9496561061ba5eee6020db272a17a7228d35f99|deb
noble|ubuntu:24.04|1.24.0|77a2541637b92a621e3ee76776c8b7b40cf6d707e69ba53a940283e30ff2f55d|deb
el9|almalinux:9|1.20.1|e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49|rpm
EOF
      fi ;;
    el10)
      [ "$arch" = x64 ] || { echo "release_nginx: no el10 leg for $arch" >&2; return 1; }
      echo 'el10|almalinux:10|1.26.3|69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa|rpm' ;;
    *) echo "release_nginx: unknown set '$set'" >&2; return 1 ;;
  esac
}

# Host memory in whole GB (Linux).
host_mem_gb() {
  if [ -r /proc/meminfo ]; then
    awk '/^MemTotal:/ { printf "%d", $2 / 1024 / 1024 }' /proc/meminfo
  else
    echo 16
  fi
}

# plan CORES_PER_LEG MEM_PER_CORE_GB RAM_BUDGET_GB NPROC NLEGS
# Prints "cores leg_mem_gb concurrency".
plan() {
  local cores="$1" per_core="$2" budget="$3" nproc="$4" nlegs="$5" leg_mem conc_ram conc_cpu conc
  [ "$cores" -gt "$nproc" ] && cores="$nproc"
  [ "$cores" -lt 1 ] && cores=1
  leg_mem=$(awk -v c="$cores" -v m="$per_core" 'BEGIN { printf "%d", (c * m) + 2 }')
  conc_ram=$(( budget / leg_mem )); [ "$conc_ram" -lt 1 ] && conc_ram=1
  conc_cpu=$(( nproc / cores ));    [ "$conc_cpu" -lt 1 ] && conc_cpu=1
  conc=$(( conc_ram < conc_cpu ? conc_ram : conc_cpu ))
  [ "$conc" -gt "$nlegs" ] && conc="$nlegs"
  echo "$cores $leg_mem $conc"
}

# Signatures of a failed download that a retry fixes (a corrupt archive, a
# reset connection). Anything else, a compile or link error, is not retried.
TRANSIENT_FETCH_RE='Hash Sum mismatch|Unable to fetch some archives|bad record MAC|Connection reset|Temporary failure resolving|unexpected EOF|could not download Bazel'

build() {
  local arch="" set="main" ws="" out=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) arch="$2"; shift 2 ;;
      --set) set="$2"; shift 2 ;;
      --workspace) ws="$2"; shift 2 ;;
      --out) out="$2"; shift 2 ;;
      *) echo "release_nginx build: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$arch" ] && [ -d "$ws" ] && [ -n "$out" ] || { echo "release_nginx build: --arch, --workspace and --out are required" >&2; return 2; }
  ws="$(cd "$ws" && pwd)"
  mkdir -p "$out"; out="$(cd "$out" && pwd)"

  local legs=() row
  while IFS= read -r row; do legs+=("$row"); done < <(legs_for "$arch" "$set")

  local default_cores=8
  [ "$arch" = arm64 ] && default_cores=6
  local nproc_n budget p cores leg_mem conc
  nproc_n="$(nproc)"
  budget="${RAM_BUDGET_GB:-}"
  [ -n "$budget" ] || budget=$(( $(host_mem_gb) * 9 / 10 ))
  p="$(plan "${CORES_PER_LEG:-$default_cores}" "${MEM_PER_CORE_GB:-1.5}" "$budget" "$nproc_n" "${#legs[@]}")"
  read -r cores leg_mem conc <<<"$p"
  echo "fan-out plan: nproc=$nproc_n cores/leg=$cores mem/leg=${leg_mem}g budget=${budget}g -> concurrency=$conc over ${#legs[@]} legs"

  local extra_docker=()
  if [ -n "${BAZEL_CACHE_HOST:-}" ]; then
    extra_docker+=(--add-host="bazel-remote-cache:${BAZEL_CACHE_HOST}")
  fi
  local cache_dir="${CACHE_DIR:-}"
  if [ -z "$cache_dir" ]; then cache_dir="$(mktemp -d)"; fi
  mkdir -p "$cache_dir"
  local logs; logs="$(mktemp -d)"

  # run_leg ROW: one leg in its own workspace copy. The caller of this
  # function is on the left of `||`, which suppresses errexit inside it, so
  # every step that must abort the leg carries an explicit `|| return N`.
  run_leg() {
    local row="$1" distro image ngxver ngxsha pkg wsdir wrap n
    IFS='|' read -r distro image ngxver ngxsha pkg <<<"$row" || return 92
    wsdir="${ws}-leg-${distro}"
    for n in 1 2 3 4 5; do
      rm -rf "$wsdir" 2>/dev/null || true
      [ ! -e "$wsdir" ] && break
      [ "$n" -lt 5 ] && sleep 3
    done
    if [ -e "$wsdir" ]; then
      # A previous run packaged as root inside the container, so the runner
      # user cannot delete what it left. Clear it through a throwaway root
      # container of the leg's own image.
      docker run --rm -v "$wsdir:/w" "${image}" sh -c 'rm -rf /w/* /w/.[!.]* 2>/dev/null; true' >/dev/null 2>&1 || true
      rmdir "$wsdir" 2>/dev/null || true
    fi
    if [ -e "$wsdir" ]; then
      echo "::error::leg '${distro}': could not clear its workspace"
      return 90
    fi
    cp -a "$ws" "$wsdir" || { echo "::error::leg '${distro}': could not seed its workspace"; return 91; }
    if [ "$pkg" = rpm ]; then
      if [ "$arch" = arm64 ]; then
        wrap="./install/nginx/build_nginx_rpm.sh -a aarch64 -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so -d .${distro}"
      else
        wrap="./install/nginx/build_nginx_rpm.sh -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so -d .${distro}"
      fi
    else
      if [ "$arch" = arm64 ]; then
        wrap="./install/nginx/build_nginx_deb.sh -a arm64 -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so"
      else
        wrap="./install/nginx/build_nginx_deb.sh -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so"
      fi
    fi
    # Root inside the container: apt and dnf install the build toolchain of
    # the distro. On success the leg copies its packages to /collect and
    # empties its own workspace copy, so a runner with little disk never
    # holds more than the running legs.
    docker run --rm --init ${extra_docker[@]+"${extra_docker[@]}"} \
      --cpus="${cores}" --memory="${leg_mem}g" --memory-swap="${leg_mem}g" \
      -e BAZEL_CPUS="${cores}" -e MAKE_JOBS="${cores}" \
      -e HOME=/workspace \
      -e NGINX_SRC_VERSION="${ngxver}" \
      -e NGINX_SRC_SHA256="${ngxsha}" \
      -v "$wsdir":/workspace \
      -v "$cache_dir":/cache \
      -v "$out":/collect \
      -w /workspace \
      "${image}" \
      bash -c '
        set -e
        export BAZEL_DISK_CACHE=/cache
        mkdir -p /workspace/nginx-packages
        ./install/nginx/build_module_in_container.sh '"${distro}"' \
          -o /workspace/ngx_pagespeed_module_'"${distro}"'.so
        '"${wrap}"'
        cp -a /workspace/nginx-packages/. /collect/
        rm -rf /workspace/* /workspace/.[!.]*
      ' >"${logs}/${distro}.log" 2>&1
  }

  run_leg_retrying() {
    local row="$1" distro attempt rc
    distro="${row%%|*}"
    for attempt in 1 2 3; do
      rc=0
      run_leg "$row" || rc=$?
      [ "$rc" -eq 0 ] && return 0
      # 90 to 92 are the leg's own workspace preparation failures; the log
      # then belongs to the previous attempt.
      if [ "$rc" -ge 90 ] && [ "$rc" -le 92 ]; then return "$rc"; fi
      if [ "$attempt" -ge 3 ] || ! grep -qE "$TRANSIENT_FETCH_RE" "${logs}/${distro}.log" 2>/dev/null; then
        return "$rc"
      fi
      echo "::warning::leg '${distro}' hit a transient fetch failure, retry $((attempt + 1))/3"
      sleep $((attempt * 15))
    done
    return "$rc"
  }

  local pids=() names=() running=0
  for row in "${legs[@]}"; do
    echo "=== launching nginx leg: ${row%%|*} ($arch) ==="
    run_leg_retrying "$row" &
    pids+=("$!"); names+=("${row%%|*}")
    running=$((running + 1))
    if [ "$running" -ge "$conc" ]; then
      wait -n || true
      running=$((running - 1))
    fi
  done

  # Reap every leg and inspect every exit status before deciding: a failing
  # leg must not be masked by the others.
  local fail=0 i rc
  for i in "${!pids[@]}"; do
    if wait "${pids[$i]}"; then rc=0; else rc=$?; fi
    echo "=== leg ${names[$i]} exit=${rc} ==="
    echo "----- tail of the ${names[$i]} log -----"
    tail -n 60 "${logs}/${names[$i]}.log" 2>/dev/null || echo "(no log)"
    echo "----- end ${names[$i]} -----"
    if [ "$rc" -ne 0 ]; then
      echo "::error::nginx distro leg '${names[$i]}' ($arch) failed (exit ${rc})"
      fail=1
    fi
  done
  [ "$fail" -eq 0 ] || { echo "::error::one or more nginx distro legs failed"; return 1; }
  echo "=== collected nginx distro packages ($arch) ==="
  ls -lh "$out"
}

# --- gates -------------------------------------------------------------------

gate_args() {
  ARCH=""; SRC=""; PKGDIR=""; SET="main"
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) ARCH="$2"; shift 2 ;;
      --src) SRC="$2"; shift 2 ;;
      --pkgdir) PKGDIR="$2"; shift 2 ;;
      --set) SET="$2"; shift 2 ;;
      *) echo "release_nginx: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$ARCH" ] && [ -d "$PKGDIR" ] || { echo "release_nginx: --arch and --pkgdir are required" >&2; return 2; }
  PKGDIR="$(cd "$PKGDIR" && pwd)"
  if [ -n "$SRC" ]; then SRC="$(cd "$SRC" && pwd)"; fi
  case "$ARCH" in
    x64) DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
    arm64) DEB_ARCH=arm64; RPM_ARCH=aarch64 ;;
    *) echo "release_nginx: unknown arch $ARCH" >&2; return 2 ;;
  esac
}

# first_of PATH...: the first path that exists (the arguments come from a glob).
first_of() {
  local f
  for f in "$@"; do
    if [ -e "$f" ]; then printf '%s\n' "$f"; return 0; fi
  done
  echo "::error::no package matches the expected name" >&2
  return 1
}

# The suites the debs are built for and the images that carry their stock nginx.
DEB_SUITES=("noble|ubuntu:24.04" "bookworm|debian:12" "trixie|debian:13" "jammy|ubuntu:22.04")

# The symbol-leak check needs readelf and, for an rpm, cpio: run it in a
# container of the package's own family so the runner needs neither.
symbol_leak() {
  gate_args "$@" || return $?
  [ -n "$SRC" ] || { echo "release_nginx: --src is required" >&2; return 2; }
  shopt -s nullglob
  local debs=("$PKGDIR"/nginx-module-pagespeed_*_"${DEB_ARCH}".deb)
  local rpms=("$PKGDIR"/nginx-module-pagespeed-*."${RPM_ARCH}".rpm)
  if [ "${#debs[@]}" -eq 0 ] && [ "${#rpms[@]}" -eq 0 ]; then
    echo "::error::no nginx module packages to check for leaked symbols"
    return 1
  fi
  if [ "${#debs[@]}" -gt 0 ]; then
    docker run --rm -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro ubuntu:24.04 bash -c '
      set -e
      apt-get update -qq >/dev/null && apt-get install -y -qq binutils >/dev/null
      shopt -s nullglob
      for p in /pkgs/nginx-module-pagespeed_*_'"${DEB_ARCH}"'.deb; do
        echo "=== symbol-leak gate: $(basename "$p") ==="
        /src/test/package-test/check-nginx-symbol-leak.sh "$p"
      done'
  fi
  if [ "${#rpms[@]}" -gt 0 ]; then
    docker run --rm -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro almalinux:9 bash -c '
      set -e
      dnf install -y -q cpio binutils >/dev/null
      shopt -s nullglob
      for p in /pkgs/nginx-module-pagespeed-*.'"${RPM_ARCH}"'.rpm; do
        echo "=== symbol-leak gate: $(basename "$p") ==="
        /src/test/package-test/check-nginx-symbol-leak.sh "$p"
      done'
  fi
}

# Every rpm must carry the dist tag of its yum tree in its name and none may
# carry the cPanel prefix: the consumer routes by name.
routing() {
  gate_args "$@" || return $?
  local dist=el9
  [ "$SET" = el10 ] && dist=el10
  shopt -s nullglob
  local rpms=("$PKGDIR"/*.rpm) r base
  [ "${#rpms[@]}" -gt 0 ] || { echo "::error::no nginx rpm to check"; return 1; }
  for r in "${rpms[@]}"; do
    base="$(basename "$r")"
    case "$base" in
      ea-*) echo "::error::nginx rpm '$base' has the cPanel prefix"; return 1 ;;
    esac
    case "$base" in
      *."${dist}"."${RPM_ARCH}".rpm) echo "OK: $base routes to ${dist}/${RPM_ARCH}" ;;
      *) echo "::error::nginx rpm '$base' is not .${dist}.${RPM_ARCH}"; return 1 ;;
    esac
  done
}

# Load the module into the STOCK packaged nginx of each suite.
load_check() {
  gate_args "$@" || return $?
  [ -n "$SRC" ] || { echo "release_nginx: --src is required" >&2; return 2; }
  local row distro image deb rpm
  if [ "$SET" = el10 ]; then
    rpm="$(first_of "$PKGDIR"/nginx-module-pagespeed-*.el10."${RPM_ARCH}".rpm)"
    echo "=== load check: el10 - $(basename "$rpm") ==="
    docker run --rm --init -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro -e DISTRO=el10 almalinux:10 bash -c '
      set -e
      cp /pkgs/'"$(basename "$rpm")"' /tmp/pkg.rpm
      /src/test/package-test/check-nginx-packaged-load.sh el10 /tmp/pkg.rpm'
    return 0
  fi
  for row in "${DEB_SUITES[@]}"; do
    IFS='|' read -r distro image <<<"$row"
    deb="$(first_of "$PKGDIR"/nginx-module-pagespeed_*~"${distro}"_"${DEB_ARCH}".deb)"
    echo "=== load check: ${distro} (${image}) - $(basename "$deb") ==="
    docker run --rm --init -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro -e DISTRO="${distro}" "${image}" bash -c '
      set -e
      cp /pkgs/'"$(basename "$deb")"' /tmp/pkg.deb
      /src/test/package-test/check-nginx-packaged-load.sh '"${distro}"' /tmp/pkg.deb'
  done
  rpm="$(first_of "$PKGDIR"/nginx-module-pagespeed-*.el9."${RPM_ARCH}".rpm)"
  echo "=== load check: el9 - $(basename "$rpm") ==="
  docker run --rm --init -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro -e DISTRO=el9 almalinux:9 bash -c '
    set -e
    cp /pkgs/'"$(basename "$rpm")"' /tmp/pkg.rpm
    /src/test/package-test/check-nginx-packaged-load.sh el9 /tmp/pkg.rpm'
}

# Install each package into its stock nginx and run the nginx test suite.
suite() {
  gate_args "$@" || return $?
  [ -n "$SRC" ] || { echo "release_nginx: --src is required" >&2; return 2; }
  local row distro image deb rpm
  for row in "${DEB_SUITES[@]}"; do
    IFS='|' read -r distro image <<<"$row"
    deb="$(first_of "$PKGDIR"/nginx-module-pagespeed_*~"${distro}"_"${DEB_ARCH}".deb)"
    echo "=== suite: ${distro} (${image}) - $(basename "$deb") ==="
    docker run --rm --init -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro -e DISTRO="${distro}" "${image}" bash -c '
      set -e
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y --no-install-recommends nginx curl ca-certificates python3 python3-pip >/dev/null
      pip3 install --break-system-packages --no-cache-dir pytest==8.3.5 pytest-timeout==2.3.1 >/dev/null 2>&1 \
        || pip3 install --no-cache-dir pytest==8.3.5 pytest-timeout==2.3.1 >/dev/null
      cp /pkgs/'"$(basename "$deb")"' /tmp/pkg.deb
      apt-get install -y /tmp/pkg.deb
      nginx -t
      export NGINX_BINARY=/usr/sbin/nginx
      export MODULE_PATH=/usr/lib/nginx/modules/ngx_pagespeed_module.so
      cp -r /src /work && cd /work
      bash test/package-test/run-nginx-tests.sh'
  done
  rpm="$(first_of "$PKGDIR"/nginx-module-pagespeed-*.el9."${RPM_ARCH}".rpm)"
  echo "=== suite: el9 - $(basename "$rpm") ==="
  docker run --rm --init -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro almalinux:9 bash -c '
    set -e
    dnf install -y --allowerasing --setopt=install_weak_deps=False nginx curl ca-certificates python3 python3-pip tar gzip findutils >/dev/null
    pip3 install --no-cache-dir pytest==8.3.5 pytest-timeout==2.3.1 >/dev/null
    cp /pkgs/'"$(basename "$rpm")"' /tmp/pkg.rpm
    dnf install -y /tmp/pkg.rpm
    nginx -t
    export NGINX_BINARY=/usr/sbin/nginx
    export MODULE_PATH=/usr/lib64/nginx/modules/ngx_pagespeed_module.so
    cp -r /src /work && cd /work
    bash test/package-test/run-nginx-tests.sh'
}

self_test() {
  local fail=0 t out
  t="$(mktemp -d)"
  trap 'rm -rf "$t"' RETURN
  chk() { [ "$2" = "$3" ] || { echo "FAIL: $1: got '$2', want '$3'"; fail=1; }; }

  # the leg tables
  chk "x64 main legs" "$(legs_for x64 main | wc -l | tr -d ' ')" 5
  chk "arm64 main legs" "$(legs_for arm64 main | wc -l | tr -d ' ')" 5
  chk "el10 legs" "$(legs_for x64 el10 | wc -l | tr -d ' ')" 1
  if legs_for arm64 el10 >/dev/null 2>&1; then echo "FAIL: arm64 el10 accepted"; fail=1; fi
  if legs_for x64 bogus >/dev/null 2>&1; then echo "FAIL: bogus set accepted"; fail=1; fi
  # every leg row has five fields and a 64 hex digit source hash
  while IFS='|' read -r d i v h k; do
    if [ -z "$d" ] || [ -z "$i" ] || [ -z "$v" ] || ! printf '%s' "$h" | grep -Eq '^[0-9a-f]{64}$' \
       || { [ "$k" != deb ] && [ "$k" != rpm ]; }; then
      echo "FAIL: malformed leg row for '$d'"; fail=1
    fi
  done < <(legs_for x64 main; legs_for arm64 main; legs_for x64 el10)

  # the budget arithmetic
  chk "plan big host" "$(plan 8 1.5 85 128 6)" "8 14 6"
  chk "plan small host" "$(plan 8 1.5 14 4 5)" "4 8 1"
  chk "plan tiny host" "$(plan 8 1.5 2 1 5)" "1 3 1"
  chk "plan one leg" "$(plan 8 1.5 85 128 1)" "8 14 1"

  # the routing gate
  mkdir -p "$t/ok" "$t/bad" "$t/ea" "$t/e10"
  : > "$t/ok/nginx-module-pagespeed-1.19.0-1.el9.x86_64.rpm"
  : > "$t/bad/nginx-module-pagespeed-1.19.0-1.x86_64.rpm"
  : > "$t/ea/ea-nginx-module-pagespeed-1.19.0-1.el9.x86_64.rpm"
  : > "$t/e10/nginx-module-pagespeed-1.19.0-1.el10.x86_64.rpm"
  routing --arch x64 --pkgdir "$t/ok" >/dev/null 2>&1 || { echo "FAIL: routing ok"; fail=1; }
  routing --arch x64 --pkgdir "$t/bad" >/dev/null 2>&1 && { echo "FAIL: routing bad accepted"; fail=1; }
  routing --arch x64 --pkgdir "$t/ea" >/dev/null 2>&1 && { echo "FAIL: routing ea accepted"; fail=1; }
  routing --arch x64 --pkgdir "$t/ok" --set el10 >/dev/null 2>&1 && { echo "FAIL: el9 accepted as el10"; fail=1; }
  routing --arch x64 --pkgdir "$t/e10" --set el10 >/dev/null 2>&1 || { echo "FAIL: routing el10"; fail=1; }
  routing --arch arm64 --pkgdir "$t/ok" >/dev/null 2>&1 && { echo "FAIL: x86_64 accepted as arm64"; fail=1; }
  routing --arch mips --pkgdir "$t/ok" >/dev/null 2>&1 && { echo "FAIL: unknown arch accepted"; fail=1; }
  out=""
  [ "$fail" = 0 ] && echo "release_nginx self-test: ok"
  return "$fail"
}

cmd="${1:-}"
case "$cmd" in
  --self-test) self_test ;;
  build) shift; build "$@" ;;
  symbol-leak) shift; symbol_leak "$@" ;;
  routing) shift; routing "$@" ;;
  load-check) shift; load_check "$@" ;;
  suite) shift; suite "$@" ;;
  *) echo "usage: $0 build|symbol-leak|routing|load-check|suite ... | --self-test" >&2; exit 2 ;;
esac
