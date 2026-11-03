#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Helpers of the nginx.org rebuild lane (.github/workflows/nginx-org-rebuild.yml,
# docs/ci/release.md, "The nginx.org rebuild lane"). Each subcommand checks one
# thing and fails closed; the workflow chains them.
#
#   nginx_org_lane.sh inputs
#       Reads MODULE_TAG, NGINX_VERSION, NGINX_SHA256, LANE_REVISION and the
#       optional PREVIOUS_NGINX_VERSION / PREVIOUS_NGINX_SHA256 from the
#       environment, checks their grammar and prints key=value lines:
#       version, lane_tag, lane_revision, revision_fragment.
#   nginx_org_lane.sh verify-sig KEYFILE "FPR..." SIGNATURE DATA
#       A detached signature of DATA by one of the keys in KEYFILE. KEYFILE must
#       hold exactly the primary keys FPR... (no more, no fewer); the signature
#       must be one good signature by one of them, by a key that is neither
#       revoked nor expired. Prints the signer's primary fingerprint.
#   nginx_org_lane.sh verify-clear KEYFILE "FPR..." SIGNED OUT
#       The same for a clearsigned file (an apt InRelease); writes the signed
#       text to OUT.
#   nginx_org_lane.sh verify-release DIR TAG
#       DIR holds a module release's manifest.json, SHA256SUMS (whose signature
#       verify-sig has checked) and its source tarball. Checks the rows of both,
#       that the manifest names TAG, is not a pre-release and lists exactly one
#       source tarball, and prints version, source_sha, source_asset and
#       optimizer_tag as key=value lines.
#   nginx_org_lane.sh index-sha INRELEASE PATH
#       The SHA256 that a verified InRelease text lists for PATH.
#   nginx_org_lane.sh previous PACKAGES
#       The newest nginx-module-pagespeed-nginxorg in an apt Packages index, by
#       dpkg --compare-versions: prints version, nginx, filename and sha256 as
#       key=value lines, or nothing when the index has none.
#   nginx_org_lane.sh repomd-primary REPOMD
#       The sha256 and the location of the primary index that a yum repomd.xml
#       (whose detached signature verify-sig has checked) lists, as one line
#       "<sha256> <href>".
#   nginx_org_lane.sh previous-rpm PRIMARY_XML_GZ
#       The newest nginx-module-pagespeed-nginxorg in a yum primary index, by
#       rpm's version order over the lane's version grammar: prints version,
#       nginx, filename and sha256 as key=value lines, or nothing when the
#       index has none.
#   nginx_org_lane.sh new-line PUBLISHED_NGINX NEW_NGINX
#       Refuses an nginx on another stable line than the published one (a new
#       line is a change of the leg table) and an nginx older than the published one.
#   nginx_org_lane.sh pins
#       The key fingerprints the lane checks against, as KEY=value lines for
#       GITHUB_ENV: RELEASE_KEY_FPR (RELEASE-SIGNING-KEY.asc), REPO_KEY_FPR
#       (the package repository's key, as install.sh pins it) and NGINX_SIGNERS
#       (the keys in nginx-org-source-signers.asc). They live here, on master,
#       next to the key files, because the workflow runs from a release tag.
#   nginx_org_lane.sh --self-test
set -euo pipefail

RELEASE_KEY_FPR=411DEFDB571351D2B25AEFCC9C02B5BDCCE75313
REPO_KEY_FPR=DF00E296BE9141CEA5411346F50D6054F10712A0
NGINX_SIGNERS="D6786CE303D9A9022998DC6CC8464D549AF75C0A 43387825DDB1BB97EC36BA5D007C8D7C15D87369"

pins() {
  echo "RELEASE_KEY_FPR=${RELEASE_KEY_FPR}"
  echo "REPO_KEY_FPR=${REPO_KEY_FPR}"
  echo "NGINX_SIGNERS=${NGINX_SIGNERS}"
}

die() { echo "::error::nginx_org_lane: $*" >&2; exit 1; }

SEMVER_RE='^[0-9]+\.[0-9]+\.[0-9]+$'

