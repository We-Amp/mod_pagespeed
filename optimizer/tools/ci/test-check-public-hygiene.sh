#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the public-tree hygiene gate. Hashed-token rules are tested
# with a temporary token file of MADE-UP tokens (never the real list), through
# check-public-hygiene.py --tokens, so no real non-public token appears here.
# The former-license regex rules are plain; their fixtures are assembled from
# fragments so this file itself does not match them. Every failing case also
# asserts the expected rule/class in the gate's stderr.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sha() { printf '%s' "$1" | tr 'A-Z' 'a-z' | shasum -a 256 | cut -d' ' -f1; }

TOK="$(mktemp)"
{
  printf 'internal-hosts\tall\t%s\tblock\n' "$(sha selfhost-01)"
  printf 'private-repos\tall\t%s\tblock\n' "$(sha selftest-org/selftest-repo)"
  printf 'dangling-tooling\tall\t%s\tblock\n' "$(sha selftest-tools/selftest-dir)"
  printf 'held-back-docs\tmd\t%s\tblock\n' "$(sha selftest-plans/selftest-doc)"
  printf 'build-infra\tall\t%s\tadvisory\n' "$(sha selftest-fleet-path)"
} > "$TOK"
trap 'rm -f "$TOK"' EXIT

# run_case <name> <pass|fail> <expected-stderr-fragment|-> <file> <content>
# runs the full gate (regex rules) and, with the made-up tokens, the checker.
run_case() {
  local name="$1" expect="$2" frag="$3" file="$4" content="$5" tmp err rc=0
  tmp="$(mktemp -d)"
  mkdir -p "$tmp/tools/ci" "$tmp/.github" "$tmp/docs"
  cp "$ROOT/tools/ci/check-public-hygiene."{sh,py} "$tmp/tools/ci/"
  cp "$ROOT/.github/hygiene-rules.txt" "$tmp/.github/"
  cp "$TOK" "$tmp/.github/hygiene-hashed-tokens.txt"   # made-up tokens only
  printf 'ok\n' > "$tmp/docs/a.md"
  mkdir -p "$(dirname "$tmp/$file")"
  printf '%s\n' "$content" > "$tmp/$file"
  err="$( (cd "$tmp" && git init -q && git add -A && tools/ci/check-public-hygiene.sh) 2>&1 >/dev/null )" || rc=$?
  rm -rf "$tmp"
  if [[ "$expect" == pass && $rc -ne 0 ]] || [[ "$expect" == fail && $rc -eq 0 ]]; then
    echo "FAIL: $name (expected $expect, rc=$rc)" >&2; return 1
  fi
  if [[ "$expect" == fail && "$frag" != "-" && "$err" != *"$frag"* ]]; then
    echo "FAIL: $name (stderr lacks '$frag')" >&2; echo "$err" >&2; return 1
  fi
  echo "ok: $name"
}

# Fragments for the plain license rules (kept split so this file stays clean).
B="BU"; S="SL"
rc=0
run_case clean pass - docs/b.md 'See [a](a.md), [dir](../docs) and [ref][r].
[r]: a.md'
run_case host fail "class internal-hosts" docs/b.txt 'run on selfhost-01 now' || rc=1
run_case host-dot-suffix fail "class internal-hosts" docs/b.txt 'ssh selfhost-01.local' || rc=1
run_case host-lan-port fail "class internal-hosts" docs/b.txt 'selfhost-01.lan:22' || rc=1
run_case host-underscore fail "class internal-hosts" docs/b.txt 'selfhost_01 up' || rc=1
run_case host-negative pass - docs/b.txt 'selfhost-02 and selfhosting' || rc=1
run_case repo-slug fail "class private-repos" docs/b.md 'https://github.com/selftest-org/selftest-repo' || rc=1
run_case repo-git-suffix fail "class private-repos" docs/b.txt 'git@github.com:selftest-org/selftest-repo.git' || rc=1
run_case repo-negative pass - docs/b.md 'github.com/selftest-org/selftest-repo-other' || rc=1
run_case backslash-path fail "class dangling-tooling" docs/b.txt 'see selftest-tools\selftest-dir\x' || rc=1
run_case md-only fail "class held-back-docs" docs/b.md 'see selftest-plans/selftest-doc/x.md' || rc=1
run_case md-only-nonmd pass - docs/b.txt 'see selftest-plans/selftest-doc/x.md' || rc=1
run_case mdx-md-only fail "class held-back-docs" docs/b.mdx 'see selftest-plans/selftest-doc/x' || rc=1
run_case advisory-only-warns pass - docs/b.txt 'selftest-fleet-path' || rc=1
run_case license-header fail "SPDX-License-Identifier" docs/b.txt "SPDX-License-Identifier: ${B}${S}-1.1" || rc=1
run_case license-header-negative pass - docs/b.txt 'SPDX-License-Identifier: Apache-2.0' || rc=1
run_case license-title fail "Source License" docs/LICENSE.txt "Bus""iness Source License 1.1" || rc=1
run_case license-title-negative pass - docs/b.md 'prose about a business source license in passing' || rc=1
run_case dead-link fail "dead relative link" docs/b.md 'see [x](missing.md)' || rc=1
run_case dead-link-mdx fail "dead relative link" docs/b.mdx 'see [x](missing.md)' || rc=1
run_case dead-link-ref fail "dead relative link" docs/b.md '[r]: missing.md' || rc=1
run_case dead-link-parens fail "dead relative link" docs/b.md 'see [x](missing_(1).md)' || rc=1
run_case dead-link-in-fence pass - docs/b.md $'```\n[x](missing.md)\n```' || rc=1
run_case dead-link-in-code-span pass - docs/b.md 'use `[x](missing.md)` syntax' || rc=1
run_case url-link pass - docs/b.md '[x](https://example.com/y) [f](#frag)' || rc=1

# --tokens option: the checker alone, real list not consulted.
tmp="$(mktemp -d)"; (cd "$tmp" && git init -q && printf 'selfhost-01\n' > f.txt && git add -A)
if python3 -I "$ROOT/tools/ci/check-public-hygiene.py" --root "$tmp" --tokens "$TOK" >/dev/null 2>&1; then
  echo "FAIL: --tokens option" >&2; rc=1
else echo "ok: --tokens option"; fi
rm -rf "$tmp"
exit $rc
