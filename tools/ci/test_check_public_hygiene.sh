#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the credential-literal pass of tools/ci/check-public-hygiene.sh.
# Each case builds a throwaway git repository holding copies of the gate and
# its rules plus one fixture file, and runs the gate there. Fake secrets are
# assembled from fragments at run time so this file itself never matches.
#
# Run: tools/ci/test_check_public_hygiene.sh
# shellcheck disable=SC2016  # PowerShell fixtures keep $ literal on purpose
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FAILS=0
CASE=0

# run_case <expect: pass|fail> <description> <fixture-name> <fixture-content>
run_case() {
  local expect="$1" desc="$2" name="$3" content="$4" repo rc=0 out
  CASE=$((CASE + 1))
  repo="${WORK}/case${CASE}"
  mkdir -p "${repo}/tools/ci" "${repo}/.github"
  cp "${ROOT}/tools/ci/check-public-hygiene.sh" "${repo}/tools/ci/"
  cp "${ROOT}/.github/hygiene-rules.txt" "${repo}/.github/"
  { grep -v '^allow-path' "${ROOT}/.github/hygiene-credential-rules.txt"; printf '%b' "${EXTRA_RULES:-}"; } \
    > "${repo}/.github/hygiene-credential-rules.txt"
  mkdir -p "$(dirname "${repo}/${name}")"
  printf '%s\n' "$content" > "${repo}/${name}"
  git -C "$repo" init -q
  git -C "$repo" add -A
  out="$(bash "${repo}/tools/ci/check-public-hygiene.sh" 2>&1)" || rc=$?
  if [[ "$expect" == fail && -n "${EXPECT_MSG:-}" ]] && ! grep -qF -e "$EXPECT_MSG" <<<"$out"; then
    rc=0
  fi
  if [[ "$expect" == fail && $rc -ne 0 ]] || [[ "$expect" == pass && $rc -eq 0 ]]; then
    echo "PASS: ${desc}"
  else
    echo "FAIL: ${desc} (expected ${expect}, rc=${rc})"
    printf '%s\n' "$out" | sed 's/^/    /'
    FAILS=$((FAILS + 1))
  fi
}

PW='hunter2hunter2'
Q="'"
D='"'

# (a) ConvertTo-SecureString literals
run_case fail "a: single-quoted literal" t.ps1 "ConvertTo-SecureString ${Q}${PW}${Q} -AsPlainText -Force"
run_case fail "a: double-quoted literal, -String form" t.ps1 "ConvertTo-SecureString -String ${D}${PW}${D} -AsPlainText -Force"
run_case fail "a: -AsPlainText first" t.ps1 "ConvertTo-SecureString -AsPlainText -Force ${Q}${PW}${Q}"
run_case fail "a: pipeline form" t.ps1 "${Q}${PW}${Q} | ConvertTo-SecureString -AsPlainText -Force"
run_case pass "a: env variable argument" t.ps1 'ConvertTo-SecureString $env:GUEST_PW -AsPlainText -Force'
run_case pass "a: quoted variable reference" t.ps1 'ConvertTo-SecureString "$env:GUEST_PW" -AsPlainText -Force'
run_case pass "a: no -AsPlainText" t.ps1 "ConvertTo-SecureString ${Q}${PW}${Q}"

# (b) PSCredential literals
run_case fail "b: literal second argument" t.ps1 "New-Object System.Management.Automation.PSCredential(${Q}admin${Q}, ${Q}${PW}${Q})"
run_case fail "b: variable user, double-quoted literal" t.ps1 "New-Object PSCredential(\$user, ${D}${PW}${D})"
run_case pass "b: variable second argument" t.ps1 'New-Object PSCredential($user, $secure)'

