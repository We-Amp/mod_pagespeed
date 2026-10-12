#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Signs and verifies rule packs: an Ed25519 detached signature in the
# sidecar <pack>.sig, one line "ed25519 <key_id> <base64 signature>".
#
#   sign-pack.sh keygen <key_id> <private-key.pem>
#   sign-pack.sh pubkey <key_id> <private-key.pem>
#   sign-pack.sh sign   <key_id> <private-key.pem> <pack.json>
#   sign-pack.sh verify <pack.json> <base64 public key>
#   sign-pack.sh --self-test
#
# Needs bash and OpenSSL 3.0 or newer (OPENSSL=/path/to/openssl selects the
# binary). The private key never leaves the file you name; keep it out of
# every repository.
set -euo pipefail

OPENSSL="${OPENSSL:-openssl}"
# SubjectPublicKeyInfo prefix of an Ed25519 public key (12 bytes), then the
# 32 key bytes.
SPKI_PREFIX_HEX="302a300506032b6570032100"

die() {
  local code="${2:-1}"
  echo "sign-pack.sh: $1" >&2
  exit "$code"
}

# Every external tool the script uses must exist: a missing one must never be
# reported as a bad signature.
need_tools() {
  local t
  for t in "$OPENSSL" od tr wc mktemp mv rm cat cp head tail dd sed awk grep dirname; do
    command -v "$t" >/dev/null 2>&1 || die "needs the tool '$t' (not found in PATH)" 2
  done
}

need_openssl3() {
  need_tools
  local v
  v="$("$OPENSSL" version 2>/dev/null || true)"
  case "$v" in
    OpenSSL\ [3-9].* | OpenSSL\ [1-9][0-9].*) return 0 ;;
  esac
  die "needs OpenSSL 3.0 or newer (found: ${v:-no openssl}); set OPENSSL=/path/to/openssl" 2
}

check_key_id() {
  [[ "$1" =~ ^[a-z0-9-]{1,64}$ ]] || die "the key id must match [a-z0-9-]{1,64}"
}

hex_of() { od -An -v -tx1 | tr -d ' \n'; }

# Prints the 32-byte raw public key of a private key file as base64.
public_key_b64() {
  local der
  der="$(mktemp)"
  "$OPENSSL" pkey -in "$1" -pubout -outform DER -out "$der"
  [[ "$(wc -c <"$der" | tr -d ' ')" == 44 ]] || { rm -f "$der"; die "not an Ed25519 key: $1"; }
  [[ "$(head -c 12 "$der" | hex_of)" == "$SPKI_PREFIX_HEX" ]] ||
    { rm -f "$der"; die "not an Ed25519 key: $1"; }
  tail -c 32 "$der" | "$OPENSSL" base64 -A
  rm -f "$der"
}

