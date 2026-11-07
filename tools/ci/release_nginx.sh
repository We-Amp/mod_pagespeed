#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The nginx distro packages of the module release lane (docs/ci/release.md).
#
#   release_nginx.sh legs    --arch x64|arm64 --set main|el10|nginxorg [--json]
#   release_nginx.sh build   --arch x64|arm64 --set main|el10|nginxorg --workspace DIR --out DIR [--distro NAME]
#   release_nginx.sh check-set --arch ARCH --pkgdir DIR [--set main|el10|nginxorg]
#   release_nginx.sh symbol-leak --arch ARCH --src DIR --pkgdir DIR
#   release_nginx.sh routing     --arch ARCH --pkgdir DIR [--set main|el10|nginxorg]
#   release_nginx.sh load-check  --arch ARCH --src DIR --pkgdir DIR [--set main|el10|nginxorg]
#   release_nginx.sh suite       --arch x64  --src DIR --pkgdir DIR
#   release_nginx.sh leg-row     --arch ARCH --distro NAME [--set main|el10|nginxorg]
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
#   RAM_BUDGET_GB    memory the legs may use together (default CI_SLOT_MEM_GB
#                    when the runner sets it, else 90% of the host)
#   CI_SLOT_CPUS     per-slot CPU share of the runner; bounds the core count
# The number of concurrent legs is the smaller of what the memory budget and
# the core count allow. On a small runner that is one leg at a time.
#
# The gates (symbol-leak, routing, load-check, suite) work on the packages in
# --pkgdir and on the tree in --src, which holds test/package-test/.
set -euo pipefail

# apt-get update + install with retries, for the throwaway gate containers: a
# short Debian or Ubuntu mirror outage is waited out, a lasting one still fails
# the container. The function is spliced into the container commands with
# declare -f, so it runs in the container's shell. stdout is quiet; stderr is
# never redirected.
apt_install_retry() {
  local opts=() attempt
  [ "${1:-}" = "--no-install-recommends" ] && opts=("$1") && shift
  for attempt in 1 2 3; do
    if apt-get update -qq -o Acquire::Retries=5 >/dev/null \
       && apt-get install -y -qq "${opts[@]}" -o Acquire::Retries=5 "$@" >/dev/null; then
      return 0
    fi
    if [ "$attempt" -lt 3 ]; then
      echo "::warning::release_nginx: apt install of $* failed (attempt $attempt of 3), retrying" >&2
      sleep $((attempt * 5))
    fi
  done
  echo "::error::release_nginx: apt install of $* failed after 3 attempts" >&2
  return 1
}

# dnf with retries, for the throwaway EL containers: a short mirror or
# metadata outage (a Rocky/Alma mirror serving an empty repomd.xml, a curl
# error, a 5xx) is waited out, anything else (a missing package, a conflict,
# a dependency error) fails on the first attempt. Up to 3 tries, with
# `dnf clean metadata` and a growing pause between them; a lasting outage
# still fails loud with dnf's last output. Usage: dnf_retry install -y PKG...
# or dnf_retry repoquery ... (stdout and stderr are passed through after each
# attempt). DNF_RETRY_SLEEP_UNIT (default 10) scales the pause. The function
# is spliced into container commands with declare -f, so it runs in the
# container's shell.
dnf_retry() {
  local attempt rc out err
  out="$(mktemp)" && err="$(mktemp)" || return 1
  for attempt in 1 2 3; do
    rc=0
    dnf "$@" --setopt=retries=5 --setopt=skip_if_unavailable=False >"$out" 2>"$err" || rc=$?
    cat "$out"
    cat "$err" >&2
    if [ "$rc" -eq 0 ]; then
      rm -f "$out" "$err"
      return 0
    fi
    if ! cat "$out" "$err" | grep -Eqi 'Failed to download metadata|repomd\.xml parser error|Errors during downloading metadata|Curl error|Cannot download|Status code: 5[0-9][0-9]|Operation timed out|Timeout was reached|timed out|Could not resolve host|Cannot prepare internal mirrorlist|Failed to synchronize cache|Connection (reset|refused)'; then
      rm -f "$out" "$err"
      return "$rc"
    fi
    if [ "$attempt" -lt 3 ]; then
      echo "::warning::dnf $1 failed on a repository or network error (attempt $attempt of 3), retrying" >&2
      dnf clean metadata >/dev/null 2>&1 || true
      sleep $((attempt * ${DNF_RETRY_SLEEP_UNIT:-10}))
    fi
  done
  echo "::error::dnf $1 failed after 3 attempts" >&2
  rm -f "$out" "$err"
  return "$rc"
}

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
resolute|ubuntu:26.04|1.28.3|2c96a946bfb0882a21744ed429770a2123ae1828c7c48665092993ddee91a918|deb
el9|almalinux:9|1.20.1|e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49|rpm
EOF
      else
        cat <<'EOF'