inputs() {
  local tag="${MODULE_TAG:-}" ngx="${NGINX_VERSION:-}" sha="${NGINX_SHA256:-}" rev="${LANE_REVISION:-0}"
  local pngx="${PREVIOUS_NGINX_VERSION:-}" psha="${PREVIOUS_NGINX_SHA256:-}" version
  [[ "$tag" =~ ^module-v([0-9]+\.[0-9]+\.[0-9]+)$ ]] || die "module_tag must be module-vX.Y.Z (got '$tag')"
  version="${BASH_REMATCH[1]}"
  [[ "$ngx" =~ $SEMVER_RE ]] || die "nginx_version must be X.Y.Z (got '$ngx')"
  [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || die "nginx_sha256 must be 64 lower-case hex digits"
  [[ "$rev" =~ ^(0|[1-9][0-9]?)$ ]] || die "lane_revision must be 0 to 99 (got '$rev')"
  if [ -n "$pngx" ] || [ -n "$psha" ]; then
    [[ "$pngx" =~ $SEMVER_RE ]] || die "previous_nginx_version must be X.Y.Z (got '$pngx')"
    [[ "$psha" =~ ^[0-9a-f]{64}$ ]] || die "previous_nginx_sha256 must be 64 lower-case hex digits"
  fi
  echo "version=${version}"
  if [ "$rev" = 0 ]; then
    echo "lane_tag=nginxorg-v${version}-ngx${ngx}"
    echo "revision_fragment=+ngx${ngx}~"
  else
    echo "lane_tag=nginxorg-v${version}-ngx${ngx}-r${rev}"
    echo "revision_fragment=+ngx${ngx}.${rev}~"
  fi
  echo "lane_revision=${rev}"
}

# _gpg_home: a fresh keyring directory under RUNNER_TEMP or TMPDIR; the caller
# removes it.
_gpg_home() {
  local h
  h="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/lane-gpg.XXXXXX")"
  chmod 700 "$h"
  printf '%s\n' "$h"
}

# _check_keyfile HOME KEYFILE "FPR...": the file holds exactly these primary keys.
_check_keyfile() {
  local home="$1" keyfile="$2" want got
  [ -s "$keyfile" ] || die "key file $keyfile is missing or empty"
  got="$(GNUPGHOME="$home" gpg --batch --with-colons --show-keys "$keyfile" 2>/dev/null \
    | awk -F: '$1=="pub" {want=1} $1=="sub" {want=0} $1=="fpr" && want {print $10; want=0}' | LC_ALL=C sort)"
  want="$(printf '%s\n' $3 | LC_ALL=C sort)"
  [ "$got" = "$want" ] || die "key file $keyfile holds [$(echo $got)], expected exactly [$(echo $want)]"
  GNUPGHOME="$home" gpg --batch --quiet --import "$keyfile" 2>/dev/null || die "could not import $keyfile"
}

# _judge STATUS "FPR...": one good signature by a valid listed key; prints it.
_judge() {
  local status="$1" fprs=" $2 " t signer
  count() { printf '%s\n' "$status" | awk -v t="$1" '$1=="[GNUPG:]" && $2==t {n++} END {print n+0}'; }
  for t in REVKEYSIG EXPKEYSIG EXPSIG KEYREVOKED ERRSIG BADSIG NO_PUBKEY; do
    [ "$(count "$t")" = 0 ] || die "signature check reports $t"
  done
  [ "$(count GOODSIG)" = 1 ] && [ "$(count VALIDSIG)" = 1 ] \
    || die "expected exactly one good signature (GOODSIG $(count GOODSIG), VALIDSIG $(count VALIDSIG))"
  # VALIDSIG field 10 (awk $12) is the primary-key fingerprint.
  signer="$(printf '%s\n' "$status" | awk '$1=="[GNUPG:]" && $2=="VALIDSIG" {print $12}')"
  case "$fprs" in *" $signer "*) ;; *) die "signed by $signer, which is not a pinned key" ;; esac
  printf '%s\n' "$signer"
}

# The keyring of a signature check and, for verify-clear, its output: globals,
# so that the EXIT trap removes them also when die ends the script mid-check.
LANE_GPG_HOME=""
LANE_CLEAR_OUT=""
_cleanup() {
  if [ -n "$LANE_GPG_HOME" ]; then
    GNUPGHOME="$LANE_GPG_HOME" gpgconf --kill gpg-agent >/dev/null 2>&1 || true
    rm -rf "$LANE_GPG_HOME"
    LANE_GPG_HOME=""
  fi
  if [ -n "$LANE_CLEAR_OUT" ]; then rm -f "$LANE_CLEAR_OUT"; LANE_CLEAR_OUT=""; fi
}

