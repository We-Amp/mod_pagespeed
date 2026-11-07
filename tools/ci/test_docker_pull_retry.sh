#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for docker_pull_retry (tools/ci/docker_pull_retry.sh), with a fake
# docker on PATH: a transient registry error is retried and then passes, a
# lasting one fails after three tries with an ::error:: line, and a
# non-transient one (manifest unknown, not found, unauthorized, denied) is not
# retried. A structural check then requires that every `docker run` of a
# registry image in the release lane and its test scripts is preceded by a
# docker_pull_retry of the same image.
#
# Run: tools/ci/test_docker_pull_retry.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
FAILS=0

mkdir "${WORK}/bin"
cat > "${WORK}/bin/docker" <<'FAKE'
#!/usr/bin/env bash
# Fake docker: FAKE_MODE = ok | transient-once | transient-always | <message>
if [ "$1 $2" = "image inspect" ]; then
  # FAKE_CACHED = "" (not local) or the OS/arch of the local image
  [ -n "${FAKE_CACHED:-}" ] || { echo "Error: No such image" >&2; exit 1; }
  echo "${FAKE_CACHED}"; exit 0
fi
echo "docker $*" >> "${FAKE_LOG}"
n=$(wc -l < "${FAKE_LOG}")
case "${FAKE_MODE}" in
  ok) exit 0 ;;
  transient-once)
    if [ "$n" -lt 2 ]; then
      echo 'Error response from daemon: Get "https://mcr.microsoft.com/v2/": context deadline exceeded' >&2
      exit 1
    fi
    exit 0 ;;
  transient-always)
    echo 'Error response from daemon: Get "https://registry-1.docker.io/v2/": net/http: TLS handshake timeout' >&2
    exit 1 ;;
  *) echo "Error response from daemon: ${FAKE_MODE}" >&2; exit 1 ;;
esac
FAKE
chmod +x "${WORK}/bin/docker"

check() {
  local name="$1" want="$2" got="$3"
  if [ "$want" = "$got" ]; then echo "ok   $name"; else echo "FAIL $name: want '$want' got '$got'"; FAILS=$((FAILS + 1)); fi
}

run_mode() { # mode [cached] [--quiet] -> sets rc, tries, err
  export FAKE_LOG="${WORK}/log" FAKE_MODE="$1" FAKE_CACHED="${2:-}" DOCKER_PULL_RETRY_SLEEP_UNIT=0
  : > "${FAKE_LOG}"
  PATH="${WORK}/bin:${PATH}" bash -c 'source "$1"; docker_pull_retry ${3:+"$3"} "mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:abc" --platform linux/amd64' _ \
    "${HERE}/docker_pull_retry.sh" "" "${3:-}" 2>"${WORK}/err" >/dev/null
  rc=$?
  tries=$(wc -l < "${FAKE_LOG}" | tr -d ' ')
  err="$(cat "${WORK}/err")"
}

run_mode ok
check "ok: passes" 0 "$rc"; check "ok: 1 try" 1 "$tries"
grep -q 'docker pull -q --platform linux/amd64 mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:abc' "${FAKE_LOG}"
check "ok: digest and platform passed through" 0 $?

run_mode transient-once
check "transient-once: passes" 0 "$rc"; check "transient-once: 2 tries" 2 "$tries"

run_mode transient-always
check "transient-always: fails" 1 "$rc"; check "transient-always: 3 tries" 3 "$tries"
case "$err" in *"::error::docker pull of mcr.microsoft.com/dotnet/sdk:10.0-noble failed after 3 attempts"*) r=0 ;; *) r=1 ;; esac
check "transient-always: loud ::error::" 0 "$r"

for msg in 'manifest unknown: manifest unknown' 'pull access denied for foo, repository does not exist or may require authorization' \
  'unauthorized: authentication required' 'Error: not found' 'denied: requested access to the resource is denied'; do
  run_mode "$msg"
  check "no retry: ${msg%%:*}" 1 "$tries"; check "no retry rc: ${msg%%:*}" 1 "$rc"
  case "$err" in *"::error::"*) r=0 ;; *) r=1 ;; esac
  check "no retry error line: ${msg%%:*}" 0 "$r"
done

# A cached image is used without the registry; a cached image of the wrong
# platform is pulled.
run_mode transient-always linux/amd64
check "cached, same platform: passes" 0 "$rc"; check "cached, same platform: no pull" 0 "$tries"
run_mode transient-always linux/arm64
check "cached, other platform: pulls" 3 "$tries"; check "cached, other platform: fails" 1 "$rc"
run_mode ok linux/arm64
check "cached, other platform, registry ok: passes" 0 "$rc"; check "cached, other platform: 1 pull" 1 "$tries"