bookworm|debian:12|1.22.1|9ebb333a9e82b952acd3e2b4aeb1d4ff6406f72491bab6cd9fe69f0dea737f31|deb
trixie|debian:13|1.26.3|69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa|deb
jammy|ubuntu:22.04|1.18.0|4c373e7ab5bf91d34a4f11a0c9496561061ba5eee6020db272a17a7228d35f99|deb
noble|ubuntu:24.04|1.24.0|77a2541637b92a621e3ee76776c8b7b40cf6d707e69ba53a940283e30ff2f55d|deb
resolute|ubuntu:26.04|1.28.3|2c96a946bfb0882a21744ed429770a2123ae1828c7c48665092993ddee91a918|deb
el9|almalinux:9|1.20.1|e462e11533d5c30baa05df7652160ff5979591d291736cfa5edb9fd2edb48c49|rpm
EOF
      fi ;;
    el10)
      [ "$arch" = x64 ] || { echo "release_nginx: no el10 leg for $arch" >&2; return 1; }
      echo 'el10|almalinux:10|1.26.3|69ee2b237744036e61d24b836668aad3040dda461fe6f570f1787eab570c75aa|rpm' ;;
    nginxorg)
      # nginx.org's stable nginx rather than a distribution's, as used by the
      # nginx Docker image. Its own set: it rebuilds on nginx's schedule.
      # x64 only until the arm64 cells have their first-ship proof.
      [ "$arch" = x64 ] || { echo "release_nginx: no nginxorg legs for $arch" >&2; return 1; }
      # NGINXORG_VERSION and NGINXORG_SHA256 build another nginx.org version: a
      # rebuild, or the older build the upgrade test starts from.
      local ngx="${NGINXORG_VERSION:-1.30.6}"
      local ngxsha="${NGINXORG_SHA256:-3cf46f5ed7504d460125866b7bea99fb357f31d887e2e63b35503310e9b8b6c8}"
      if [ -n "${NGINXORG_VERSION:-}" ] && [ -z "${NGINXORG_SHA256:-}" ]; then
        echo "release_nginx: NGINXORG_VERSION needs NGINXORG_SHA256" >&2; return 1
      fi
      # The el9 cell keeps the exact pin; the dnf plugin the package ships
      # (install/nginx/pagespeed_nginx_pin.py) holds nginx back, so a plain
      # dnf update of a host passes while this package lags nginx.org
      # (test/package-test/check-nginx-org-upgrade.sh, plain_upgrade).
      # NGINXORG_EL9=0 leaves the cell out (a local or fallback build).
      cat <<LEGS
nginxorg-trixie|debian:13|${ngx}|${ngxsha}|deb
nginxorg-noble|ubuntu:24.04|${ngx}|${ngxsha}|deb
LEGS
      if [ "${NGINXORG_EL9:-1}" != 0 ]; then echo "nginxorg-el9|almalinux:9|${ngx}|${ngxsha}|rpm"; fi
      ;;
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

