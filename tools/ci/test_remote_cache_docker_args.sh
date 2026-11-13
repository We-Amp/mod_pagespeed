#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for tools/ci/remote-cache-docker-args.sh: no cache host appends
# nothing; a cache host alone appends only the host mapping; the credentials
# mount is added only when BAZEL_CACHE_AUTH_RC names a readable file; the
# file's contents never reach the output.
#
# Run: tools/ci/test_remote_cache_docker_args.sh
# shellcheck disable=SC2016  # the bash -c bodies expand in the child shell
set -uo pipefail
HELPER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/remote-cache-docker-args.sh"
WORK="$(mktemp -d)"
trap 'chmod -R u+rw "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
fail=0
check() { # label expected actual
  if [ "$2" != "$3" ]; then echo "FAIL: $1: expected [$2] got [$3]" >&2; fail=1; else echo "ok: $1"; fi
}
# run VAR=VALUE...: the array the helper builds (one flag per '|'), then the
# helper's stderr after '#'.
run() {
  env -u BAZEL_CACHE_HOST -u BAZEL_CACHE_AUTH_RC "$@" bash -c '
    set -euo pipefail
    source "$1"
    A=(--rm)
    remote_cache_docker_args A 2>"$2/err"
    printf "%s|" "${A[@]}"
    printf "#%s" "$(cat "$2/err")"' _ "${HELPER}" "${WORK}"
}

RC="${WORK}/auth.bazelrc"
printf '# secret-marker\n' > "${RC}"
chmod 600 "${RC}"
H="--add-host=bazel-remote-cache:cache.invalid"
M="--volume=${RC}:/etc/bazel.bazelrc:ro"

check "no cache host"              "--rm|#"        "$(run)"
check "no cache host, rc set"      "--rm|#"        "$(run BAZEL_CACHE_AUTH_RC="${RC}")"
check "cache host, rc unset"       "--rm|${H}|#"   "$(run BAZEL_CACHE_HOST=cache.invalid)"
check "cache host, rc empty"       "--rm|${H}|#"   "$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC=)"
check "cache host, rc readable"    "--rm|${H}|${M}|#" "$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${RC}")"

out="$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${WORK}/missing.bazelrc")"
case "${out}" in "--rm|${H}|#::warning::BAZEL_CACHE_AUTH_RC is not a readable file"*) echo "ok: rc missing";; *) echo "FAIL: rc missing -> ${out}" >&2; fail=1;; esac
out="$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${WORK}")"
case "${out}" in "--rm|${H}|#::warning::BAZEL_CACHE_AUTH_RC is not a readable file"*) echo "ok: rc is a directory";; *) echo "FAIL: rc is a directory -> ${out}" >&2; fail=1;; esac
if [ "$(id -u)" -ne 0 ]; then
  cp "${RC}" "${WORK}/locked.bazelrc"; chmod 000 "${WORK}/locked.bazelrc"
  out="$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${WORK}/locked.bazelrc")"
  case "${out}" in "--rm|${H}|#::warning::BAZEL_CACHE_AUTH_RC is not a readable file"*) echo "ok: rc unreadable";; *) echo "FAIL: rc unreadable -> ${out}" >&2; fail=1;; esac
else
  echo "skip: rc unreadable (running as root)"
fi
out="$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC=auth.bazelrc)"
case "${out}" in "--rm|${H}|#::warning::BAZEL_CACHE_AUTH_RC is not an absolute path"*) echo "ok: relative path";; *) echo "FAIL: relative path -> ${out}" >&2; fail=1;; esac
mkdir "${WORK}/a b"; cp "${RC}" "${WORK}/a b/auth.bazelrc"
out="$(run BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${WORK}/a b/auth.bazelrc")"
case "${out}" in "--rm|${H}|#::warning::BAZEL_CACHE_AUTH_RC holds an unsupported character"*) echo "ok: path with a space";; *) echo "FAIL: path with a space -> ${out}" >&2; fail=1;; esac

# The contents never reach stdout or stderr, also under xtrace.
out="$(env -u BAZEL_CACHE_AUTH_RC BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${RC}" bash -xc 'source "$1"; A=(); remote_cache_docker_args A; printf "%s\n" "${A[@]}"' _ "${HELPER}" 2>&1)"
case "${out}" in *secret-marker*) echo "FAIL: the rc contents reached the output" >&2; fail=1;; *) echo "ok: contents never printed";; esac

# The joined form survives an unquoted expansion as separate words.
got="$(env BAZEL_CACHE_HOST=cache.invalid BAZEL_CACHE_AUTH_RC="${RC}" bash -c 'source "$1"; A=(); remote_cache_docker_args A; S="${A[*]}"; set -- ${S}; echo "$#"' _ "${HELPER}")"
check "joined string splits into two words" "2" "${got}"

# A bad array name is an error.
env BAZEL_CACHE_HOST=cache.invalid bash -c 'source "$1"; remote_cache_docker_args "A;x"' _ "${HELPER}" >/dev/null 2>&1
check "bad array name returns 2" "2" "$?"

exit "${fail}"
