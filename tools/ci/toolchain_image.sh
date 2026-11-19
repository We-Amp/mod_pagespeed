#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The toolchain images of the focal and jammy build legs.
#
# The nginx module's focal leg (the sidecar pair, tools/ci/release_sidecar.sh)
# and jammy leg (the jammy deb, tools/ci/release_nginx.sh) build in the
# toolchain images of tools/ci/toolchain-images/: the stock Ubuntu image with
# g++-13 (Ubuntu toolchain PPA) and clang 18 (apt.llvm.org) installed once, when
# the image was built. A build in such an image installs nothing from those two
# repositories, so it does not depend on them being up. The images are named
# by digest in install/nginx/toolchain-images.lock, a file of the tree that is
# built, so a release builds with the images its own commit names.
#
# Source this file for the functions, or run it:
#   toolchain_image.sh ref SUITE [LOCK]     print the image of SUITE
#   toolchain_image.sh verify-lock [LOCK]   check the attestation of every
#                                           image in LOCK (TOOLCHAIN_IMAGE_VERIFY
#                                           defaults to `required` here)
#   toolchain_image.sh --self-test
# LOCK defaults to install/nginx/toolchain-images.lock of this tree.
#
# toolchain_image_ref LOCK SUITE: print the image of SUITE from LOCK,
#   <image>@sha256:<64 hex digits>. Fails closed: a missing file, no line or
#   more than one line for SUITE, or a line that is not exactly
#   SUITE=ghcr.io/we-amp/mod-pagespeed-toolchain-SUITE@sha256:<64 hex digits>
#   prints an ::error:: line and returns 1. Only a digest is accepted, never a
#   tag.
#
# toolchain_image_verify IMAGE: check the build provenance attestation of
#   IMAGE with `gh attestation verify`: it must come from
#   .github/workflows/toolchain-images.yml of We-Amp/mod_pagespeed, run from
#   refs/heads/master.
#   TOOLCHAIN_IMAGE_VERIFY says what happens:
#     required  verify; fail when gh cannot verify (no gh, a gh without the
#               attestation command, no token) or the attestation does not
#               verify.
#     auto      (the default) verify when gh can; fail when the attestation
#               does not verify; when gh cannot verify, print a warning and go
#               on. Some build runners have a gh without the attestation
#               command. The pull is by digest, so the bytes are exact either
#               way; the CI tooling job (ci.yml, hosted) runs verify-lock on
#               every pull request and merge group, so every digest that
#               reaches master has been verified.
#     off       skip the check.
#   gh needs a token (GH_TOKEN) to read the attestations.
#
# toolchain_image_for SUITE TREE STOCK: print the image a SUITE leg builds in.
#   With TOOLCHAIN_PPA_FALLBACK=1 that is STOCK, the stock image, and the
#   recipe installs the compilers from the PPA and apt.llvm.org as it did
#   before the images (for a local build, or an emergency when GHCR is out);
#   the build containers must be given TOOLCHAIN_PPA_FALLBACK=1 as well.
#   Otherwise it is the image of SUITE in TREE/install/nginx/toolchain-images.lock,
#   after toolchain_image_verify.

TOOLCHAIN_IMAGE_REPO="We-Amp/mod_pagespeed"
TOOLCHAIN_IMAGE_SIGNER="We-Amp/mod_pagespeed/.github/workflows/toolchain-images.yml"
TOOLCHAIN_IMAGE_SOURCE_REF="refs/heads/master"
TOOLCHAIN_IMAGE_LOCK_PATH="install/nginx/toolchain-images.lock"

toolchain_image_ref() {
  local lock="${1:?toolchain_image_ref: lock file}" suite="${2:?toolchain_image_ref: suite}" lines line
  case "$suite" in focal|jammy) ;; *) echo "::error::toolchain image: no toolchain image for suite '${suite}'" >&2; return 1 ;; esac
  if [ ! -f "$lock" ]; then
    echo "::error::toolchain image: ${lock} is missing" >&2
    return 1
  fi
  lines="$(grep -E "^[[:space:]]*${suite}[[:space:]]*=" "$lock" || true)"
  if [ -z "$lines" ]; then
    echo "::error::toolchain image: ${lock} has no ${suite}= line" >&2
    return 1
  fi
  if [ "$(printf '%s\n' "$lines" | wc -l)" -ne 1 ]; then
    echo "::error::toolchain image: ${lock} has more than one ${suite}= line" >&2
    return 1
  fi
  line="$lines"
  if ! printf '%s\n' "$line" | grep -Eqx "${suite}=ghcr\.io/we-amp/mod-pagespeed-toolchain-${suite}@sha256:[0-9a-f]{64}"; then
    echo "::error::toolchain image: the ${suite} line of ${lock} is not ${suite}=ghcr.io/we-amp/mod-pagespeed-toolchain-${suite}@sha256:<64 hex digits>: ${line}" >&2
    return 1
  fi
  printf '%s\n' "${line#*=}"
}