# legs --arch A --set S [--json]: the distro names of a leg table, one per
# line, or as a JSON array (the input of a workflow matrix).
legs() {
  local arch="" set="main" json=0 rows name names=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) arch="$2"; shift 2 ;;
      --set) set="$2"; shift 2 ;;
      --json) json=1; shift ;;
      *) echo "release_nginx legs: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$arch" ] || { echo "release_nginx legs: --arch is required" >&2; return 2; }
  rows="$(legs_for "$arch" "$set")" || return 1
  while IFS= read -r name; do names+=("${name%%|*}"); done <<<"$rows"
  if [ "$json" = 1 ]; then
    local out="" n
    for n in "${names[@]}"; do out="${out:+$out,}\"$n\""; done
    printf '[%s]\n' "$out"
  else
    printf '%s\n' "${names[@]}"
  fi
}

build() {
  local arch="" set="main" ws="" out="" distro_only=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) arch="$2"; shift 2 ;;
      --set) set="$2"; shift 2 ;;
      --workspace) ws="$2"; shift 2 ;;
      --out) out="$2"; shift 2 ;;
      --distro) distro_only="$2"; shift 2 ;;
      *) echo "release_nginx build: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$arch" ] && [ -d "$ws" ] && [ -n "$out" ] || { echo "release_nginx build: --arch, --workspace and --out are required" >&2; return 2; }
  ws="$(cd "$ws" && pwd)"
  mkdir -p "$out"; out="$(cd "$out" && pwd)"

  local legs=() row
  while IFS= read -r row; do
    if [ -z "$distro_only" ] || [ "${row%%|*}" = "$distro_only" ]; then legs+=("$row"); fi
  done < <(legs_for "$arch" "$set")
  if [ "${#legs[@]}" -eq 0 ]; then
    echo "release_nginx build: no leg '${distro_only}' for $arch/$set" >&2
    return 2
  fi

  local default_cores=8
  [ "$arch" = arm64 ] && default_cores=6
  local nproc_n budget p cores leg_mem conc
  nproc_n="$(nproc)"
  # Per-slot budget of the runner (CI_SLOT_CPUS / CI_SLOT_MEM_GB) bounds the
  # fan-out; RAM_BUDGET_GB still wins when set explicitly.
  # shellcheck source=slot-budget.sh
  source "$(dirname "${BASH_SOURCE[0]}")/slot-budget.sh"
  slot_budget || return 1
  if [ -n "$SLOT_CPUS" ] && [ "$SLOT_CPUS" -lt "$nproc_n" ]; then nproc_n="$SLOT_CPUS"; fi
  budget="${RAM_BUDGET_GB:-${SLOT_MEM_GB:-}}"
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
    # A nginxorg-<suite> leg builds in <suite>'s container and recipe, against
    # nginx.org's source, under its own package name.
    local build_distro="${distro#nginxorg-}" pkg_name=""
    if [ "$build_distro" != "$distro" ]; then pkg_name="nginx-module-pagespeed-nginxorg"; fi
    # nginx.org's el9 nginx carries epoch 2; the rpm pin must name it.
    local pkg_epoch=""
    if [ -n "$pkg_name" ] && [ "$pkg" = rpm ]; then pkg_epoch=2; fi
    # Its release names the nginx too, so a rebuild sorts as an upgrade.
    local rpm_dist=".${build_distro}" lane_rev="${NGINXORG_LANE_REVISION:-0}"
    case "$lane_rev" in [0-9]|[1-9][0-9]) ;; *) echo "::error::NGINXORG_LANE_REVISION must be 0 to 99"; return 92 ;; esac
    if [ -n "$pkg_name" ]; then
      rpm_dist=".ngx${ngxver}.${build_distro}"
      # A rebuild of the same pair (lane revision N > 0) sorts after the first.
      [ "$lane_rev" = 0 ] || rpm_dist=".ngx${ngxver}.${lane_rev}.${build_distro}"
    fi
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
        wrap="./install/nginx/build_nginx_rpm.sh -a aarch64 -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so -d ${rpm_dist}"
      else
        wrap="./install/nginx/build_nginx_rpm.sh -o /workspace/nginx-packages -s /workspace/ngx_pagespeed_module_${distro}.so -d ${rpm_dist}"
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
      -e NGINX_PKG_NAME="${pkg_name}" \
      ${pkg_name:+-e NGINX_UPSTREAM_VERSION="${ngxver}" -e NGX_SRC_MODE_OVERRIDE=vanilla -e NGINXORG_LANE_REVISION="${lane_rev}"} \
      ${pkg_epoch:+-e NGINX_EPOCH="${pkg_epoch}"} \
      -v "$wsdir":/workspace \
      -v "$cache_dir":/cache \
      -v "$out":/collect \
      -w /workspace \
      "${image}" \
      bash -c '
        set -e
        export BAZEL_DISK_CACHE=/cache
        mkdir -p /workspace/nginx-packages
        ./install/nginx/build_module_in_container.sh '"${build_distro}"' \
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