# --quiet: no annotations.
run_mode transient-always "" --quiet
check "quiet: fails" 1 "$rc"
case "$err" in *"::"*) r=annotation ;; *) r=plain ;; esac
check "quiet: no ::error:: or ::warning::" plain "$r"

for msg in 'toomanyrequests: You have reached your pull rate limit' 'read: connection reset by peer' 'dial tcp: i/o timeout' \
  'received unexpected HTTP status: 503 Service Unavailable' 'unexpected EOF' 'net/http: request canceled while waiting for connection'; do
  run_mode "$msg"
  check "transient: ${msg%%:*}" 3 "$tries"
done

# Structural: every docker run of a registry image in every script of
# tools/ci and test/package-test (and the hydration check) has its pull
# (tools/ci/check_docker_pull_before_run.py).
CHECK="${HERE}/check_docker_pull_before_run.py"
SCRIPTS=()
for f in tools/ci/*.sh test/package-test/*.sh test/browser/hydration/*.sh; do
  [ "$f" = tools/ci/test_docker_pull_retry.sh ] || SCRIPTS+=("$f") # it holds the fixtures
done
out="$(cd "${ROOT}" && python3 -I "${CHECK}" "${SCRIPTS[@]}")"
if [ -z "$out" ]; then echo "ok   structural: every registry docker run has its pull"; else echo "$out"; echo "FAIL structural"; FAILS=$((FAILS + 1)); fi

# Mutations: each fixture must be flagged (want a message containing $2), the
# good one must pass.
fixture() { # name, expected message part ("" = must pass), body
  local name="$1" want="$2" body="$3" res
  printf '#!/bin/bash\nset -e\nsource "$(dirname "${BASH_SOURCE[0]}")/docker_pull_retry.sh"\n%s\n' "$body" > "${WORK}/fx.sh"
  res="$(cd "${WORK}" && python3 -I "${CHECK}" fx.sh)"
  if [ -z "$want" ]; then
    if [ -z "$res" ]; then echo "ok   fixture $name: passes"; else echo "FAIL fixture $name: $res"; FAILS=$((FAILS + 1)); fi
  else
    case "$res" in *"$want"*) echo "ok   fixture $name: flagged" ;; *) echo "FAIL fixture $name: want '$want' got '$res'"; FAILS=$((FAILS + 1)) ;; esac
  fi
}
fixture good "" 'docker_pull_retry almalinux:9 --platform linux/amd64 || exit 1
docker run --rm --platform linux/amd64 -v /a:/b almalinux:9 true'
fixture no-pull "without a docker_pull_retry" 'docker run --rm almalinux:9 true'
fixture other-image "without a docker_pull_retry" 'docker_pull_retry almalinux:8 || exit 1
docker run --rm almalinux:9 true'
fixture image-prefix "without a docker_pull_retry" 'docker_pull_retry almalinux:9 || exit 1
docker run --rm almalinux:9-minimal true'
fixture container-run "without a docker_pull_retry" 'docker container run --rm almalinux:9 true'
fixture tab-spacing "without a docker_pull_retry" 'docker	 run --rm almalinux:9 true'
fixture pull-or-true "ends in || true" 'docker_pull_retry almalinux:9 || true
docker run --rm almalinux:9 true'
fixture dropped-platform "--platform of the docker run" 'docker_pull_retry almalinux:9 || exit 1
docker run --rm --platform linux/arm64 almalinux:9 true'
fixture added-platform "--platform of the docker run" 'docker_pull_retry almalinux:9 --platform linux/arm64 || exit 1
docker run --rm almalinux:9 true'
fixture marker-abuse "not built by a docker build" '# local-image
docker run --rm almalinux:9 true'
fixture marker-midline "without a docker_pull_retry" 'echo hi # local-image
docker run --rm almalinux:9 true'
fixture marker-wrong-tag "not built by a docker build" 'docker build -t mine:1 .
# local-image
docker run --rm almalinux:9 true'
fixture marker-ok "" 'docker build -t mine:1 .
# local-image
docker run --rm mine:1 true'
filler="$(printf 'echo x\n%.0s' $(seq 1 13))"
fixture too-far "without a docker_pull_retry" "  docker_pull_retry almalinux:9 || exit 1
${filler}
  docker run --rm almalinux:9 true"
printf '#!/bin/bash\ndocker_pull_retry almalinux:9 || exit 1\ndocker run --rm almalinux:9 true\n' > "${WORK}/ns.sh"
res="$(cd "${WORK}" && python3 -I "${CHECK}" ns.sh)"
case "$res" in *"does not source"*) echo "ok   fixture not-sourced: flagged" ;; *) echo "FAIL fixture not-sourced: '$res'"; FAILS=$((FAILS + 1)) ;; esac

if [ "$FAILS" -ne 0 ]; then echo "${FAILS} failure(s)"; exit 1; fi
echo "all ok"