# gh_can_verify: 0 when gh is there, knows `attestation verify` and has a token.
toolchain_image_gh_can_verify() {
  command -v gh >/dev/null 2>&1 || return 1
  gh attestation verify --help >/dev/null 2>&1 || return 1
  [ -n "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ] || gh auth status >/dev/null 2>&1
}

toolchain_image_verify() {
  local image="${1:?toolchain_image_verify: image}" mode="${TOOLCHAIN_IMAGE_VERIFY:-auto}" out
  case "$mode" in
    off)
      echo "toolchain image: attestation check skipped (TOOLCHAIN_IMAGE_VERIFY=off): ${image}" >&2
      return 0 ;;
    auto|required) ;;
    *) echo "::error::toolchain image: TOOLCHAIN_IMAGE_VERIFY must be required, auto or off, not '${mode}'" >&2; return 1 ;;
  esac
  if ! toolchain_image_gh_can_verify; then
    if [ "$mode" = required ]; then
      echo "::error::toolchain image: cannot check the attestation of ${image}: needs gh with the attestation command and a token (GH_TOKEN)" >&2
      return 1
    fi
    echo "::warning::toolchain image: attestation not checked here (no gh with the attestation command, or no token); the image is pulled by digest: ${image}" >&2
    return 0
  fi
  if ! out="$(gh attestation verify "oci://${image}" -R "${TOOLCHAIN_IMAGE_REPO}" \
        --signer-workflow "${TOOLCHAIN_IMAGE_SIGNER}" --source-ref "${TOOLCHAIN_IMAGE_SOURCE_REF}" 2>&1)"; then
    printf '%s\n' "$out" >&2
    echo "::error::toolchain image: the attestation of ${image} does not verify against ${TOOLCHAIN_IMAGE_SIGNER}" >&2
    return 1
  fi
  echo "toolchain image: attestation verified (${TOOLCHAIN_IMAGE_SIGNER}): ${image}" >&2
}

toolchain_image_for() {
  local suite="${1:?toolchain_image_for: suite}" tree="${2:?toolchain_image_for: tree}" stock="${3:?toolchain_image_for: stock image}" image
  if [ "${TOOLCHAIN_PPA_FALLBACK:-}" = 1 ]; then
    echo "::warning::toolchain image: TOOLCHAIN_PPA_FALLBACK=1, the ${suite} leg builds in ${stock} and installs its compilers from the Ubuntu toolchain PPA and apt.llvm.org" >&2
    printf '%s\n' "$stock"
    return 0
  fi
  image="$(toolchain_image_ref "${tree}/${TOOLCHAIN_IMAGE_LOCK_PATH}" "$suite")" || return 1
  toolchain_image_verify "$image" || return 1
  printf '%s\n' "$image"
}

