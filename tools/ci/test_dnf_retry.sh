#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for the dnf_retry helper copies (release_nginx.sh,
# build_module_in_container.sh, release_ea4_build.sh and two package-test
# scripts; the copies must behave the same): a transient repository error is retried and then passes, a lasting
# one fails after three tries, and a resolution error is not retried.
#
# Run: tools/ci/test_dnf_retry.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
FAILS=0

mkdir "${WORK}/bin"
cat > "${WORK}/bin/dnf" <<'FAKE'
#!/usr/bin/env bash
# Fake dnf: FAKE_MODE = transient-once | transient-always | resolution | ok
echo "dnf $*" >> "${FAKE_LOG}"
[ "$1" = clean ] && exit 0
n=$(grep -c -v '^dnf clean' "${FAKE_LOG}")
case "${FAKE_MODE}" in
  transient-once)
    if [ "$n" -lt 2 ]; then
      echo "Error: Failed to download metadata for repo 'appstream': repomd.xml parser error: Document is empty" >&2
      exit 1
    fi ;;
  transient-always)
    echo "Curl error (28): Timeout was reached for https://mirror/repomd.xml" >&2
    exit 1 ;;
  resolution)
    echo "Error: Unable to find a match: no-such-package" >&2
    exit 1 ;;
esac
echo "installed"
FAKE
chmod +x "${WORK}/bin/dnf"

check() {
  local name="$1" want="$2" got="$3"
  if [ "$want" = "$got" ]; then echo "ok   $name"; else echo "FAIL $name: want '$want' got '$got'"; FAILS=$((FAILS + 1)); fi
}

for f in tools/ci/release_nginx.sh install/nginx/build_module_in_container.sh tools/ci/release_ea4_build.sh \
  test/package-test/check-nginx-packaged-load.sh test/package-test/check-nginx-org-host.sh; do
  awk '/^dnf_retry\(\) \{/,/^\}/' "${ROOT}/${f}" > "${WORK}/fn.sh"
  [ -s "${WORK}/fn.sh" ] || { echo "FAIL ${f}: dnf_retry not found"; FAILS=$((FAILS + 1)); continue; }
  for mode in transient-once transient-always resolution ok; do
    export FAKE_LOG="${WORK}/log" FAKE_MODE="${mode}" DNF_RETRY_SLEEP_UNIT=0
    : > "${FAKE_LOG}"
    out="$(PATH="${WORK}/bin:${PATH}" bash -c 'source "$1"; dnf_retry install -y pkg' _ "${WORK}/fn.sh" 2>"${WORK}/err")"
    rc=$?
    tries=$(grep -c -v '^dnf clean' "${FAKE_LOG}")
    case "${mode}" in
      transient-once)   check "${f} ${mode}: passes"          0 "${rc}"; check "${f} ${mode}: 2 tries" 2 "${tries}"
                        check "${f} ${mode}: stdout passed"   installed "${out}"
                        check "${f} ${mode}: cleaned metadata" 1 "$(grep -c '^dnf clean metadata' "${FAKE_LOG}")" ;;
      transient-always) check "${f} ${mode}: fails"           1 "${rc}"; check "${f} ${mode}: 3 tries" 3 "${tries}"
                        check "${f} ${mode}: last error shown" 1 "$(grep -c 'Curl error' "${WORK}/err" | sed 's/^[1-9][0-9]*$/1/')"
                        check "${f} ${mode}: error line"       1 "$(grep -c '::error::dnf install failed after 3' "${WORK}/err")" ;;
      resolution)       check "${f} ${mode}: fails"           1 "${rc}"; check "${f} ${mode}: 1 try" 1 "${tries}"
                        check "${f} ${mode}: error shown"      1 "$(grep -c 'Unable to find a match' "${WORK}/err")" ;;
      ok)               check "${f} ${mode}: passes"          0 "${rc}"; check "${f} ${mode}: 1 try" 1 "${tries}"
                        check "${f} ${mode}: repo option"     1 "$(grep -c '^dnf install -y pkg --setopt=skip_if_unavailable=False$' "${FAKE_LOG}")" ;;
    esac
  done
done


# Structural check: a dnf_retry call inside a single-quoted container body
# (opened by a line ending in `bash -c '`, `bash -euc '` ...) must follow a
# `declare -f dnf_retry` splice in that body; the function defined on the host
# does not exist in the container's shell.
for f in tools/ci/release_nginx.sh install/nginx/build_module_in_container.sh tools/ci/release_ea4_build.sh \
  test/package-test/check-nginx-packaged-load.sh test/package-test/check-nginx-org-host.sh; do
  bad="$(awk '
    /^dnf_retry\(\) \{/ { indef = 1 }
    indef { if ($0 ~ /^\}/) indef = 0; next }
    /^[[:space:]]*#/ { next }
    /bash -[a-z]*c \x27[[:space:]]*$/ { body = 1; spliced = 0; next }
    /declare -f dnf_retry/ { spliced = 1; next }
    /(^|[^_[:alnum:]])dnf_retry[[:space:]]/ { if (body && !spliced) print NR": "$0 }
    body && /\x27[[:space:]]*$/ && !/declare -f/ { body = 0 }
  ' "${ROOT}/${f}")"
  check "${f}: every container dnf_retry call has the function spliced in" "" "${bad}"
done

# Run the real container body of check-nginx-org-host.sh (through a stub docker
# that runs the bash -euc body locally, cut before its first command) with a
# fake dnf: the body's shell must have dnf_retry defined and working.
mkdir "${WORK}/stub"
cat > "${WORK}/stub/docker" <<'DOCKER'
#!/usr/bin/env bash
[ "$1" = run ] || exit 0
while [ "$1" != "-euc" ]; do shift; done
body="$(printf '%s\n' "$2" | sed '/^fail() {/,$d')"
bash -euc "${body}
dnf_retry install -y probe"
DOCKER
chmod +x "${WORK}/stub/docker"
for mode in ok transient-always; do
  export FAKE_LOG="${WORK}/log" FAKE_MODE="${mode}" DNF_RETRY_SLEEP_UNIT=0
  : > "${FAKE_LOG}"
  PATH="${WORK}/stub:${WORK}/bin:${PATH}" "${ROOT}/test/package-test/check-nginx-org-host.sh" img 1.0 /x/p.rpm >/dev/null 2>"${WORK}/err"
  rc=$?
  want=0; [ "${mode}" = ok ] || want=1
  check "org-host container body ${mode}: rc" "${want}" "${rc}"
  check "org-host container body ${mode}: tries" "$([ "${mode}" = ok ] && echo 1 || echo 3)" "$(grep -c -v '^dnf clean' "${FAKE_LOG}")"
done


[ "${FAILS}" -eq 0 ] && echo "all dnf_retry checks passed" || { echo "${FAILS} check(s) failed"; exit 1; }
