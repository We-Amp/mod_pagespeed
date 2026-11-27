#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Signs and verifies a release's SHA256SUMS with the We-Amp release signing
# key (see docs/ci/release.md). Both commands name the key by its full
# fingerprint and fail on any other key.
#
# Usage:
#   RELEASE_GPG_KEY=<base64 of the armored secret key> \
#   RELEASE_GPG_PASSPHRASE=<passphrase> \
#     tools/ci/release_checksums_sig.sh sign <SHA256SUMS> <SHA256SUMS.asc>
#   tools/ci/release_checksums_sig.sh verify <SHA256SUMS> <SHA256SUMS.asc> <public key file>
#
# sign:   imports the secret into a throwaway GNUPGHOME, refuses unless it
#         holds exactly one secret key whose fingerprint is the release key,
#         writes a detached armored signature, and removes the keyring.
# verify: imports only <public key file> into a fresh GNUPGHOME (the file
#         must hold exactly one key, the release key) and requires the
#         VALIDSIG primary-key fingerprint to be the release key exactly.
#
# RELEASE_KEY_FPR overrides the pinned fingerprint, and is honoured only
# together with RELEASE_SIG_SELF_TEST=1, which only the self-test
# (tools/ci/test_release_checksums_sig.sh) sets. Anywhere else an override is
# refused, so a stray environment variable can never change the pin.
#
# The pin is a consistency check between the signing secret and the
# committed RELEASE-SIGNING-KEY.asc, not an independent trust root: whoever
# can change main can change both.

set -euo pipefail

# The release signing key. A new key changes this line,
# RELEASE-SIGNING-KEY.asc and docs/ci/release.md together.
PINNED_FPR=411DEFDB571351D2B25AEFCC9C02B5BDCCE75313
die() { echo "::error::$*" >&2; exit 1; }

FPR_WANT="$PINNED_FPR"
if [ -n "${RELEASE_KEY_FPR:-}" ]; then
  [ "${RELEASE_SIG_SELF_TEST:-}" = 1 ] \
    || die "RELEASE_KEY_FPR is set outside the self-test; the release signing key is pinned to $PINNED_FPR"
  FPR_WANT="$RELEASE_KEY_FPR"
fi

# Every keyring is a fresh mktemp directory (mode 0700) removed by an EXIT
# trap on success and failure alike, so the private key never outlives the
# command, also on a persistent runner. The workflow adds an always() step
# that removes $RUNNER_TEMP/gnupg.* in case the shell is killed.
new_home() {
  GNUPGHOME="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gnupg.XXXXXX")"
  export GNUPGHOME
  chmod 700 "$GNUPGHOME"
  trap 'gpgconf --kill gpg-agent >/dev/null 2>&1 || true; rm -rf "$GNUPGHOME"' EXIT
}

# Primary-key fingerprints in a key file, one per line (subkeys skipped).
primary_fprs() {
  gpg --batch --with-colons --show-keys "$1" 2>/dev/null \
    | awk -F: '$1=="pub"||$1=="sec"||$1=="sub"||$1=="ssb" {want=($1=="pub"||$1=="sec")} $1=="fpr" && want {print $10; want=0}'
}

cmd_sign() {
  [ $# -eq 2 ] || die "usage: $0 sign <SHA256SUMS> <SHA256SUMS.asc>"
  local sums="$1" out="$2"
  [ -s "$sums" ] || die "$sums is missing or empty"
  if [ -z "${RELEASE_GPG_KEY:-}" ] || [ -z "${RELEASE_GPG_PASSPHRASE:-}" ]; then
    die "RELEASE_GPG_KEY or RELEASE_GPG_PASSPHRASE is not set in the release environment. The repository owner must add the release signing key (fingerprint $FPR_WANT, armored, base64 encoded) and its passphrase as environment secrets of 'release' (docs/ci/release.md). Nothing was signed."
  fi
  new_home
  printf '%s' "$RELEASE_GPG_KEY" | base64 -d | gpg --batch --quiet --import 2>/dev/null \
    || die "RELEASE_GPG_KEY does not import as an OpenPGP key"
  local secs fpr
  secs="$(gpg --batch --list-secret-keys --with-colons | grep -c '^sec:' || true)"
  [ "$secs" = 1 ] || die "the signing secret must hold exactly one secret key, it holds $secs"
  fpr="$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1=="fpr" {print $10; exit}')"
  [ "$fpr" = "$FPR_WANT" ] \
    || die "the signing secret holds key ${fpr:-<none>}, not the release signing key $FPR_WANT. Nothing was signed."
  mkdir -p "$(dirname "$out")"
  printf '%s' "$RELEASE_GPG_PASSPHRASE" | gpg --batch --quiet --yes --pinentry-mode loopback --passphrase-fd 0 \
    --local-user "$fpr" --armor --detach-sign --output "$out" "$sums"
  echo "Signed $sums with key $fpr"
}

# status_count <gpg status output> <keyword> -> number of [GNUPG:] <keyword> lines
status_count() {
  printf '%s\n' "$1" | awk -v t="$2" '$1=="[GNUPG:]" && $2==t {n++} END {print n+0}'
}

cmd_verify() {
  [ $# -eq 3 ] || die "usage: $0 verify <SHA256SUMS> <SHA256SUMS.asc> <public key file>"
  local sums="$1" sig="$2" key="$3" keys status signer
  [ -s "$key" ] || die "the published key file $key is missing or empty"
  [ -s "$sums" ] || die "$sums is missing or empty"
  [ -s "$sig" ] || die "$sig is missing or empty"
  new_home
  keys="$(primary_fprs "$key")"
  [ "$keys" = "$FPR_WANT" ] \
    || die "$key holds [$(printf '%s' "$keys" | tr '\n' ' ')], expected exactly the release signing key $FPR_WANT"
  gpg --batch --quiet --import "$key" 2>/dev/null \
    || die "cannot import $key into the temporary keyring (GNUPGHOME $GNUPGHOME)"
  status="$(gpg --batch --status-fd 1 --verify "$sig" "$sums" 2>/dev/null)" \
    || die "$sig does not verify against the published release signing key"
  # gpg exits 0 and prints VALIDSIG also for a signature by a revoked or
  # expired key (REVKEYSIG / EXPKEYSIG): require exactly one good signature
  # and none of the bad-key or bad-signature states.
  local t n
  for t in REVKEYSIG EXPKEYSIG EXPSIG KEYREVOKED ERRSIG BADSIG; do
    n="$(status_count "$status" "$t")"
    [ "$n" = 0 ] || die "$sig verification reports $t; not a good signature by a valid release key"
  done
  [ "$(status_count "$status" GOODSIG)" = 1 ] && [ "$(status_count "$status" VALIDSIG)" = 1 ] \
    || die "$sig must carry exactly one good signature (GOODSIG $(status_count "$status" GOODSIG), VALIDSIG $(status_count "$status" VALIDSIG))"
  # VALIDSIG field 10 (awk $12) is the primary-key fingerprint; field 1
  # ($3) is the key that made the signature, which may be a subkey.
  signer="$(printf '%s\n' "$status" | awk '$1=="[GNUPG:]" && $2=="VALIDSIG" {print $12}')"
  [ "$signer" = "$FPR_WANT" ] \
    || die "$sig VALIDSIG primary key is [$(printf '%s' "$signer" | tr '\n' ' ')], expected exactly $FPR_WANT"
  echo "Verified $sig against $key ($FPR_WANT)"
}

case "${1:-}" in
  sign) shift; cmd_sign "$@" ;;
  verify) shift; cmd_verify "$@" ;;
  *) die "usage: $0 sign|verify ..." ;;
esac