toolchain_image_self_test() {
  local t fail=0 d
  t="$(mktemp -d)"
  d="$(printf 'a%.0s' $(seq 1 64))"
  chk() { if [ "$2" = "$3" ]; then echo "ok   - $1"; else echo "FAIL - $1: got '$2', want '$3'"; fail=1; fi; }
  rejects() { if toolchain_image_ref "$t/lock" "$2" >/dev/null 2>&1; then echo "FAIL - $1: accepted"; fail=1; else echo "ok   - $1: refused"; fi; }

  printf '# comment\nfocal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\njammy=ghcr.io/we-amp/mod-pagespeed-toolchain-jammy@sha256:%s\n' "$d" "$d" > "$t/lock"
  chk "focal line" "$(toolchain_image_ref "$t/lock" focal 2>/dev/null)" "ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:$d"
  chk "jammy line" "$(toolchain_image_ref "$t/lock" jammy 2>/dev/null)" "ghcr.io/we-amp/mod-pagespeed-toolchain-jammy@sha256:$d"
  rejects "unknown suite" noble
  rm -f "$t/lock"; rejects "missing file" focal
  printf 'jammy=ghcr.io/we-amp/mod-pagespeed-toolchain-jammy@sha256:%s\n' "$d" > "$t/lock"; rejects "no focal line" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal:latest\n' > "$t/lock"; rejects "a tag" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal:v1@sha256:%s\n' "$d" > "$t/lock"; rejects "a tag with a digest" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\n' "${d%a}" > "$t/lock"; rejects "a short digest" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\n' "${d%a}A" > "$t/lock"; rejects "an upper-case digest" focal
  printf 'focal=ghcr.io/someone/mod-pagespeed-toolchain-focal@sha256:%s\n' "$d" > "$t/lock"; rejects "another registry owner" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-jammy@sha256:%s\n' "$d" > "$t/lock"; rejects "the image of another suite" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s \n' "$d" > "$t/lock"; rejects "trailing text" focal
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\nfocal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\n' "$d" "$d" > "$t/lock"; rejects "two focal lines" focal
  printf ' focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\n' "$d" > "$t/lock"; rejects "an indented line" focal

  # The checked-in lock has a valid line for both suites.
  local root; root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  for s in focal jammy; do
    if toolchain_image_ref "$root/$TOOLCHAIN_IMAGE_LOCK_PATH" "$s" >/dev/null; then echo "ok   - checked-in lock: $s"; else echo "FAIL - checked-in lock: $s"; fail=1; fi
  done

  # The fallback takes the stock image and reads no lock; without it the lock is read.
  mkdir -p "$t/tree/install/nginx"
  chk "fallback: stock image" "$(TOOLCHAIN_PPA_FALLBACK=1 toolchain_image_for focal "$t/tree" ubuntu:20.04@sha256:"$d" 2>/dev/null)" "ubuntu:20.04@sha256:$d"
  if TOOLCHAIN_PPA_FALLBACK=0 toolchain_image_for focal "$t/tree" ubuntu:20.04 >/dev/null 2>&1; then echo "FAIL - no lock in the tree: accepted"; fail=1; else echo "ok   - no lock in the tree: refused"; fi
  printf 'focal=ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:%s\n' "$d" > "$t/tree/$TOOLCHAIN_IMAGE_LOCK_PATH"
  chk "lock image" "$(TOOLCHAIN_IMAGE_VERIFY=off toolchain_image_for focal "$t/tree" ubuntu:20.04 2>/dev/null)" "ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:$d"

  # The attestation policy, with a fake gh: auto warns without a capable gh
  # and fails on a failed verification; required fails without a capable gh.
  mkdir -p "$t/bin"
  printf '#!/bin/sh\nexit 1\n' > "$t/bin/gh"; chmod +x "$t/bin/gh"
  local img="ghcr.io/we-amp/mod-pagespeed-toolchain-focal@sha256:$d"
  if PATH="$t/bin:$PATH" GH_TOKEN=x TOOLCHAIN_IMAGE_VERIFY=auto toolchain_image_verify "$img" 2>/dev/null; then echo "ok   - auto, old gh: goes on"; else echo "FAIL - auto, old gh"; fail=1; fi
  if PATH="$t/bin:$PATH" GH_TOKEN=x TOOLCHAIN_IMAGE_VERIFY=required toolchain_image_verify "$img" 2>/dev/null; then echo "FAIL - required, old gh: accepted"; fail=1; else echo "ok   - required, old gh: refused"; fi
  printf '#!/bin/sh\ncase "$*" in *--help*) exit 0 ;; esac\necho "verification failed" >&2\nexit 1\n' > "$t/bin/gh"
  if PATH="$t/bin:$PATH" GH_TOKEN=x TOOLCHAIN_IMAGE_VERIFY=auto toolchain_image_verify "$img" 2>/dev/null; then echo "FAIL - auto, bad attestation: accepted"; fail=1; else echo "ok   - auto, bad attestation: refused"; fi
  printf '#!/bin/sh\ncase "$*" in *--help*) exit 0 ;; esac\necho "$*" > "%s/args"\nexit 0\n' "$t" > "$t/bin/gh"
  if PATH="$t/bin:$PATH" GH_TOKEN=x TOOLCHAIN_IMAGE_VERIFY=required toolchain_image_verify "$img" 2>/dev/null; then echo "ok   - required, good attestation: accepted"; else echo "FAIL - required, good attestation"; fail=1; fi
  chk "gh arguments" "$(cat "$t/args" 2>/dev/null)" "attestation verify oci://$img -R We-Amp/mod_pagespeed --signer-workflow We-Amp/mod_pagespeed/.github/workflows/toolchain-images.yml --source-ref refs/heads/master"
  if TOOLCHAIN_IMAGE_VERIFY=sometimes toolchain_image_verify "$img" 2>/dev/null; then echo "FAIL - unknown mode: accepted"; fail=1; else echo "ok   - unknown mode: refused"; fi

  rm -rf "$t"
  [ "$fail" -eq 0 ] && echo "toolchain_image: all ok"
  return "$fail"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  case "${1:-}" in
    ref)
      toolchain_image_ref "${3:-$root/$TOOLCHAIN_IMAGE_LOCK_PATH}" "${2:?usage: toolchain_image.sh ref SUITE [LOCK]}" ;;
    verify-lock)
      lock="${2:-$root/$TOOLCHAIN_IMAGE_LOCK_PATH}"
      export TOOLCHAIN_IMAGE_VERIFY="${TOOLCHAIN_IMAGE_VERIFY:-required}"
      for suite in focal jammy; do
        image="$(toolchain_image_ref "$lock" "$suite")"
        toolchain_image_verify "$image"
      done ;;
    --self-test) toolchain_image_self_test ;;
    *) echo "usage: toolchain_image.sh ref SUITE [LOCK] | verify-lock [LOCK] | --self-test" >&2; exit 2 ;;
  esac
fi