# leg_row: print the one leg row (name|image|nginx version|sha256|pkg) for a
# distro, so a lane that builds a single leg (the nightly shared-cache check)
# takes the same nginx version and source hash as the release.
leg_row() {
  local arch="" set="main" distro="" row
  while [ $# -gt 0 ]; do
    case "$1" in
      --arch) arch="$2"; shift 2 ;;
      --set) set="$2"; shift 2 ;;
      --distro) distro="$2"; shift 2 ;;
      *) echo "release_nginx leg-row: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -n "$arch" ] && [ -n "$distro" ] || { echo "release_nginx leg-row: --arch and --distro are required" >&2; return 2; }
  while IFS= read -r row; do
    if [ "${row%%|*}" = "$distro" ]; then
      echo "$row"
      return 0
    fi
  done < <(legs_for "$arch" "$set")
  echo "release_nginx leg-row: no '$distro' leg for $arch ($set)" >&2
  return 1
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
DEB_SUITES=("noble|ubuntu:24.04" "bookworm|debian:12" "trixie|debian:13" "jammy|ubuntu:22.04" "resolute|ubuntu:26.04")

# The symbol-leak check needs readelf and, for an rpm, cpio: run it in a
# container of the package's own family so the runner needs neither.
symbol_leak() {
  gate_args "$@" || return $?
  [ -n "$SRC" ] || { echo "release_nginx: --src is required" >&2; return 2; }
  shopt -s nullglob
  local debs=("$PKGDIR"/nginx-module-pagespeed_*_"${DEB_ARCH}".deb "$PKGDIR"/nginx-module-pagespeed-nginxorg_*_"${DEB_ARCH}".deb)
  local rpms=("$PKGDIR"/nginx-module-pagespeed-*."${RPM_ARCH}".rpm)
  if [ "${#debs[@]}" -eq 0 ] && [ "${#rpms[@]}" -eq 0 ]; then
    echo "::error::no nginx module packages to check for leaked symbols"
    return 1
  fi
  if [ "${#debs[@]}" -gt 0 ]; then
    docker run --rm -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro ubuntu:24.04 bash -c '
      set -e
      '"$(declare -f apt_install_retry)"'
      apt_install_retry binutils
      shopt -s nullglob
      n=0
      for p in /pkgs/nginx-module-pagespeed_*_'"${DEB_ARCH}"'.deb /pkgs/nginx-module-pagespeed-nginxorg_*_'"${DEB_ARCH}"'.deb; do
        echo "=== symbol-leak gate: $(basename "$p") ==="
        /src/test/package-test/check-nginx-symbol-leak.sh "$p"
        n=$((n + 1))
      done
      [ "$n" = '"${#debs[@]}"' ] || { echo "::error::symbol-leak gate checked $n of '"${#debs[@]}"' debs"; exit 1; }'
  fi
  if [ "${#rpms[@]}" -gt 0 ]; then
    docker run --rm -v "$SRC":/src:ro -v "$PKGDIR":/pkgs:ro almalinux:9 bash -c '
      set -e
      '"$(declare -f dnf_retry)"'
      dnf_retry install -y -q cpio binutils >/dev/null
      shopt -s nullglob
      n=0
      for p in /pkgs/nginx-module-pagespeed-*.'"${RPM_ARCH}"'.rpm; do
        echo "=== symbol-leak gate: $(basename "$p") ==="
        /src/test/package-test/check-nginx-symbol-leak.sh "$p"
        n=$((n + 1))
      done
      [ "$n" = '"${#rpms[@]}"' ] || { echo "::error::symbol-leak gate checked $n of '"${#rpms[@]}"' rpms"; exit 1; }'
  fi
}

# The packages in --pkgdir are exactly the leg table's: one package per leg
# (the deb of that suite, the rpm of that dist), nothing else.
check_set() {
  gate_args "$@" || return $?
  local row distro image v h kind pat n=0 bad=0 f total
  # The loop below reads the table through a process substitution, which
  # hides a refused arch; refuse it here.
  legs_for "$ARCH" "$SET" >/dev/null || return 1
  shopt -s nullglob
  while IFS='|' read -r distro image v h kind; do
    if [ "$SET" = nginxorg ]; then
      # The nginx.org package: its own name, the suite without the set prefix.
      if [ "$kind" = deb ]; then
        pat="nginx-module-pagespeed-nginxorg_*[~.]${distro#nginxorg-}_${DEB_ARCH}.deb"
      else
        pat="nginx-module-pagespeed-nginxorg-*.${distro#nginxorg-}.${RPM_ARCH}.rpm"
      fi
    elif [ "$kind" = deb ]; then
      pat="nginx-module-pagespeed_*[~.]${distro}_${DEB_ARCH}.deb"
    else
      pat="nginx-module-pagespeed-*.${distro}.${RPM_ARCH}.rpm"
    fi
    local m=("$PKGDIR"/$pat)
    if [ "${#m[@]}" -ne 1 ]; then
      echo "::error::leg '${distro}': expected exactly one package matching ${pat}, found ${#m[@]}"
      bad=1
    fi
    n=$((n + 1))
  done < <(legs_for "$ARCH" "$SET")
  total=0
  for f in "$PKGDIR"/* "$PKGDIR"/.[!.]*; do [ -e "$f" ] && total=$((total + 1)); done
  if [ "$total" -ne "$n" ]; then
    echo "::error::expected ${n} packages in the set, found ${total}"
    bad=1
  fi
  [ "$bad" -eq 0 ] && echo "OK: the ${ARCH} set is exactly the ${n} expected packages"
  return "$bad"
}

# Every rpm must carry the dist tag of its yum tree in its name and none may
# carry the cPanel prefix: the consumer routes by name.
routing() {
  gate_args "$@" || return $?
  local dist=el9
  [ "$SET" = el10 ] && dist=el10
  shopt -s nullglob
  local rpms=("$PKGDIR"/*.rpm) r base
  if [ "${#rpms[@]}" -eq 0 ]; then
    # The nginxorg set has no rpm only when its el9 leg is left out.
    if [ "$SET" = nginxorg ] && [ "${NGINXORG_EL9:-1}" = 0 ]; then echo "OK: no rpm in the nginxorg set"; return 0; fi
    echo "::error::no nginx rpm to check"; return 1
  fi
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
  if [ "$SET" = nginxorg ]; then
    # Not the distribution's nginx: nginx.org's, at the exact version.
    local ngxver leg_rows pkgfile
    leg_rows="$(legs_for "$ARCH" nginxorg)" || return 1
    while IFS='|' read -r distro image ngxver _ _; do
      case "$distro" in
        nginxorg-el9) pkgfile="$(first_of "$PKGDIR"/nginx-module-pagespeed-nginxorg-*.el9."${RPM_ARCH}".rpm)" ;;
        *) pkgfile="$(first_of "$PKGDIR"/nginx-module-pagespeed-nginxorg_*~"${distro#nginxorg-}"_"${DEB_ARCH}".deb)" ;;
      esac
      echo "=== load check: ${distro} (nginx ${ngxver}) - $(basename "$pkgfile") ==="
      if [ "$distro" = nginxorg-trixie ]; then
        "$SRC"/test/package-test/check-nginx-org-image.sh "nginx:${ngxver}" "$pkgfile" </dev/null || return $?
      fi
      "$SRC"/test/package-test/check-nginx-org-host.sh "$image" "$ngxver" "$pkgfile" </dev/null || return $?
    done <<<"$leg_rows"
    return 0
  fi
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
      '"$(declare -f apt_install_retry)"'
      apt_install_retry --no-install-recommends nginx curl ca-certificates python3 python3-pip
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
    '"$(declare -f dnf_retry)"'
    dnf_retry install -y --allowerasing --setopt=install_weak_deps=False nginx curl ca-certificates python3 python3-pip tar gzip findutils >/dev/null
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
  chk "x64 main legs" "$(legs_for x64 main | wc -l | tr -d ' ')" 6
  chk "arm64 main legs" "$(legs_for arm64 main | wc -l | tr -d ' ')" 6
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

  # the matrix listing and the single-leg build
  chk "arm64 legs json" "$(legs --arch arm64 --set main --json)" '["bookworm","trixie","jammy","noble","resolute","el9"]'
  chk "el10 legs json" "$(legs --arch x64 --set el10 --json)" '["el10"]'
  chk "x64 legs lines" "$(legs --arch x64 --set main | tr '\n' ' ')" "noble bookworm trixie jammy resolute el9 "
  chk "legs json equals the table" "$(legs --arch arm64 --set main --json | tr -d '[]"' | tr ',' '\n' | sort)" \
    "$(legs_for arm64 main | cut -d'|' -f1 | sort)"
  if legs --arch arm64 --set el10 --json >/dev/null 2>&1; then echo "FAIL: arm64 el10 listed"; fail=1; fi
  if legs --set main --json >/dev/null 2>&1; then echo "FAIL: legs without --arch accepted"; fail=1; fi
  mkdir -p "$t/ws" "$t/o"
  build --arch arm64 --set main --distro nosuch --workspace "$t/ws" --out "$t/o" >/dev/null 2>&1 \
    && { echo "FAIL: unknown --distro accepted"; fail=1; }
  build --arch arm64 --set main --distro el10 --workspace "$t/ws" --out "$t/o" >/dev/null 2>&1 \
    && { echo "FAIL: a distro of another table accepted"; fail=1; }

  # the exact-set gate
  mkdir -p "$t/set"
  for d in bookworm trixie jammy noble resolute; do : > "$t/set/nginx-module-pagespeed_1.0-r1~${d}_arm64.deb"; done
  : > "$t/set/nginx-module-pagespeed-1.0-1.el9.aarch64.rpm"
  check_set --arch arm64 --pkgdir "$t/set" >/dev/null 2>&1 || { echo "FAIL: exact set rejected"; fail=1; }
  : > "$t/set/extra.txt"
  check_set --arch arm64 --pkgdir "$t/set" >/dev/null 2>&1 && { echo "FAIL: extra file accepted"; fail=1; }
  rm "$t/set/extra.txt"
  : > "$t/set/.hidden"
  check_set --arch arm64 --pkgdir "$t/set" >/dev/null 2>&1 && { echo "FAIL: dotfile accepted"; fail=1; }
  rm "$t/set/.hidden"
  : > "$t/set/nginx-module-pagespeed_1.0-r2~jammy_arm64.deb"
  check_set --arch arm64 --pkgdir "$t/set" >/dev/null 2>&1 && { echo "FAIL: duplicate leg accepted"; fail=1; }
  rm "$t/set/nginx-module-pagespeed_1.0-r2~jammy_arm64.deb" "$t/set/nginx-module-pagespeed_1.0-r1~jammy_arm64.deb"
  check_set --arch arm64 --pkgdir "$t/set" >/dev/null 2>&1 && { echo "FAIL: missing leg accepted"; fail=1; }

  # the budget arithmetic
  chk "plan big host" "$(plan 8 1.5 85 128 6)" "8 14 6"
  chk "plan small host" "$(plan 8 1.5 14 4 5)" "4 8 1"
  chk "plan tiny host" "$(plan 8 1.5 2 1 5)" "1 3 1"
  chk "plan one leg" "$(plan 8 1.5 85 128 1)" "8 14 1"

  # the single-leg row
  chk "leg-row noble" "$(leg_row --arch x64 --distro noble)" "$(legs_for x64 main | grep '^noble|')"
  chk "leg-row el10" "$(leg_row --arch x64 --set el10 --distro el10)" "$(legs_for x64 el10)"
  leg_row --arch x64 --distro nosuch >/dev/null 2>&1 && { echo "FAIL: unknown leg accepted"; fail=1; }

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
  # the nginxorg set: its own package name, the el9 rpm by default
  chk "nginxorg legs" "$(legs --arch x64 --set nginxorg | tr '\n' ' ')" "nginxorg-trixie nginxorg-noble nginxorg-el9 "
  chk "nginxorg legs without el9" "$(NGINXORG_EL9=0 legs --arch x64 --set nginxorg | tr '\n' ' ')" "nginxorg-trixie nginxorg-noble "
  mkdir -p "$t/ngx"
  : > "$t/ngx/nginx-module-pagespeed-nginxorg_2.8.0-r0+ngx1.30.6~trixie_amd64.deb"
  : > "$t/ngx/nginx-module-pagespeed-nginxorg_2.8.0-r0+ngx1.30.6~noble_amd64.deb"
  check_set --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 && { echo "FAIL: nginxorg check-set accepted a set without its el9 rpm"; fail=1; }
  NGINXORG_EL9=0 check_set --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 || { echo "FAIL: nginxorg check-set without el9"; fail=1; }
  routing --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 && { echo "FAIL: nginxorg routing accepted a missing el9 rpm"; fail=1; }
  NGINXORG_EL9=0 routing --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 || { echo "FAIL: nginxorg routing without el9"; fail=1; }
  : > "$t/ngx/nginx-module-pagespeed-nginxorg-2.8.0-0.ngx1.30.6.el9.x86_64.rpm"
  check_set --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 || { echo "FAIL: nginxorg check-set with el9"; fail=1; }
  routing --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 || { echo "FAIL: nginxorg routing with el9"; fail=1; }
  NGINXORG_EL9=0 check_set --arch x64 --pkgdir "$t/ngx" --set nginxorg >/dev/null 2>&1 && { echo "FAIL: nginxorg check-set without el9 accepted an rpm"; fail=1; }
  legs --arch arm64 --set nginxorg >/dev/null 2>&1 && { echo "FAIL: nginxorg legs for arm64 accepted"; fail=1; }
  # An empty directory, so only the refused arch can fail the check.
  mkdir -p "$t/empty"
  check_set --arch arm64 --pkgdir "$t/empty" --set nginxorg >/dev/null 2>&1 && { echo "FAIL: nginxorg check-set for arm64 accepted"; fail=1; }
  out=""
  [ "$fail" = 0 ] && echo "release_nginx self-test: ok"
  return "$fail"
}

cmd="${1:-}"
case "$cmd" in
  --self-test) self_test ;;
  legs) shift; legs "$@" ;;
  build) shift; build "$@" ;;
  leg-row) shift; leg_row "$@" ;;
  check-set) shift; check_set "$@" ;;
  symbol-leak) shift; symbol_leak "$@" ;;
  routing) shift; routing "$@" ;;
  load-check) shift; load_check "$@" ;;
  suite) shift; suite "$@" ;;
  *) echo "usage: $0 legs|build|leg-row|check-set|symbol-leak|routing|load-check|suite ... | --self-test" >&2; exit 2 ;;
esac
