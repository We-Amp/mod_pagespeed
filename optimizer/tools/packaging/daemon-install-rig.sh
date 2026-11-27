#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Host-side driver for the package-install verification rig (design §9):
# builds the optimizer package for one target distro, installs it
# in throwaway containers, and runs the assertion legs.
#
#   daemon-install-rig.sh --distro debian12|ubuntu2404|alma9 \
#       --binary PATH --library PATH [--version V] [--work DIR]
#
# Phases, one container each (never one container reused -- "the install
# created the user, the group and the cache directory" is only an assertion
# on a machine where they did not exist yet):
#
#   fresh    plain container: install onto a clean system; identity, layout,
#            process identity (setpriv, secrets off argv), peer access
#            (kernel permissions AND ps_cache_open through the shipped
#            client library)
#   legacy   plain container: install over a planted root-owned pre-2.1
#            cache; cold-start notice, legacy tree untouched; then a
#            same-package reinstall asserting both env files come out of an
#            upgrade at 0640 root:pagespeed with operator edits kept (#1486)
#   systemd  booted-systemd container: the unit as systemd runs it, and the
#            restart-backoff latch.  Needs a container that can host PID 1
#            systemd (privileged + host cgroup namespace).  A runner that
#            cannot provide one FAILS this lane -- it does not skip it.
#            These two legs carry the half of the security claim that has no
#            other observation point, and a lane that goes quiet when it
#            loses the ability to check is worse than one that goes red: the
#            claim would keep reporting green while nothing verified it.  The
#            blocker is printed either way, so a genuine infrastructure
#            regression is diagnosable from the failure itself.
#            This phase also carries the opt-in long-idle browser soak (leg
#            h6-browser-soak, #1485): set H6_SOAK_SECONDS in the environment
#            to give it a window (CI Periodic's daemon-rig-soak-x64 job runs
#            it at 570 s); unset it stays a SKIP row, because a window long
#            enough to catch a minutes-scale periodic syscall is not a
#            per-PR cadence.
#   selinux  EL9 only, and one of the legs that may report SKIP (the others:
#            the EL9 browser legs, which have no Chromium, and the soak leg,
#            which skips unless H6_SOAK_SECONDS gives it a window): a container shares
#            the host kernel, so enforcing SELinux is a property of the
#            KERNEL the runner boots, not of the image or of how the
#            container is configured -- no change to this lane can make it
#            runnable here.  The shipped .te/.fc stay DRAFT and unpackaged
#            until a host that can enforce runs it.
#
# The package is built ONCE, inside the target distro (rpmbuild is not on the
# runner), and the same bytes are installed in every phase.
set -euo pipefail

DISTRO=""
BINARY=""
LIBRARY=""
VERSION="0.0.0~rig"
WORK=""
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

usage() { sed -n '4,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --distro) DISTRO="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --library) LIBRARY="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    *) usage ;;
  esac
done
case "$DISTRO" in debian12|ubuntu2404|alma9) ;; *) usage ;; esac
[[ -f "$BINARY" ]] || { echo "error: binary not found: $BINARY" >&2; exit 2; }
[[ -f "$LIBRARY" ]] || { echo "error: library not found: $LIBRARY" >&2; exit 2; }

BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"
LIBRARY="$(cd "$(dirname "$LIBRARY")" && pwd)/$(basename "$LIBRARY")"
ARTDIR="$(dirname "$BINARY")"
[[ "$(dirname "$LIBRARY")" == "$ARTDIR" ]] || {
  echo "error: --binary and --library must live in the same directory" >&2; exit 2; }

WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
RUN_ID="${GITHUB_RUN_ID:-local}-$$"
SYSTEMD_CTR="rig-systemd-${DISTRO}-${RUN_ID}"
cleanup() {
  docker rm -f "$SYSTEMD_CTR" >/dev/null 2>&1 || true
  # Everything in the work dir was written by root inside a container; hand
  # it back so the caller (and the next run) can clean it up without root.
  [[ -n "${IMAGE:-}" ]] && docker run --rm -v "$WORK:/work" "$IMAGE" \
    chown -R "$(id -u):$(id -g)" /work >/dev/null 2>&1 || true
}
trap cleanup EXIT

