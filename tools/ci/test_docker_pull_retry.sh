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

run_mode() { # mode -> sets rc, tries, err
  export FAKE_LOG="${WORK}/log" FAKE_MODE="$1" DOCKER_PULL_RETRY_SLEEP_UNIT=0
  : > "${FAKE_LOG}"
  PATH="${WORK}/bin:${PATH}" bash -c 'source "$1"; docker_pull_retry "mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:abc" --platform linux/amd64' _ \
    "${HERE}/docker_pull_retry.sh" 2>"${WORK}/err" >/dev/null
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

for msg in 'toomanyrequests: You have reached your pull rate limit' 'read: connection reset by peer' 'dial tcp: i/o timeout' \
  'received unexpected HTTP status: 503 Service Unavailable' 'net/http: request canceled while waiting for connection'; do
  run_mode "$msg"
  check "transient: ${msg%%:*}" 3 "$tries"
done

# Structural: every docker run of a registry image is preceded (within 12
# lines, same function or script) by a docker_pull_retry of the same image
# expression. A run of a locally built image carries "# local-image" on the
# line above it.
FILES=(tools/ci/release_sidecar.sh tools/ci/release_nginx.sh tools/ci/release_ea4_build.sh
  test/package-test/check-sidecar-pair.sh test/package-test/check-nginx-org-host.sh
  test/package-test/check-nginx-org-upgrade.sh test/package-test/check-nginx-org-image.sh
  test/package-test/check-debian-maintscripts.sh test/package-test/check-nginx-symbol-leak.sh
  test/browser/hydration/run_module_check.sh)
cat > "${WORK}/structural.py" <<'PY'
import re, sys
bad = []
for f in sys.argv[1:]:
    L = open(f).read().split('\n')
    pulls = 0
    for i, line in enumerate(L):
        if re.match(r'\s*#', line):
            continue
        if re.search(r'\bdocker run\b', line):
            if i and 'local-image' in L[i - 1]:
                continue
            # the statement: this line and its continuations up to the image
            stmt = line
            j = i
            while stmt.rstrip().endswith('\\') and j + 1 < len(L):
                j += 1
                stmt += '\n' + L[j]
            prev = [m for k in range(max(0, i - 12), i)
                    for m in [re.match(r'\s*docker_pull_retry (\S+)', L[k])] if m]
            if not prev:
                bad.append(f'{f}:{i+1}: docker run without a docker_pull_retry before it')
                continue
            if not any(m.group(1) in stmt for m in prev):
                bad.append(f'{f}:{i+1}: docker run does not use the image of the docker_pull_retry before it')
    if 'docker run' in '\n'.join(L) and 'docker_pull_retry.sh' not in '\n'.join(L):
        bad.append(f'{f}: does not source docker_pull_retry.sh')
print('\n'.join(bad))
PY
out="$(cd "${ROOT}" && python3 -I "${WORK}/structural.py" "${FILES[@]}")"
if [ -z "$out" ]; then echo "ok   structural: every registry docker run has its pull"; else echo "$out"; echo "FAIL structural"; FAILS=$((FAILS + 1)); fi

# The negative control: the same check must flag a run without a pull.
printf 'x\ndocker run --rm img true\n' > "${WORK}/bad.sh"
neg="$(cd "${WORK}" && python3 -I "${WORK}/structural.py" bad.sh)"
case "$neg" in *"without a docker_pull_retry"*) r=caught ;; *) r=missed ;; esac
check "negative control" caught "$r"

if [ "$FAILS" -ne 0 ]; then echo "${FAILS} failure(s)"; exit 1; fi
echo "all ok"