cmd_pubkey() {
  [[ $# -eq 2 ]] || die "usage: pubkey <key_id> <private-key.pem>"
  check_key_id "$1"
  [[ -f "$2" ]] || die "no such file: $2"
  echo "RulePackTrustKey $1 $(public_key_b64 "$2")"
}

cmd_keygen() {
  [[ $# -eq 2 ]] || die "usage: keygen <key_id> <private-key.pem>"
  check_key_id "$1"
  [[ ! -e "$2" ]] || die "refusing to overwrite $2"
  (umask 077 && "$OPENSSL" genpkey -algorithm ed25519 -out "$2")
  cmd_pubkey "$1" "$2"
}

cmd_sign() {
  [[ $# -eq 3 ]] || die "usage: sign <key_id> <private-key.pem> <pack.json>"
  local key_id="$1" pem="$2" pack="$3"
  check_key_id "$key_id"
  [[ -f "$pem" ]] || die "no such file: $pem"
  [[ -f "$pack" ]] || die "no such file: $pack"
  local tmpdir sigbin out
  tmpdir="$(mktemp -d "$(dirname "$pack")/.sign-pack.XXXXXX")"
  sigbin="$tmpdir/sig.bin"
  out="$tmpdir/sig.txt"
  if ! "$OPENSSL" pkeyutl -sign -inkey "$pem" -rawin -in "$pack" -out "$sigbin"; then
    rm -rf "$tmpdir"
    die "signing failed"
  fi
  [[ "$(wc -c <"$sigbin" | tr -d ' ')" == 64 ]] || { rm -rf "$tmpdir"; die "unexpected signature size"; }
  printf 'ed25519 %s %s\n' "$key_id" "$("$OPENSSL" base64 -A <"$sigbin")" >"$out"
  mv -f "$out" "$pack.sig"
  rm -rf "$tmpdir"
  if ! grep -Eq "\"publisher\"[[:space:]]*:[[:space:]]*\"$key_id\"" "$pack"; then
    echo "sign-pack.sh: warning: $pack does not name \"publisher\": \"$key_id\";" \
      "the module will report a publisher mismatch" >&2
  fi
  echo "wrote $pack.sig"
}

cmd_verify() {
  [[ $# -eq 2 ]] || die "usage: verify <pack.json> <base64 public key>"
  local pack="$1" pub="$2" sig="$1.sig"
  [[ -f "$pack" ]] || die "no such file: $pack"
  local re='^ed25519 ([a-z0-9-]{1,64}) ([A-Za-z0-9+/]{86}==)$'
  local line=""
  if [[ -f "$sig" ]] && [[ "$(wc -c <"$sig" | tr -d ' ')" -le 1024 ]]; then
    line="$(cat "$sig")"  # command substitution drops trailing newlines
    # The module accepts at most one trailing newline.
    [[ "$(tail -c 2 "$sig" | hex_of)" != "0a0a" ]] || line=""
  fi
  if [[ ! "$line" =~ $re ]]; then
    echo "signature unreadable"
    return 1
  fi
  local key_id="${BASH_REMATCH[1]}" sig_b64="${BASH_REMATCH[2]}"
  [[ "$pub" =~ ^[A-Za-z0-9+/]{43}=$ ]] || { echo "public key must be 44 base64 characters" >&2; return 2; }
  local tmpdir
  tmpdir="$(mktemp -d)"
  printf '%s' "$sig_b64" | "$OPENSSL" base64 -d -A >"$tmpdir/sig.bin"
  # The 12-byte Ed25519 SubjectPublicKeyInfo prefix (octal), then the key.
  { printf '\060\052\060\005\006\003\053\145\160\003\041\000'; printf '%s' "$pub" | "$OPENSSL" base64 -d -A; } >"$tmpdir/pub.der"
  local result
  result="$("$OPENSSL" pkeyutl -verify -pubin -keyform DER -inkey "$tmpdir/pub.der" \
    -rawin -in "$pack" -sigfile "$tmpdir/sig.bin" 2>&1 || true)"
  rm -rf "$tmpdir"
  case "$result" in
    *"Signature Verified Successfully"*)
      echo "verified: key=$key_id"
      return 0
      ;;
    *"Signature Verification Failure"*)
      echo "bad signature"
      return 1
      ;;
  esac
  die "openssl failed: $result" 2
}

dir=""
cmd_self_test() {
  dir="$(mktemp -d)"
  trap 'rm -rf "$dir"' EXIT
  local pack="$dir/pack.json" pub other_pub out
  cat >"$pack" <<'JSON'
{"pack":{"id":"self-test","version":"0.0.1","publisher":"self-test-key"},"sites":[{"host":"example.com","mode":"report"}],"rules":[{"id":"t","kind":"title","enforce":true,"value":{"template":"T"}}]}
JSON
  pub="$(cmd_keygen self-test-key "$dir/a.pem" | awk '{print $3}')"
  cmd_sign self-test-key "$dir/a.pem" "$pack" >/dev/null
  out="$(cmd_verify "$pack" "$pub")" || die "self-test: a fresh signature did not verify"
  [[ "$out" == "verified: key=self-test-key" ]] || die "self-test: unexpected output: $out"
  echo "PASS: keygen, sign and verify a pack with a throwaway key"

  [[ "$(cat "$pack.sig")" =~ ^ed25519\ [a-z0-9-]{1,64}\ [A-Za-z0-9+/]{86}==$ ]] ||
    die "self-test: the sidecar does not match the format"
  echo "PASS: the sidecar matches the format"

  cp "$pack" "$dir/t.json"
  cp "$pack.sig" "$dir/t.json.sig"
  printf 'X' | dd of="$dir/t.json" bs=1 seek=10 conv=notrunc 2>/dev/null
  out="$(cmd_verify "$dir/t.json" "$pub" || true)"
  [[ "$out" == "bad signature" ]] || die "self-test: a tampered pack was not refused ($out)"
  echo "PASS: one flipped byte of the pack is a bad signature"

  other_pub="$(cmd_keygen other-key "$dir/b.pem" | awk '{print $3}')"
  out="$(cmd_verify "$pack" "$other_pub" || true)"
  [[ "$out" == "bad signature" ]] || die "self-test: a second key verified the pack ($out)"
  echo "PASS: a second key is a bad signature"

  sed 's/^ed25519 /rsa /' "$pack.sig" >"$dir/r.json.sig"
  cp "$pack" "$dir/r.json"
  out="$(cmd_verify "$dir/r.json" "$pub" || true)"
  [[ "$out" == "signature unreadable" ]] || die "self-test: a wrong tag was not unreadable ($out)"
  echo "PASS: a wrong tag is signature unreadable"

  echo "sign-pack self-test: all 5 cases passed"
}

main() {
  [[ $# -ge 1 ]] || die "usage: sign-pack.sh keygen|pubkey|sign|verify|--self-test ..." 2
  local sub="$1"
  shift
  case "$sub" in
    keygen | pubkey | sign | verify | --self-test) need_openssl3 ;;
  esac
  case "$sub" in
    keygen) cmd_keygen "$@" ;;
    pubkey) cmd_pubkey "$@" ;;
    sign) cmd_sign "$@" ;;
    verify) cmd_verify "$@" ;;
    --self-test) cmd_self_test ;;
    *) die "unknown command: $sub" 2 ;;
  esac
}

main "$@"