DOCKERFILE="$HERE/rig/Dockerfile.$DISTRO"
[[ -f "$DOCKERFILE" ]] || { echo "error: no rig Dockerfile for $DISTRO" >&2; exit 2; }
# The aarch64 lane's runner is macOS-hosted -- it does all of its Linux work
# inside containers, but this driver itself runs on the host, where there is
# no sha256sum (BSD ships `shasum`). Same formula either way, so the image tag
# is identical on both lanes and the cache is shared.
hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"
  else shasum -a 256 "$1"; fi
}
# The build context is a copy of rig/ plus docker/install-chromium.sh (the
# Ubuntu image provisions its Chromium through the product's own script), and
# the tag hashes both so a change to either rebuilds.
CHROMIUM_SH="$REPO/docker/install-chromium.sh"
IMAGE_HASH="$(cat "$DOCKERFILE" "$CHROMIUM_SH" | hash_file /dev/stdin | cut -c1-12)"
IMAGE="pagespeed-rig:${DISTRO}-${IMAGE_HASH}"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building rig image $IMAGE"
  CTX="$(mktemp -d)"
  cp "$HERE"/rig/Dockerfile.* "$CHROMIUM_SH" "$CTX/"
  docker build -q -t "$IMAGE" -f "$CTX/$(basename "$DOCKERFILE")" "$CTX" >/dev/null
  rm -rf "$CTX"
fi
echo "rig image: $IMAGE"

RESULTS="$WORK/results.txt"
: > "$RESULTS"
record() { printf '%s|%s|%s\n' "$1" "$2" "$3" >> "$RESULTS"; }
harvest() { # phase-log fallback-leg-name
  if grep -q '^RIG-RESULT: ' "$1"; then
    sed -n 's/^RIG-RESULT: //p' "$1" >> "$RESULTS"
  else
    record "$2" FAIL "phase produced no leg result -- see the log above"
  fi
}

plain_run() { # scenario extra-args...
  local scenario="$1"; shift
  # SYS_PTRACE is for the RIG, never for the daemon: the uid transition
  # clears the daemon's dumpable flag, which puts /proc/<pid>/environ behind
  # PTRACE_MODE_READ even for root, and reading it is how the rig proves the
  # secrets really were delivered through the env file (the counterpart to
  # asserting they are absent from the command line).
  docker run --rm --init --cap-add=SYS_PTRACE \
    -v "$REPO:/repo:ro" -v "$WORK:/work" -v "$ARTDIR:/artifacts:ro" \
    -w /repo "$IMAGE" \
    bash /repo/tools/packaging/verify-daemon-install.sh \
      --scenario "$scenario" --work /work \
      --binary /artifacts/"$(basename "$BINARY")" \
      --library /artifacts/"$(basename "$LIBRARY")" \
      --version "$VERSION" "$@"
}

# ---------------------------------------------------------------- fresh ----
echo "::group::${DISTRO}: fresh install"
rc=0
plain_run fresh --package-out /work/pkg 2>&1 | tee "$WORK/fresh.log" || rc=$?
echo "::endgroup::"
harvest "$WORK/fresh.log" install-fresh
[[ "$rc" -eq 0 ]] || echo "::error::${DISTRO}: fresh-install phase failed"

PACKAGE="$(find "$WORK/pkg" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) | head -1)"
if [[ -z "$PACKAGE" ]]; then
  record install-over-legacy FAIL "no package was produced by the fresh phase"
  record systemd-identity FAIL "no package was produced by the fresh phase"
  record restart-backoff FAIL "no package was produced by the fresh phase"
  record h6-browser-analysis FAIL "no package was produced by the fresh phase"
  record h6-browser-soak FAIL "no package was produced by the fresh phase"
  rc=1
