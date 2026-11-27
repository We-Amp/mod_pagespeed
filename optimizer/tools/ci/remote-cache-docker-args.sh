# shellcheck shell=bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Source this file; it defines remote_cache_docker_args. Keep in sync with the
# mod_pagespeed copy.
#
# remote_cache_docker_args ARRAY: append the `docker run` flags that give a
# build container the remote Bazel cache to the bash array named ARRAY:
#
#   --add-host=bazel-remote-cache:${BAZEL_CACHE_HOST}
#       when BAZEL_CACHE_HOST is set (the alias the remote-cache config uses);
#   --volume=${BAZEL_CACHE_AUTH_RC}:/etc/bazel.bazelrc:ro
#       in addition, when BAZEL_CACHE_AUTH_RC names a readable file: a bazelrc
#       fragment with optional cache credentials, which Bazel inside the
#       container reads as its system rc. Unset, or not a readable file, means
#       no mount and the cache is used without it.
#
# With BAZEL_CACHE_HOST unset nothing is appended. Both runner variables are
# described in docs/ci/runner-environment.md. Every container that maps the
# cache alias gets its flags from this function, so the two always travel
# together (tools/ci/check-workflow-shape.sh, rule cache-auth).
#
# Each flag is one word, so the array may also be joined with spaces into a
# string that is expanded unquoted later; the mount is therefore skipped when
# the path is not absolute or holds a character other than [A-Za-z0-9_./-].
# The function never reads or prints the file's contents.
#
# Test: tools/ci/test_remote_cache_docker_args.sh

remote_cache_docker_args() {
  local _bcda_arr="${1:-}"
  case "${_bcda_arr}" in
    '' | [0-9]* | *[!A-Za-z0-9_]*)
      echo "::error::remote_cache_docker_args: '${_bcda_arr}' is not an array name" >&2
      return 2 ;;
  esac
  [ -n "${BAZEL_CACHE_HOST:-}" ] || return 0
  eval "${_bcda_arr}+=(\"--add-host=bazel-remote-cache:\${BAZEL_CACHE_HOST}\")"
  [ -n "${BAZEL_CACHE_AUTH_RC:-}" ] || return 0
  case "${BAZEL_CACHE_AUTH_RC}" in
    /*) ;;
    *) echo "::warning::BAZEL_CACHE_AUTH_RC is not an absolute path; using the cache without it" >&2
       return 0 ;;
  esac
  case "${BAZEL_CACHE_AUTH_RC}" in
    *[!A-Za-z0-9_./-]*)
      echo "::warning::BAZEL_CACHE_AUTH_RC holds an unsupported character; using the cache without it" >&2
      return 0 ;;
  esac
  if [ ! -f "${BAZEL_CACHE_AUTH_RC}" ] || [ ! -r "${BAZEL_CACHE_AUTH_RC}" ]; then
    echo "::warning::BAZEL_CACHE_AUTH_RC is not a readable file; using the cache without it" >&2
    return 0
  fi
  eval "${_bcda_arr}+=(\"--volume=\${BAZEL_CACHE_AUTH_RC}:/etc/bazel.bazelrc:ro\")"
}
