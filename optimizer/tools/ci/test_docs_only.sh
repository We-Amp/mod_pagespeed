#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/docs_only.sh: a temporary git repository with a root
# commit holding a few files and one child commit per case.
#
# Run: tools/ci/test_docs_only.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS="${HERE}/docs_only.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
REPO="${WORK}/repo"
ERRF="${WORK}/stderr.txt"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# case_repo <op>...: a root commit with src/a.cc, docs/a.md and README.md,
# then one commit applying each op: `add:<path>`, `rm:<path>` or
# `mv:<from>:<to>`. Sets BASE (the root) and TIP.
case_repo() {
  local op a b
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  mkdir -p "${REPO}/src" "${REPO}/docs"
  echo x > "${REPO}/src/a.cc"
  echo x > "${REPO}/docs/a.md"
  echo x > "${REPO}/README.md"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m root
  BASE="$(git -C "${REPO}" rev-parse HEAD)"
  for op in "$@"; do
    case "${op}" in
      add:*)
        a="${op#add:}"
        mkdir -p "${REPO}/$(dirname "${a}")"
        echo "y${RANDOM}" >> "${REPO}/${a}" ;;
      rm:*)
        git -C "${REPO}" rm -q "${op#rm:}" ;;
      mv:*)
        a="${op#mv:}"; b="${a#*:}"; a="${a%%:*}"
        mkdir -p "${REPO}/$(dirname "${b}")"
        git -C "${REPO}" mv "${a}" "${b}" ;;
    esac
  done
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q --allow-empty -m change
  TIP="$(git -C "${REPO}" rev-parse HEAD)"
}

# expect <name> <want true|false> [base] [head]
expect() {
  local name="$1" want="$2" base="${3-${BASE}}" head="${4-${TIP}}" out reason rc=0
  out="$(cd "${REPO}" && bash "${DOCS}" "${base}" "${head}" 2> "${ERRF}")" || rc=$?
  reason="$(cat "${ERRF}")"
  if [ "${rc}" -ne 0 ]; then
    fail "${name}: exit ${rc}"
  elif [ "${out}" != "docs_only=${want}" ]; then
    fail "${name}: stdout '${out}', want docs_only=${want} (${reason})"
  elif ! printf '%s\n' "${reason}" | grep -qE '^docs lane: docs_only=(true|false) \('; then
    fail "${name}: no reason line on stderr: ${reason}"
  else
    pass "${name}"
  fi
}

# Shared by both product repositories.
case_repo add:docs/b.md;                        expect docs-file true
case_repo add:README.md;                        expect top-level-md true
case_repo add:CHANGELOG.md add:docs/x/y.md;     expect two-docs-files true
case_repo add:.github/ISSUE_TEMPLATE/bug.md;    expect issue-template true
case_repo add:.github/pull_request_template.md; expect pr-template true
case_repo rm:docs/a.md;                         expect deleted-docs-only true
case_repo add:src/b.cc;                         expect code-file false
case_repo add:docs/b.md add:src/b.cc;           expect mixed false
case_repo mv:src/a.cc:docs/a.cc;                expect rename-code-to-docs false
case_repo mv:docs/a.md:src/a.md;                expect rename-docs-to-code false
case_repo rm:src/a.cc;                          expect deleted-code false
case_repo add:src/README.md;                    expect nested-md-outside-docs false
case_repo add:tools/ci/docs_only.sh;            expect classifier-itself false
case_repo add:.github/workflows/ci.yml;         expect workflow false
case_repo add:.github/pull_request_template.md.bak; expect template-lookalike false
case_repo add:MODULE.bazel;                     expect build-file false
case_repo;                                      expect empty-diff false
case_repo add:docs/b.md;                        expect no-base false ""
case_repo add:docs/b.md;                        expect unknown-base false 1234567890123456789012345678901234567890
case_repo add:docs/b.md;                        expect unknown-head false "${BASE}" 1234567890123456789012345678901234567890
case_repo add:docs/b.md;                        expect base-is-head false "${TIP}" "${TIP}"

# Merge-base semantics: a base that moved on after the branch point is not
# part of the change.
case_repo add:docs/b.md
DOCS_TIP="${TIP}"
git -C "${REPO}" checkout -q -b other "${BASE}"
echo z > "${REPO}/src/z.cc"
git -C "${REPO}" add -A
git -C "${REPO}" commit -q -m "base moved on"
expect base-moved-on true "$(git -C "${REPO}" rev-parse HEAD)" "${DOCS_TIP}"

# Specific to this repository's allowlist.
case_repo add:website/src/pages/a.astro;        expect website true
case_repo add:website/Dockerfile;               expect website-dockerfile true
case_repo add:shared/product-facts.mjs;         expect shared-facts false
case_repo add:src/worker/main.cc;               expect reference-source false
case_repo add:docker/nginx-version.txt;         expect nginx-version false

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All docs-only classifier cases passed"