else
  echo "package under test: $(basename "$PACKAGE")"
  # ---------------------------------------------------------------- legacy --
  echo "::group::${DISTRO}: install over a planted root-owned legacy cache"
  lrc=0
  plain_run legacy --package "/work/pkg/$(basename "$PACKAGE")" 2>&1 \
    | tee "$WORK/legacy.log" || lrc=$?
  echo "::endgroup::"
  harvest "$WORK/legacy.log" install-over-legacy
  [[ "$lrc" -eq 0 ]] || { rc=1; echo "::error::${DISTRO}: legacy-install phase failed"; }

  # --------------------------------------------------------------- systemd --
  echo "::group::${DISTRO}: booted-systemd container"
  blocker=""
  if ! docker run -d --name "$SYSTEMD_CTR" --privileged --cgroupns=host \
        -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock \
        --tmpfs /tmp -e container=docker \
        -v "$REPO:/repo:ro" -v "$WORK:/work" "$IMAGE" /sbin/init \
        >"$WORK/systemd-start.log" 2>&1; then
    blocker="container refused to start: $(tail -3 "$WORK/systemd-start.log" | tr '\n' ' ')"
  else
    ready=""
    for _ in $(seq 1 60); do
      state="$(docker exec "$SYSTEMD_CTR" systemctl is-system-running 2>&1 || true)"
      case "$state" in running|degraded) ready=1; break ;; esac
      sleep 2
    done
    [[ -n "$ready" ]] || blocker="systemd never reached running/degraded (last: ${state}); docker logs: $(docker logs "$SYSTEMD_CTR" 2>&1 | tail -3 | tr '\n' ' ')"
  fi
  if [[ -n "$blocker" ]]; then
    # Deliberately fatal, not a skip: see the phase table at the top.  An
    # unrunnable leg is an unverified claim, and an unverified claim must not
    # report green.
    echo "::error::${DISTRO}: booted-systemd phase could not run -- ${blocker}"
    record systemd-identity FAIL "could not run: ${blocker}"
    record restart-backoff FAIL "could not run: ${blocker}"
    record h6-browser-analysis FAIL "could not run: ${blocker}"
    record h6-browser-soak FAIL "could not run: ${blocker}"
    rc=1
  else
    src=0
    # H6_SOAK_SECONDS is opt-in (default 0: the soak leg reports SKIP -- see
    # verify-daemon-systemd.sh).  Forward it only when set, so the per-PR
    # matrix and the periodic soak job share this one driver.
    SOAK_ENV=()
    [[ -n "${H6_SOAK_SECONDS:-}" ]] && SOAK_ENV=(-e "H6_SOAK_SECONDS=${H6_SOAK_SECONDS}")
    docker exec ${SOAK_ENV[@]+"${SOAK_ENV[@]}"} "$SYSTEMD_CTR" \
      bash /repo/tools/packaging/verify-daemon-systemd.sh \
      --package "/work/pkg/$(basename "$PACKAGE")" --work /work 2>&1 \
      | tee "$WORK/systemd.log" || src=$?
    harvest "$WORK/systemd.log" systemd-identity
    [[ "$src" -eq 0 ]] || { rc=1; echo "::error::${DISTRO}: booted-systemd phase failed"; }
  fi
  echo "::endgroup::"
fi

# --------------------------------------------------------------- selinux ----
if [[ "$DISTRO" == alma9 ]]; then
  if [[ ! -e /sys/fs/selinux/enforce ]]; then
    sel="BLOCKER: the runner's kernel has no SELinux (/sys/fs/selinux/enforce absent). A container shares the host kernel, so no image can make this leg enforcing -- it needs a runner whose kernel is booted with SELinux enforcing (an EL9 VM or bare metal). The shipped .te/.fc therefore stay DRAFT and unpackaged."
  elif [[ "$(cat /sys/fs/selinux/enforce 2>/dev/null)" != 1 ]]; then
    sel="BLOCKER: the runner's kernel has SELinux but is not enforcing (/sys/fs/selinux/enforce=$(cat /sys/fs/selinux/enforce 2>/dev/null)); enforcement is a host property a container cannot supply. The shipped .te/.fc stay DRAFT and unpackaged."
  else
    sel="the runner IS enforcing -- this leg is implementable here and is the next step: load the policy module, run the request/optimize cycle, assert no AVCs. Until it passes, the .te/.fc stay DRAFT and unpackaged."
  fi
  echo "::warning::alma9: enforcing-SELinux leg SKIPPED -- ${sel}"
  record selinux-enforcing SKIP "$sel"
fi

# --------------------------------------------------------------- summary ----
{
  echo "### Daemon privilege rig — ${DISTRO}"
  echo
  echo "| Leg | Result | Note |"
  echo "| --- | --- | --- |"
  while IFS='|' read -r name status note; do
    [[ -n "$name" ]] || continue
    echo "| ${name} | ${status} | ${note} |"
  done < "$RESULTS"
} | tee "$WORK/summary.md"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  cat "$WORK/summary.md" >> "$GITHUB_STEP_SUMMARY"
fi

if grep -q '|FAIL|' "$RESULTS" || [[ "$rc" -ne 0 ]]; then
  echo "daemon-install-rig: ${DISTRO} FAILED" >&2
  exit 1
fi
echo "daemon-install-rig: ${DISTRO} passed"