# (c) password-like assignments
run_case fail "c: shell password" t.sh "password=${D}${PW}${D}"
run_case fail "c: PowerShell variable" t.ps1 "\$Password = ${Q}${PW}${Q}"
run_case fail "c: YAML token" t.yml "token: ${D}${PW}${D}"
run_case fail "c: Python api_key" t.py "api_key = ${Q}${PW}${Q}"
run_case fail "c: JSON secret, case-insensitive" t.json "{${D}Secret${D}: ${D}${PW}${D}}"
run_case pass "c: short value" t.sh "password=${D}short${D}"
run_case pass "c: empty value" t.sh "password=${D}${D}"
run_case pass "c: changeme" t.yml "password: ${D}changeme-please${D}"
run_case pass "c: example" t.yml "password: ${D}example-password${D}"
run_case pass "c: angle placeholder" t.yml "token: ${D}<your-token-here>${D}"
run_case fail "c: ADMIN_PASSWORD" t.sh "ADMIN_PASSWORD=${D}${PW}${D}"
run_case fail "c: env-qualified PowerShell name" t.ps1 "\$env:ADMIN_PASSWORD = ${Q}${PW}${Q}"
run_case fail "c: camel-case PowerShell name" t.ps1 "\$AdminPassword = ${Q}${PW}${Q}"
run_case fail "c: GITHUB_TOKEN in YAML" t.yml "GITHUB_TOKEN: ${D}${PW}${D}"
run_case fail "c: db_password" t.sh "db_password=${D}${PW}${D}"
run_case fail "c: canary value" t.sh "password=${D}Qw7rTz9pLm2x${D}"
run_case fail "c: placeholder word only as a substring" t.sh "password=${D}Qw7rTestz9pLm2x${D}"
run_case fail "c: single-quoted dollar literal" t.ps1 "\$password = ${Q}\$Abcdefgh12${Q}"
run_case fail "c: x run inside a longer value" t.sh "password=${D}xxxQw7rTz9${D}"
run_case pass "c: GitHub Actions expression" t.yml "token: ${D}\${{ secrets.GITHUB_TOKEN }}${D}"
run_case pass "c: shell default expansion" t.sh "password=${D}\${DB_PASSWORD:-fallback}${D}"
run_case pass "c: xxx run" t.sh "password=${D}xxxxxxxxxx${D}"
run_case pass "c: shell expansion" t.sh "token=${D}\${CI_TOKEN_VALUE}${D}"
run_case pass "c: PowerShell env" t.ps1 "\$token = ${D}\$env:CI_TOKEN_VALUE${D}"
run_case pass "c: percent form" t.sh "password=${D}%PASSWORD_VALUE%${D}"
run_case pass "c: dummy" t.py "password = ${Q}dummy-value-1${Q}"
run_case pass "c: test" t.py "password = ${Q}test-value-123${Q}"
run_case pass "c: name only contains the word" t.sh "my_password_file=${D}${PW}${D}"
run_case pass "c: unlisted file type" t.txt "password=${D}${PW}${D}"
run_case pass "c: per-line marker" t.sh "password=${D}${PW}${D} # hygiene-allow-credential"

# (b) additional PSCredential forms
run_case fail "b: New-Object -ArgumentList" t.ps1 "New-Object PSCredential -ArgumentList ${Q}admin${Q}, ${Q}${PW}${Q}"
run_case fail "b: -ArgumentList first" t.ps1 "New-Object -ArgumentList ${Q}admin${Q}, ${Q}${PW}${Q} -TypeName PSCredential"
run_case pass "b: -ArgumentList with variable" t.ps1 'New-Object PSCredential -ArgumentList $user, $secure'
run_case fail "b: -Password single-quoted" t.ps1 "Set-Thing -Password ${Q}${PW}${Q}"
run_case fail "b: -Password double-quoted" t.ps1 "Set-Thing -Password ${D}${PW}${D}"
run_case pass "b: -Password variable" t.ps1 'Set-Thing -Password $secure'

# (d) private key headers
run_case fail "d: RSA" t.txt "-----BEGIN RSA ""PRIVATE KEY-----"
run_case fail "d: OPENSSH" t.txt "-----BEGIN OPENSSH ""PRIVATE KEY-----"
run_case pass "d: public key header" t.txt "-----BEGIN PUBLIC KEY-----"

# (e) token shapes
run_case fail "e: GitHub" t.txt "gh""p_$(printf 'A%.0s' $(seq 36))"
run_case fail "e: AWS" t.txt "AK""IA$(printf 'A%.0s' $(seq 16))"
run_case fail "e: Slack" t.txt "xo""xb-12345"
run_case fail "e: Google" t.txt "AI""za$(printf 'A%.0s' $(seq 35))"
run_case fail "e: GitHub fine-grained" t.txt "github""_pat_$(printf 'A%.0s' $(seq 25))"
run_case pass "e: too short GitHub" t.txt "gh""p_abc"
run_case pass "e: too short AWS" t.txt "AK""IAABC"