verify_sig() {
  [ $# = 4 ] || die "usage: verify-sig KEYFILE \"FPR...\" SIGNATURE DATA"
  local status
  [ -s "$3" ] && [ -f "$4" ] || die "signature or data file missing"
  trap _cleanup EXIT
  LANE_GPG_HOME="$(_gpg_home)"
  _check_keyfile "$LANE_GPG_HOME" "$1" "$2"
  status="$(GNUPGHOME="$LANE_GPG_HOME" gpg --batch --status-fd 1 --verify "$3" "$4" 2>/dev/null)" \
    || die "the signature of $4 does not verify against the pinned keys"
  _judge "$status" "$2"
  _cleanup
}

verify_clear() {
  [ $# = 4 ] || die "usage: verify-clear KEYFILE \"FPR...\" SIGNED OUT"
  local status
  [ -s "$3" ] || die "signed file missing"
  trap _cleanup EXIT
  LANE_GPG_HOME="$(_gpg_home)"
  LANE_CLEAR_OUT="$4"
  rm -f "$4"
  _check_keyfile "$LANE_GPG_HOME" "$1" "$2"
  status="$(GNUPGHOME="$LANE_GPG_HOME" gpg --batch --status-fd 1 --output "$4" --decrypt "$3" 2>/dev/null)" \
    || die "$3 does not verify against the pinned keys"
  _judge "$status" "$2" >/dev/null
  # Verified: keep the text.
  LANE_CLEAR_OUT=""
  _cleanup
}

# _sums_row SUMS NAME: the sha256 SUMS lists for NAME (exactly one row).
_sums_row() {
  local rows
  rows="$(awk -v n="$2" 'length($1) == 64 && $2 == n {print $1}' "$1")"
  [ -n "$rows" ] && [ "$(printf '%s\n' "$rows" | wc -l | tr -d ' ')" = 1 ] || die "SHA256SUMS has no single row for $2"
  printf '%s\n' "$rows"
}

verify_release() {
  [ $# = 2 ] || die "usage: verify-release DIR TAG"
  local dir="$1" tag="$2" m version sha src want got
  m="$dir/manifest.json"
  [ -f "$m" ] && [ -f "$dir/SHA256SUMS" ] || die "manifest.json or SHA256SUMS missing in $dir"
  [ "$(sha256sum "$m" | cut -d' ' -f1)" = "$(_sums_row "$dir/SHA256SUMS" manifest.json)" ] \
    || die "manifest.json does not match SHA256SUMS"
  [ "$(jq -r '.schema' "$m")" = 1 ] || die "unknown manifest schema"
  [ "$(jq -r '.tag' "$m")" = "$tag" ] || die "the manifest names $(jq -r '.tag' "$m"), not $tag"
  [ "$(jq -r '.prerelease' "$m")" = false ] || die "$tag is a pre-release; the lane builds only from a release"
  version="$(jq -r '.version' "$m")"
  [ "module-v${version}" = "$tag" ] || die "the manifest version $version does not match $tag"
  sha="$(jq -r '.source_sha' "$m")"
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || die "the manifest's source_sha is not a 40 hex digit sha"
  [ "$(jq '[.files[] | select(.component == "source")] | length' "$m")" = 1 ] \
    || die "the manifest must list exactly one source file"
  src="$(jq -r '.files[] | select(.component == "source") | .asset' "$m")"
  [ "$src" = "mod_pagespeed-${version}.tar.gz" ] || die "unexpected source asset name '$src'"
  [ -f "$dir/$src" ] || die "the source tarball $src is not in $dir"
  want="$(_sums_row "$dir/SHA256SUMS" "$src")"
  [ "$(jq -r --arg a "$src" '.files[] | select(.asset == $a) | .sha256' "$m")" = "$want" ] \
    || die "the manifest and SHA256SUMS disagree on $src"
  got="$(sha256sum "$dir/$src" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || die "$src does not match SHA256SUMS"
  echo "version=${version}"
  echo "source_sha=${sha}"
  echo "source_asset=${src}"
  echo "optimizer_tag=$(jq -r '.optimizer_tag // ""' "$m")"
}

index_sha() {
  [ $# = 2 ] || die "usage: index-sha INRELEASE PATH"
  local rows
  # The SHA256 section: indented "<64 hex> <size> <path>" rows after "SHA256:".
  rows="$(awk -v p="$2" '/^[A-Za-z0-9-]+:/ {sec=$1} sec=="SHA256:" && NF==3 && length($1)==64 && $3==p {print $1}' "$1")"
  [ -n "$rows" ] && [ "$(printf '%s\n' "$rows" | wc -l | tr -d ' ')" = 1 ] || die "the index lists no single SHA256 for $2"
  printf '%s\n' "$rows"
}

previous() {
  [ $# = 1 ] || die "usage: previous PACKAGES"
  local best="" bestf="" bests="" pkg="" ver="" fil="" sha="" line ngx
  pick() {
    [ "$pkg" = nginx-module-pagespeed-nginxorg ] && [ -n "$ver" ] || return 0
    [ -n "$fil" ] && [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || die "the index entry for $ver lacks Filename or SHA256"
    if [ -z "$best" ] || dpkg --compare-versions "$ver" gt "$best"; then best="$ver"; bestf="$fil"; bests="$sha"; fi
  }
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      "") pick; pkg=""; ver=""; fil=""; sha="" ;;
      "Package: "*) pkg="${line#Package: }" ;;
      "Version: "*) ver="${line#Version: }" ;;
      "Filename: "*) fil="${line#Filename: }" ;;
      "SHA256: "*) sha="${line#SHA256: }" ;;
    esac
  done < "$1"
  pick
  [ -n "$best" ] || return 0
  [[ "$best" =~ \+ngx([0-9]+\.[0-9]+\.[0-9]+)(\.[0-9]+)?~ ]] || die "the published version $best does not name an nginx"
  ngx="${BASH_REMATCH[1]}"
  case "$bestf" in pool/*) ;; *) die "unexpected Filename $bestf" ;; esac
  [[ "$bestf" =~ ^[A-Za-z0-9/._+~-]+$ ]] || die "unexpected characters in Filename $bestf"
  echo "version=${best}"
  echo "nginx=${ngx}"
  echo "filename=${bestf}"
  echo "sha256=${bests}"
}

repomd_primary() {
  [ $# = 1 ] || die "usage: repomd-primary REPOMD"
  [ -s "$1" ] || die "repomd.xml missing or empty"
  python3 -I - "$1" <<'PY' || die "repomd.xml lists no single sha256 primary index"
import re, sys
import xml.etree.ElementTree as ET
ns = {"r": "http://linux.duke.edu/metadata/repo"}
root = ET.parse(sys.argv[1]).getroot()
rows = root.findall("r:data[@type='primary']", ns)
if len(rows) != 1:
    sys.exit(1)
c = rows[0].find("r:checksum", ns)
loc = rows[0].find("r:location", ns)
if c is None or loc is None or c.get("type") != "sha256":
    sys.exit(1)
sha, href = (c.text or "").strip(), loc.get("href") or ""
if not re.fullmatch(r"[0-9a-f]{64}", sha) or not re.fullmatch(r"repodata/[A-Za-z0-9._-]+\.xml\.gz", href):
    sys.exit(1)
print(sha, href)
PY
}

previous_rpm() {
  [ $# = 1 ] || die "usage: previous-rpm PRIMARY_XML_GZ"
  [ -s "$1" ] || die "primary index missing or empty"
  python3 -I - "$1" <<'PY' || die "the yum primary index has a malformed nginx-module-pagespeed-nginxorg entry"
import gzip, re, sys
import xml.etree.ElementTree as ET
ns = {"c": "http://linux.duke.edu/metadata/common"}
root = ET.parse(gzip.open(sys.argv[1])).getroot()
VER = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
# <n>.ngx<nginx>[.<revision>].el9: rpm compares the segments in this order,
# and a revision (a number) sorts above the "el9" that follows a first build.
REL = re.compile(r"([0-9]+)\.ngx([0-9]+\.[0-9]+\.[0-9]+)(?:\.([0-9]+))?\.el9")
best = None
for p in root.findall("c:package", ns):
    if (p.findtext("c:name", "", ns) or "") != "nginx-module-pagespeed-nginxorg":
        continue
    if (p.findtext("c:arch", "", ns) or "") != "x86_64":
        sys.exit(1)
    v = p.find("c:version", ns)
    c = p.find("c:checksum", ns)
    loc = p.find("c:location", ns)
    if v is None or c is None or loc is None:
        sys.exit(1)
    ver, rel, epoch = v.get("ver") or "", v.get("rel") or "", v.get("epoch") or "0"
    m = REL.fullmatch(rel)
    if epoch != "0" or not VER.fullmatch(ver) or not m:
        sys.exit(1)
    sha, href = (c.text or "").strip(), loc.get("href") or ""
    if c.get("type") != "sha256" or not re.fullmatch(r"[0-9a-f]{64}", sha):
        sys.exit(1)
    if not re.fullmatch(r"[A-Za-z0-9._+-]+(/[A-Za-z0-9._+-]+)*\.rpm", href) or ".." in href.split("/"):
        sys.exit(1)
    key = (tuple(int(x) for x in ver.split(".")), int(m.group(1)),
           tuple(int(x) for x in m.group(2).split(".")), int(m.group(3)) if m.group(3) else -1)
    if best is None or key > best[0]:
        best = (key, "%s-%s" % (ver, rel), m.group(2), href, sha)
if best:
    print("version=%s\nnginx=%s\nfilename=%s\nsha256=%s" % best[1:])
PY
}

new_line() {
  [ $# = 2 ] || die "usage: new-line PUBLISHED_NGINX NEW_NGINX"
  [[ "$1" =~ $SEMVER_RE ]] && [[ "$2" =~ $SEMVER_RE ]] || die "versions must be X.Y.Z"
  [ "${1%.*}" = "${2%.*}" ] || die "nginx $2 is on another stable line than the published $1: a new line is a change of the leg table, not a dispatch"
  [ "${2##*.}" -ge "${1##*.}" ] || die "nginx $2 is older than the published $1"
  echo "nginx $2 is on the published line ${1%.*}"
}

self_test() {
  local fail=0 t out
  t="$(mktemp -d)"
  trap 'rm -rf "$t"' RETURN
  ok() { echo "ok   $1"; }
  bad() { echo "FAIL $1"; fail=1; }
  expect() { local name="$1"; shift; if ( "$@" ) >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi; }
  refuse() { local name="$1"; shift; if ( "$@" ) >/dev/null 2>&1; then bad "$name (accepted)"; else ok "$name"; fi; }

  # inputs
  local S64; S64="$(printf 'a%.0s' $(seq 64))"
  out="$(MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" inputs)"
  [ "$out" = "$(printf 'version=2.8.0\nlane_tag=nginxorg-v2.8.0-ngx1.30.7\nrevision_fragment=+ngx1.30.7~\nlane_revision=0')" ] \
    && ok "inputs: first build" || bad "inputs: first build: $out"
  out="$(MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" LANE_REVISION=2 inputs)"
  case "$out" in *"lane_tag=nginxorg-v2.8.0-ngx1.30.7-r2"*"revision_fragment=+ngx1.30.7.2~"*) ok "inputs: revision" ;; *) bad "inputs: revision: $out" ;; esac
  refuse "inputs: rc tag" env MODULE_TAG=module-v2.8.0-rc.1 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" bash "$0" inputs
  refuse "inputs: other tag" env MODULE_TAG=nginxorg-v2.8.0-ngx1.30.7 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" bash "$0" inputs
  refuse "inputs: injection in nginx_version" env MODULE_TAG=module-v2.8.0 NGINX_VERSION='1.30.7;id' NGINX_SHA256="$S64" bash "$0" inputs
  refuse "inputs: short sha" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256=abc bash "$0" inputs
  refuse "inputs: upper-case sha" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$(printf 'A%.0s' $(seq 64))" bash "$0" inputs
  refuse "inputs: revision 100" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" LANE_REVISION=100 bash "$0" inputs
  refuse "inputs: revision 01" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" LANE_REVISION=01 bash "$0" inputs
  refuse "inputs: previous version without its sha" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" PREVIOUS_NGINX_VERSION=1.30.6 bash "$0" inputs
  expect "inputs: previous pair" env MODULE_TAG=module-v2.8.0 NGINX_VERSION=1.30.7 NGINX_SHA256="$S64" PREVIOUS_NGINX_VERSION=1.30.6 PREVIOUS_NGINX_SHA256="$S64" bash "$0" inputs

  # pins: the pinned signer set is exactly the keys in the key file next to this script
  local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  if command -v gpg >/dev/null 2>&1 && [ -f "$here/nginx-org-source-signers.asc" ]; then
    local got
    got="$(gpg --batch --with-colons --show-keys "$here/nginx-org-source-signers.asc" 2>/dev/null \
      | awk -F: '$1=="pub" {w=1} $1=="sub" {w=0} $1=="fpr" && w {print $10; w=0}' | LC_ALL=C sort | tr '\n' ' ')"
    [ "$got" = "$(printf '%s\n' $NGINX_SIGNERS | LC_ALL=C sort | tr '\n' ' ')" ] \
      && ok "pins: NGINX_SIGNERS matches nginx-org-source-signers.asc" || bad "pins: NGINX_SIGNERS [$NGINX_SIGNERS] vs the key file [$got]"
  fi
  [ "$(pins | wc -l | tr -d ' ')" = 3 ] && ok "pins: three lines" || bad "pins"

  # new-line
  expect "new-line: next patch" new_line 1.30.6 1.30.7
  expect "new-line: same nginx (a module release or a revision)" new_line 1.30.6 1.30.6
  refuse "new-line: next stable line" new_line 1.30.6 1.32.0
  refuse "new-line: older patch" new_line 1.30.6 1.30.5

  # previous: highest by dpkg, revision above its first build, below the next nginx
  if command -v dpkg >/dev/null 2>&1; then
    cat > "$t/Packages" <<EOF
Package: nginx-module-pagespeed
Version: 9.9.9-r0~trixie
Filename: pool/main/n/nginx-module-pagespeed/a.deb
SHA256: $(printf 'b%.0s' $(seq 64))

Package: nginx-module-pagespeed-nginxorg
Version: 2.8.0-r0+ngx1.30.7~trixie
Filename: pool/main/n/nginx-module-pagespeed-nginxorg/x1.deb
SHA256: $(printf 'c%.0s' $(seq 64))

Package: nginx-module-pagespeed-nginxorg
Version: 2.8.0-r0+ngx1.30.7.1~trixie
Filename: pool/main/n/nginx-module-pagespeed-nginxorg/x2.deb
SHA256: $(printf 'd%.0s' $(seq 64))

Package: nginx-module-pagespeed-nginxorg
Version: 2.7.0-r0+ngx1.30.6~trixie
Filename: pool/main/n/nginx-module-pagespeed-nginxorg/x0.deb
SHA256: $(printf 'e%.0s' $(seq 64))
EOF
    out="$(previous "$t/Packages")"
    case "$out" in *"version=2.8.0-r0+ngx1.30.7.1~trixie"*"nginx=1.30.7"*"x2.deb"*) ok "previous: the revision is the newest" ;; *) bad "previous: $out" ;; esac
    printf '\nPackage: nginx-module-pagespeed-nginxorg\nVersion: 2.8.0-r0+ngx1.30.8~trixie\nFilename: pool/main/n/nginx-module-pagespeed-nginxorg/x3.deb\nSHA256: %s\n' "$(printf 'f%.0s' $(seq 64))" >> "$t/Packages"
    out="$(previous "$t/Packages")"
    case "$out" in *"nginx=1.30.8"*) ok "previous: the next nginx outranks a revision" ;; *) bad "previous: next nginx: $out" ;; esac
    printf 'Package: mod-pagespeed\nVersion: 1\nFilename: pool/a.deb\nSHA256: %s\n' "$S64" > "$t/none"
    [ -z "$(previous "$t/none")" ] && ok "previous: none published" || bad "previous: none published"
    printf 'Package: nginx-module-pagespeed-nginxorg\nVersion: 2.8.0-r0+ngx1.30.7~trixie\nFilename: ../../etc/passwd\nSHA256: %s\n' "$S64" > "$t/evil"
    refuse "previous: a Filename outside the pool" previous "$t/evil"
    printf 'Package: nginx-module-pagespeed-nginxorg\nVersion: 2.8.0-r0+ngx1.30.7~trixie\nFilename: pool/x.deb\n' > "$t/nosha"
    refuse "previous: an entry without SHA256" previous "$t/nosha"
  else
    echo "skip previous: no dpkg on this host"
  fi

  # repomd-primary and previous-rpm
  if command -v python3 >/dev/null 2>&1; then
    local S1 S2 S3 S4; S1="$(printf '1%.0s' $(seq 64))"; S2="$(printf '2%.0s' $(seq 64))"
    S3="$(printf '3%.0s' $(seq 64))"; S4="$(printf '4%.0s' $(seq 64))"
    cat > "$t/repomd.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<repomd xmlns="http://linux.duke.edu/metadata/repo" xmlns:rpm="http://linux.duke.edu/metadata/rpm">
  <data type="primary"><checksum type="sha256">$S1</checksum><location href="repodata/primary.xml.gz"/></data>
  <data type="filelists"><checksum type="sha256">$S2</checksum><location href="repodata/filelists.xml.gz"/></data>
</repomd>
EOF
    [ "$(repomd_primary "$t/repomd.xml")" = "$S1 repodata/primary.xml.gz" ] && ok "repomd-primary: the primary row" || bad "repomd-primary"
    sed 's#repodata/primary.xml.gz#../primary.xml.gz#' "$t/repomd.xml" > "$t/repomd.bad"
    refuse "repomd-primary: a location outside repodata" repomd_primary "$t/repomd.bad"
    sed 's#type="sha256">'"$S1"'#type="sha1">'"$S1"'#' "$t/repomd.xml" > "$t/repomd.sha1"
    refuse "repomd-primary: a primary without sha256" repomd_primary "$t/repomd.sha1"
    rpmpkg() { # name ver rel sha href [epoch]
      printf '<package type="rpm"><name>%s</name><arch>x86_64</arch><version epoch="%s" ver="%s" rel="%s"/><checksum type="sha256" pkgid="YES">%s</checksum><location href="%s"/></package>\n' \
        "$1" "${6:-0}" "$2" "$3" "$4" "$5"
    }
    primary() { # OUT, package lines on stdin
      { echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<metadata xmlns="http://linux.duke.edu/metadata/common" xmlns:rpm="http://linux.duke.edu/metadata/rpm" packages="0">'
        cat; echo '</metadata>'; } | gzip -c > "$1"
    }
    {
      rpmpkg nginx-module-pagespeed 9.9.9 1.el9 "$S1" Packages/a.rpm
      rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.el9 "$S2" Packages/x1.rpm
      rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.1.el9 "$S3" Packages/x2.rpm
      rpmpkg nginx-module-pagespeed-nginxorg 2.7.0 0.ngx1.30.6.el9 "$S4" Packages/x0.rpm
    } > "$t/p1.lines"
    primary "$t/p1.xml.gz" < "$t/p1.lines"
    out="$(previous_rpm "$t/p1.xml.gz")"
    [ "$out" = "$(printf 'version=2.8.0-0.ngx1.30.7.1.el9\nnginx=1.30.7\nfilename=Packages/x2.rpm\nsha256=%s' "$S3")" ] \
      && ok "previous-rpm: the revision is the newest" || bad "previous-rpm: $out"
    { cat "$t/p1.lines"; rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.8.el9 "$S1" Packages/x3.rpm; } | primary "$t/p2.xml.gz"
    case "$(previous_rpm "$t/p2.xml.gz")" in *"nginx=1.30.8"*"x3.rpm"*) ok "previous-rpm: the next nginx outranks a revision" ;; *) bad "previous-rpm: next nginx" ;; esac
    { cat "$t/p1.lines"; rpmpkg nginx-module-pagespeed-nginxorg 2.8.1 0.ngx1.30.6.el9 "$S1" Packages/x4.rpm; } | primary "$t/p3.xml.gz"
    case "$(previous_rpm "$t/p3.xml.gz")" in *"version=2.8.1-0.ngx1.30.6.el9"*) ok "previous-rpm: a module release outranks a newer nginx" ;; *) bad "previous-rpm: module release" ;; esac
    rpmpkg nginx-module-pagespeed 9.9.9 1.el9 "$S1" Packages/a.rpm | primary "$t/p0.xml.gz"
    [ -z "$(previous_rpm "$t/p0.xml.gz")" ] && ok "previous-rpm: none published" || bad "previous-rpm: none published"
    rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.el9 "$S1" ../../etc/x.rpm | primary "$t/pe.xml.gz"
    refuse "previous-rpm: a location outside the tree" previous_rpm "$t/pe.xml.gz"
    rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 1.el9 "$S1" Packages/x.rpm | primary "$t/pn.xml.gz"
    refuse "previous-rpm: a release that names no nginx" previous_rpm "$t/pn.xml.gz"
    rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.el9 "$S1" Packages/x.rpm 1 | primary "$t/pp.xml.gz"
    refuse "previous-rpm: an epoch" previous_rpm "$t/pp.xml.gz"
    rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.el9 abc Packages/x.rpm | primary "$t/ps.xml.gz"
    refuse "previous-rpm: an entry without a sha256" previous_rpm "$t/ps.xml.gz"
    rpmpkg nginx-module-pagespeed-nginxorg 2.8.0 0.ngx1.30.7.el9 "$S1" Packages/x.rpm | sed 's#<arch>x86_64</arch>#<arch>src</arch>#' | primary "$t/pa.xml.gz"
    refuse "previous-rpm: an entry for another arch" previous_rpm "$t/pa.xml.gz"
  else
    echo "skip previous-rpm: no python3 on this host"
  fi

  # index-sha
  cat > "$t/InRelease.txt" <<EOF
Origin: test
MD5Sum:
 c0ef6d5fa29c69e8cabd95dfd30d5e97    57130 main/binary-amd64/Packages
SHA256:
 $(printf '1%.0s' $(seq 64))    57130 main/binary-amd64/Packages
 $(printf '2%.0s' $(seq 64))    1000 main/binary-arm64/Packages
EOF
  [ "$(index_sha "$t/InRelease.txt" main/binary-amd64/Packages)" = "$(printf '1%.0s' $(seq 64))" ] \
    && ok "index-sha: the SHA256 row" || bad "index-sha"
  refuse "index-sha: an unlisted path" index_sha "$t/InRelease.txt" main/binary-i386/Packages

  # signatures and the release set, with throwaway keys
  if command -v gpg >/dev/null 2>&1; then
    local gh fa fb
    gh="$t/gnupg"; mkdir -p "$gh"; chmod 700 "$gh"
    for n in a b; do
      GNUPGHOME="$gh" gpg --batch --passphrase '' --quick-gen-key "lane-test-$n <$n@example.invalid>" ed25519 sign never >/dev/null 2>&1
    done
    fa="$(GNUPGHOME="$gh" gpg --batch --with-colons --list-keys lane-test-a 2>/dev/null | awk -F: '$1=="fpr" {print $10; exit}')"
    fb="$(GNUPGHOME="$gh" gpg --batch --with-colons --list-keys lane-test-b 2>/dev/null | awk -F: '$1=="fpr" {print $10; exit}')"
    GNUPGHOME="$gh" gpg --batch --armor --export "$fa" > "$t/a.asc" 2>/dev/null
    GNUPGHOME="$gh" gpg --batch --armor --export "$fa" "$fb" > "$t/ab.asc" 2>/dev/null
    echo data > "$t/data"
    GNUPGHOME="$gh" gpg --batch --yes --local-user "$fa" --armor --detach-sign --output "$t/data.asc" "$t/data" 2>/dev/null
    GNUPGHOME="$gh" gpg --batch --yes --local-user "$fb" --armor --detach-sign --output "$t/data.b.asc" "$t/data" 2>/dev/null
    out="$(verify_sig "$t/a.asc" "$fa" "$t/data.asc" "$t/data" 2>/dev/null)" && [ "$out" = "$fa" ] \
      && ok "verify-sig: a good signature" || bad "verify-sig: a good signature"
    expect "verify-sig: one of two pinned keys" verify_sig "$t/ab.asc" "$fa $fb" "$t/data.b.asc" "$t/data"
    refuse "verify-sig: a key file with an extra key" verify_sig "$t/ab.asc" "$fa" "$t/data.asc" "$t/data"
    refuse "verify-sig: a key file missing a pinned key" verify_sig "$t/a.asc" "$fa $fb" "$t/data.asc" "$t/data"
    refuse "verify-sig: a signature by a key not in the file" verify_sig "$t/a.asc" "$fa" "$t/data.b.asc" "$t/data"
    echo tampered > "$t/data2"
    refuse "verify-sig: tampered data" verify_sig "$t/a.asc" "$fa" "$t/data.asc" "$t/data2"
    GNUPGHOME="$gh" gpg --batch --yes --local-user "$fa" --clearsign --output "$t/InRelease" "$t/InRelease.txt" 2>/dev/null
    expect "verify-clear: a clearsigned index" verify_clear "$t/a.asc" "$fa" "$t/InRelease" "$t/out.txt"
    grep -q "main/binary-amd64/Packages" "$t/out.txt" 2>/dev/null && ok "verify-clear: writes the signed text" || bad "verify-clear: text"
    sed 's/57130 main\/binary-amd64/57131 main\/binary-amd64/' "$t/InRelease" > "$t/InRelease.bad"
    refuse "verify-clear: a tampered index" verify_clear "$t/a.asc" "$fa" "$t/InRelease.bad" "$t/out2.txt"
    [ ! -e "$t/out2.txt" ] && ok "verify-clear: no text from a tampered index" || bad "verify-clear: wrote text from a tampered index"
    GNUPGHOME="$gh" gpgconf --kill gpg-agent >/dev/null 2>&1 || true
  else
    echo "skip signatures: no gpg on this host"
  fi

  # verify-release
  local r="$t/rel" src
  mkdir -p "$r"
  echo source > "$r/mod_pagespeed-2.8.0.tar.gz"
  src="$(sha256sum "$r/mod_pagespeed-2.8.0.tar.gz" | cut -d' ' -f1)"
  mkrel() { # mkrel TAG PRERELEASE [extra jq]
    jq -n --arg tag "$1" --argjson pre "$2" --arg s "$src" '{schema:1, tag:$tag, version:"2.8.0", pkg_version:"2.8.0",
      prerelease:$pre, source_sha:"0123456789abcdef0123456789abcdef01234567", optimizer_tag:"optimizer-v2.8.0",
      files:[{asset:"mod_pagespeed-2.8.0.tar.gz", component:"source", kind:"tarball", sha256:$s}]}' > "$r/manifest.json"
    (cd "$r" && sha256sum manifest.json mod_pagespeed-2.8.0.tar.gz > SHA256SUMS)
  }
  mkrel module-v2.8.0 false
  out="$(verify_release "$r" module-v2.8.0 2>/dev/null)"
  case "$out" in *"source_sha=0123456789abcdef0123456789abcdef01234567"*"source_asset=mod_pagespeed-2.8.0.tar.gz"*) ok "verify-release: a good set" ;; *) bad "verify-release: $out" ;; esac
  refuse "verify-release: another tag" verify_release "$r" module-v2.8.1
  mkrel module-v2.8.0 true
  refuse "verify-release: a pre-release" verify_release "$r" module-v2.8.0
  mkrel module-v2.8.0 false
  echo changed > "$r/mod_pagespeed-2.8.0.tar.gz"
  refuse "verify-release: a changed tarball" verify_release "$r" module-v2.8.0
  echo source > "$r/mod_pagespeed-2.8.0.tar.gz"
  mkrel module-v2.8.0 false
  jq '.tag = "module-v2.8.0" | .files += [.files[0]]' "$r/manifest.json" > "$r/m" && mv "$r/m" "$r/manifest.json"
  refuse "verify-release: a manifest not in SHA256SUMS" verify_release "$r" module-v2.8.0
  (cd "$r" && sha256sum manifest.json mod_pagespeed-2.8.0.tar.gz > SHA256SUMS)
  refuse "verify-release: two source files" verify_release "$r" module-v2.8.0

  [ "$fail" = 0 ] && echo "nginx_org_lane self-test: ok"
  return "$fail"
}

case "${1:-}" in
  inputs) shift; inputs "$@" ;;
  pins) pins ;;
  verify-sig) shift; verify_sig "$@" ;;
  verify-clear) shift; verify_clear "$@" ;;
  verify-release) shift; verify_release "$@" ;;
  index-sha) shift; index_sha "$@" ;;
  previous) shift; previous "$@" ;;
  repomd-primary) shift; repomd_primary "$@" ;;
  previous-rpm) shift; previous_rpm "$@" ;;
  new-line) shift; new_line "$@" ;;
  --self-test) self_test ;;
  *) echo "usage: $0 inputs|verify-sig|verify-clear|verify-release|index-sha|previous|repomd-primary|previous-rpm|new-line ... | --self-test" >&2; exit 2 ;;
esac