# gate configuration refusals
EXTRA_RULES='allow-path\t.\n'      EXPECT_MSG='allow-path' run_case fail "allow-path dot refused" t.sh 'x=1'
EXTRA_RULES='allow-path\t*\n'      EXPECT_MSG='allow-path' run_case fail "allow-path star refused" t.sh 'x=1'
EXTRA_RULES='allow-path\t**/*\n'   EXPECT_MSG='allow-path' run_case fail "allow-path globstar refused" t.sh 'x=1'
EXTRA_RULES='allow-path\t\n'       EXPECT_MSG='allow-path' run_case fail "allow-path empty refused" t.sh 'x=1'
EXTRA_RULES='allow-path\t..\n'     EXPECT_MSG='allow-path' run_case fail "allow-path dotdot refused" t.sh 'x=1'
EXTRA_RULES='allow-path\tmissing.sh\n' EXPECT_MSG='allow-path' run_case fail "allow-path untracked refused" t.sh 'x=1'
EXTRA_RULES='allow-path\tt.sh\n'   EXPECT_MSG='allow-path' run_case pass "allow-path exact tracked file" t.sh "password=${D}${PW}${D}"
EXTRA_RULES='placeholder\t\n'      EXPECT_MSG='empty placeholder' run_case fail "empty placeholder refused" t.sh 'x=1'
EXTRA_RULES='placeholder\t.\n'     EXPECT_MSG='canary' run_case fail "placeholder dot trips canary" t.sh 'x=1'
EXTRA_RULES='placeholder\t.*\n'    EXPECT_MSG='canary' run_case fail "placeholder dot-star trips canary" t.sh 'x=1'
EXTRA_RULES='placeholder-dq\t.\n'  EXPECT_MSG='canary' run_case fail "placeholder-dq dot trips canary" t.sh 'x=1'

# changelog fragments are scanned like any other tracked file
run_case fail "fragment under changelog.d/ is scanned" changelog.d/planted.fixed.md "- see AK""IA$(printf 'A%.0s' $(seq 16))"

# allowlist
run_case pass "clean tree" t.txt "nothing to see"
CASE=$((CASE + 1))
repo="${WORK}/case${CASE}"
mkdir -p "${repo}/tools/ci" "${repo}/.github" "${repo}/fixtures"
cp "${ROOT}/tools/ci/check-public-hygiene.sh" "${repo}/tools/ci/"
cp "${ROOT}/.github/hygiene-rules.txt" "${repo}/.github/"
{ grep -v '^allow-path' "${ROOT}/.github/hygiene-credential-rules.txt"
  printf 'allow-path\tfixtures/fake.sh\n'; } > "${repo}/.github/hygiene-credential-rules.txt"
printf 'password="%s"\n' "$PW" > "${repo}/fixtures/fake.sh"
git -C "$repo" init -q
git -C "$repo" add -A
if bash "${repo}/tools/ci/check-public-hygiene.sh" >/dev/null 2>&1; then
  echo "PASS: allow-path exempts a path"
else
  echo "FAIL: allow-path did not exempt the path"; FAILS=$((FAILS + 1))
fi

# The gate output must name the location but never echo the secret.
CASE=$((CASE + 1))
repo="${WORK}/case${CASE}"
mkdir -p "${repo}/tools/ci" "${repo}/.github"
cp "${ROOT}/tools/ci/check-public-hygiene.sh" "${repo}/tools/ci/"
cp "${ROOT}/.github/hygiene-rules.txt" "${repo}/.github/"
grep -v '^allow-path' "${ROOT}/.github/hygiene-credential-rules.txt" > "${repo}/.github/hygiene-credential-rules.txt"
printf 'password="%s"\n' "$PW" > "${repo}/t.sh"
git -C "$repo" init -q
git -C "$repo" add -A
out="$(bash "${repo}/tools/ci/check-public-hygiene.sh" 2>&1 || true)"
if grep -q 't.sh:1: secret-assignment' <<<"$out" && ! grep -qF "$PW" <<<"$out"; then
  echo "PASS: report names path:line and hides the value"
else
  echo "FAIL: report content"; FAILS=$((FAILS + 1))
fi

echo "cases: ${CASE}, failures: ${FAILS}"
[[ $FAILS -eq 0 ]]
