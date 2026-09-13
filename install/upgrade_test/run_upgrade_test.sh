#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# run_upgrade_test.sh -- scripted 1.15 -> 1.16 in-place upgrade rehearsal for
# the Apache module packages, in a booted-systemd container.
#
# WHY THIS EXISTS (the silent-degrade upgrade class). With the 1.16.0-rc.7 package pair, a host
# upgraded from 1.15 kept serving with X-Mod-Pagespeed intact while every
# Apache child logged, once, that it could not open the optimizer daemon's
# cache volume -- and in-place optimization stayed off for the life of the
# process. Every smoke that only looked at status codes and headers passed.
# This script is the host-plane proof the package rig lacked: it walks the
# exact path a 1.15 repository user takes at GA and refuses to pass unless
# in-place optimization is demonstrably ON after the upgrade.
#
# WHAT IT DOES, per distro:
#   1. boots a systemd container (privileged + host cgroup namespace -- the
#      same shape the daemon package rig uses; the units really run),
#   2. installs mod_pagespeed 1.15 for Apache exactly like a customer:
#      the public install.sh bootstrap (apt/yum source + signing key), then
#      `apt-get install mod-pagespeed` / `dnf install mod-pagespeed`; serves a
#      fixture page with an image and proves 1.15 optimizes (version header,
#      rewritten resource URL in the HTML, in-place image smaller than origin),
#   3. customizes the 1.15 config file (so the package manager must keep it),
#   4. upgrades IN PLACE to the 1.16 pair -- by default from the GitHub
#      release assets (downloaded by version, sha256-checked against the
#      release's SHA256SUMS and gpg-verified against the public signing key
#      published on the download site), with `apt-get install ./a.deb ./b.deb`
#      / `dnf install ./a.rpm ./b.rpm`; with --packages-dir, from locally
#      built packages (a pre-release check of the packaging itself; anything
#      the directory lacks is still downloaded and verified); or, with
#      --repo-url, from a package repository (`apt-get install mod-pagespeed`
#      pulling the optimizer via Depends) -- the path used to rehearse a
#      staging publish,
#   5. asserts the module package INSTALLED AND ENABLED its daemon drop-in
#      (pagespeed_daemon.conf: the two directives that point the module at
#      the daemon, at the optimizer package's defaults) -- owned by the
#      package, marked as a configuration file, listed by the web server's
#      include dump after the file that loads the module. Packages from
#      1.16.0-rc.14 on ship it; for an older pair the script writes the two
#      directives itself, as the release notes for those candidates
#      instruct, and says so. Also asserts the module package carries the
#      license text and the attribution notices (LICENSE, NOTICE and, from
#      1.16.0 on, THIRD-PARTY-NOTICES) under /usr/share/doc/mod-pagespeed/:
#      owned by the package, present on disk and, on rpm, flagged as license
#      and documentation. Then restarts the web server as the notes instruct,
#   6. asserts the silent-degrade class is ABSENT: zero error-log lines of either
#      signature after the restart, the daemon active as user pagespeed, the
#      web-server user in group pagespeed, socket + cache-dir modes as
#      designed, exactly one cache volume file, the new version in the
#      response header, the 1.15 config file byte-identical and accepted by
#      configtest, and in-place optimization PROVEN after traffic: the
#      daemon's notification counter moved (management API over its unix
#      socket) and/or the image is served smaller than origin.
#
# Any failed check exits non-zero with a one-line diagnosis per check and a
# tail of the relevant logs. --keep leaves the container running for a look.
#
# Usage:
#   install/upgrade_test/run_upgrade_test.sh --rc 1.16.0-rc.13 [--distro debian12]
#       [--repo-url https://.../staging | --packages-dir DIR] [--arch amd64|arm64]
#       [--keep] [--work DIR]
#
#   --rc VERSION        the 1.16 upstream version under test (SemVer spelling,
#                       e.g. 1.16.0-rc.13 or 1.16.0). Selects the release tag
#                       v<VERSION> and the expected package/header versions.
#   --distro NAME       debian12 (default) | debian13 | ubuntu2204 | ubuntu2404
#                       | alma9 | rocky9
#   --repo-url URL      upgrade from this package repository instead of the
#                       GitHub release assets (e.g. the auth-gated staging
#                       endpoint). Credentials, when needed, come from the
#                       PACKAGES_USER / PACKAGES_PASS environment (never from
#                       the command line), exactly as install.sh takes them.
#   --packages-dir DIR  upgrade with the module/optimizer packages found in
#                       this directory (locally built, e.g. by
#                       install/debian/build.sh / install/rpm/build.sh with
#                       -d <optimizer version>) instead of downloading them.
#                       A package the directory lacks is downloaded from the
#                       release and verified as usual; local packages are
#                       not signature-checked. A local MODULE package makes
#                       the drop-in and license-file assertions strict
#                       whatever --rc says.
#   --arch ARCH         amd64 | arm64 (default: the docker host's)
#   --release-repo R    owner/name of the GitHub repo carrying the release
#   --pubkey-url URL    where the public signing key is fetched from
#   --keep              leave the container (and the work dir) in place
#   --work DIR          scratch directory (default: mktemp)
#
# Requires on the host: docker (able to run a privileged container with the
# host cgroup namespace), gh (authenticated for the release repo, unless
# --repo-url or a complete --packages-dir), gpg, curl, sha256sum. Nothing
# here publishes or deploys.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

# ---------------------------------------------------------------------------
# Defaults + flags
# ---------------------------------------------------------------------------
RC=""
DISTRO="debian12"
REPO_URL=""
PKGS_DIR=""
ARCH=""
RELEASE_REPO="We-Amp/mod_pagespeed"
# The public key that signs the release ARTIFACTS (the detached .asc next to
# every download), from the stable location the 1.15 release manifest names.
# Note this is a different key from the one that signs the apt/yum
# REPOSITORY metadata (packages.modpagespeed.com/pubkey.gpg, rsa4096
# ...F50D6054F10712A0, which install.sh imports): the artifact key is the
# Ed25519 key ...DA8FEBD5000BC194. Both carry the same uid. Pinning the
# artifact key here means a release signed by anything else fails loudly.
PUBKEY_URL="https://modpagespeed.com/releases/v1.1.0/weamp-pkg-public.asc"
PUBKEY_ID="DA8FEBD5000BC194"
# The public bootstrap a 1.15 customer runs today.
INSTALL_SH_URL="https://packages.modpagespeed.com/install.sh"
BASELINE_PREFIX="1.15"
KEEP=0
WORK=""

usage() { sed -n '/^# Usage:/,/^# Requires/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rc) RC="$2"; shift 2 ;;
    --distro) DISTRO="$2"; shift 2 ;;
    --repo-url) REPO_URL="${2%/}"; shift 2 ;;
    --packages-dir) PKGS_DIR="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --release-repo) RELEASE_REPO="$2"; shift 2 ;;
    --pubkey-url) PUBKEY_URL="$2"; shift 2 ;;
    --pubkey-id) PUBKEY_ID="$2"; shift 2 ;;
    --install-sh-url) INSTALL_SH_URL="$2"; shift 2 ;;
    --baseline-prefix) BASELINE_PREFIX="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --work) WORK="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown flag: $1" >&2; usage ;;
  esac
done

[[ -n "$RC" ]] || { echo "error: --rc VERSION is required" >&2; usage; }
if [[ -n "$PKGS_DIR" && -n "$REPO_URL" ]]; then
  echo "error: --packages-dir and --repo-url are mutually exclusive" >&2; exit 2
fi
if [[ -n "$PKGS_DIR" ]]; then
  [[ -d "$PKGS_DIR" ]] || { echo "error: --packages-dir '$PKGS_DIR' is not a directory" >&2; exit 2; }
  PKGS_DIR="$(cd "$PKGS_DIR" && pwd)"
fi
if ! printf '%s' "$RC" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$'; then
  echo "error: --rc '$RC' is not X.Y.Z[-prerelease]" >&2; exit 2
fi

# SemVer prerelease -> dpkg/rpm tilde form. Expanded from a variable, never a
# literal '~' in the replacement (bash tilde-expands that; the release lane
# learned this the hard way).
TILDE='~'
PKGV="${RC/-/$TILDE}"            # 1.16.0~rc.13   (package Version of the pair)
PKGV_SAN="${PKGV//$TILDE/.}"     # 1.16.0.rc.13   (GitHub's sanitized asset spelling)
RELEASE_TAG="v${RC}"

case "$DISTRO" in
  debian12)   FAMILY=deb; IMAGE_BASE="debian:12" ;;
  debian13)   FAMILY=deb; IMAGE_BASE="debian:13" ;;
  ubuntu2204) FAMILY=deb; IMAGE_BASE="ubuntu:22.04" ;;
  ubuntu2404) FAMILY=deb; IMAGE_BASE="ubuntu:24.04" ;;
  alma9)      FAMILY=rpm; IMAGE_BASE="almalinux:9" ;;
  rocky9)     FAMILY=rpm; IMAGE_BASE="rockylinux:9" ;;
  *) echo "error: unknown --distro '$DISTRO'" >&2; usage ;;
esac

if [[ -z "$ARCH" ]]; then
  case "$(uname -m)" in
    x86_64) ARCH=amd64 ;;
    aarch64|arm64) ARCH=arm64 ;;
    *) echo "error: cannot map host arch $(uname -m); pass --arch" >&2; exit 2 ;;
  esac
fi
case "$ARCH" in
  amd64) DEB_ARCH=amd64; RPM_ARCH=x86_64; PLATFORM=linux/amd64 ;;
  arm64) DEB_ARCH=arm64; RPM_ARCH=aarch64; PLATFORM=linux/arm64 ;;
  *) echo "error: --arch must be amd64 or arm64" >&2; exit 2 ;;
esac

if [[ -z "$WORK" ]]; then
  WORK="$(mktemp -d -t upgrade-rehearsal.XXXXXX)"
fi
mkdir -p "$WORK/pkgs" "$WORK/fixture" "$WORK/logs"
WORK="$(cd "$WORK" && pwd)"

RUN_ID="$$-$(date +%s)"
CTR="mps-upgrade-${DISTRO}-${RUN_ID}"

# Web-server facts per family.
if [[ "$FAMILY" == deb ]]; then
  WEB_USER=www-data; WEB_UNIT=apache2; WEB_PROC=apache2; WEB_CTL="apache2ctl"
  ERROR_LOG=/var/log/apache2/error.log
  MODULE_CONF=/etc/apache2/mods-available/pagespeed.conf
  # The file that loads the module, and the directory the package's daemon
  # drop-in lands in. conf-enabled is included after mods-enabled, so any
  # drop-in name would work here; the package uses the one EL needs.
  LOADER_CONF=/etc/apache2/mods-enabled/pagespeed.load
  DROPIN_DIR=/etc/apache2/conf-available
  DROPIN_ENABLED_DIR=/etc/apache2/conf-enabled
else
  WEB_USER=apache; WEB_UNIT=httpd; WEB_PROC=httpd; WEB_CTL="httpd"
  ERROR_LOG=/var/log/httpd/error_log
  MODULE_CONF=/etc/httpd/conf.d/pagespeed.conf
  LOADER_CONF=/etc/httpd/conf.d/pagespeed.conf
  DROPIN_DIR=/etc/httpd/conf.d
  DROPIN_ENABLED_DIR=/etc/httpd/conf.d
fi
# The daemon drop-in the module package ships (install/common/pagespeed_daemon.conf).
# httpd includes conf.d/*.conf in sort order and on EL the LoadModule line
# lives in pagespeed.conf itself, so the drop-in must sort AFTER it: a
# "pagespeed-daemon.conf" ('-' < '.') is read before the module exists and
# its <IfModule pagespeed_module> block is skipped without a word -- the
# first alma9 run of this script proved it (daemon up, module never
# attached, zero log lines). '_' sorts after '.'.
DAEMON_DROPIN=pagespeed_daemon.conf
# First upstream version whose module packages ship the drop-in. Below it the
# script writes the two directives itself (the release notes of those
# candidates make that the operator's job) and only notes the gap.
DROPIN_SINCE="1.16.0-rc.14"
# First upstream version whose module packages carry the license text and the
# attribution notices under /usr/share/doc/mod-pagespeed/ (the Apache-2.0
# terms want both next to the binaries). Below it the gap is noted, not failed.
LICENSE_SINCE="1.16.0-rc.14"
# First upstream version whose module packages also carry THIRD-PARTY-NOTICES
# (the statically linked BSD/MIT/Zlib/IJG components ask that their notices
# accompany a binary redistribution). Below it the gap is noted, not failed.
TPN_SINCE="1.16.0"
PKG_DOCDIR=/usr/share/doc/mod-pagespeed
DAEMON_UNIT=pagespeed-optimizer
DAEMON_RUN=/run/pagespeed-optimizer
DAEMON_CACHE=/var/cache/pagespeed-optimizer/v1
FIXTURE_URL_DIR=/upgrade-fixture
IMAGE_NAME=Puzzle.jpg
# Same bytes under a URL the 1.15 module never saw: the post-upgrade in-place
# proof must not be satisfied by a 1.15-era entry that survived in the
# module's own file cache (it does survive -- and it did, on the first alma9
# run, while the daemon sat idle).
IMAGE_AFTER=Puzzle-after-upgrade.jpg

# ---------------------------------------------------------------------------
# Result bookkeeping
# ---------------------------------------------------------------------------
FAILS=0
PASSES=0
pass() { PASSES=$((PASSES + 1)); printf 'PASS  %s\n' "$*"; }
fail() { FAILS=$((FAILS + 1)); printf 'FAIL  %s\n' "$*"; }
note() { printf 'note  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# gh_release_download_wait <description> <gh release download args...>
# A release publish uploads assets AFTER the release object exists, so a
# rehearsal fired the moment a tag lands can outrun its own assets (the
# 2026-09-07 rc.15 failure: the run started four minutes before the publish
# finished). Poll with backoff instead of dying on the first 404. Bounded:
# ~15 minutes, then the original error, so a genuinely missing asset still
# fails loudly rather than hanging the lane.
gh_release_download_wait() {
  local desc="$1"; shift
  local attempt=1 max=30
  until gh release download "$@" 2>/dev/null; do
    if [[ "$attempt" -ge "$max" ]]; then
      # Surface the real error on the last failure.
      gh release download "$@" || die "$desc (does the release carry the assets? still missing after $max attempts)"
    fi
    note "$desc not available yet (attempt $attempt/$max) — the release publish may still be in flight; waiting 30s"
    sleep 30
    attempt=$((attempt + 1))
  done
}

# check <label> <expected> <actual>
check() {
  if [[ "$2" == "$3" ]]; then pass "$1: $3"; else fail "$1: expected '$2', got '$3'"; fi
}

CONTAINER_STARTED=0
# On exit: a passing run leaves nothing behind; a failing run removes the
# container but keeps the work dir (its logs/ are the diagnosis); --keep keeps
# both, container included, for a hands-on look.
cleanup() {
  local rc=$?
  if [[ "$CONTAINER_STARTED" -eq 1 && "$KEEP" -eq 1 ]]; then
    echo "keeping container $CTR (docker exec -it $CTR bash; docker rm -f $CTR when done)"
  elif [[ "$CONTAINER_STARTED" -eq 1 ]]; then
    docker rm -f "$CTR" >/dev/null 2>&1 || true
  fi
  if [[ "$KEEP" -eq 1 || "$rc" -ne 0 ]]; then
    echo "keeping work dir $WORK (logs under $WORK/logs)"
  else
    rm -rf "$WORK"
  fi
  exit "$rc"
}
trap cleanup EXIT

for tool in docker curl sha256sum gpg; do
  command -v "$tool" >/dev/null 2>&1 || die "missing required tool on the host: $tool"
done
if [[ -z "$REPO_URL" && -z "$PKGS_DIR" ]]; then
  command -v gh >/dev/null 2>&1 || die "missing gh (needed to download the release assets; or pass --repo-url / --packages-dir)"
fi

# semver_ge A B: A >= B for X.Y.Z[-pre]. A version without a prerelease is
# newer than one with; prerelease fields (rc.14) compare numerically when both
# are numeric, as strings otherwise, and fewer fields sort lower (SemVer 11).
semver_ge() {
  local a="$1" b="$2" acore bcore apre bpre i x y n
  acore="${a%%-*}"; bcore="${b%%-*}"
  apre="${a#"$acore"}"; apre="${apre#-}"
  bpre="${b#"$bcore"}"; bpre="${bpre#-}"
  local IFS=.
  local -a ac=($acore) bc=($bcore) ap=($apre) bp=($bpre)
  for i in 0 1 2; do
    (( ${ac[i]:-0} > ${bc[i]:-0} )) && return 0
    (( ${ac[i]:-0} < ${bc[i]:-0} )) && return 1
  done
  [[ -z "$apre" ]] && return 0
  [[ -z "$bpre" ]] && return 1
  n=${#ap[@]}; (( ${#bp[@]} > n )) && n=${#bp[@]}
  for (( i = 0; i < n; i++ )); do
    x="${ap[i]:-}"; y="${bp[i]:-}"
    [[ -z "$x" ]] && return 1
    [[ -z "$y" ]] && return 0
    if [[ "$x" =~ ^[0-9]+$ && "$y" =~ ^[0-9]+$ ]]; then
      (( x > y )) && return 0
      (( x < y )) && return 1
    else
      [[ "$x" > "$y" ]] && return 0
      [[ "$x" < "$y" ]] && return 1
    fi
  done
  return 0
}

# Run a command inside the container. stdin is /dev/null so a package
# manager can never wait on a prompt (a conffile question falls back to its
# default: keep the operator's file, exactly what a real unattended upgrade
# sees).
in_ctr() { docker exec -e DEBIAN_FRONTEND=noninteractive "$CTR" bash -c "$1" </dev/null; }
in_ctr_env() { # in_ctr_env VAR=VAL... -- CMD
  local -a envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=(-e "$1"); shift; done
  shift
  docker exec -e DEBIAN_FRONTEND=noninteractive "${envs[@]}" "$CTR" bash -c "$1" </dev/null
}

echo "upgrade rehearsal: ${BASELINE_PREFIX}.x -> ${RC} (package ${PKGV}) on ${DISTRO} (${IMAGE_BASE}, ${ARCH})"
if [[ -n "$REPO_URL" ]]; then
  echo "  upgrade source: ${REPO_URL}"
elif [[ -n "$PKGS_DIR" ]]; then
  echo "  upgrade source: local packages in ${PKGS_DIR} (missing ones from GitHub release ${RELEASE_TAG} on ${RELEASE_REPO})"
else
  echo "  upgrade source: GitHub release ${RELEASE_TAG} on ${RELEASE_REPO}"
fi
echo "  work dir:       ${WORK}"

# ---------------------------------------------------------------------------
# 0. Fetch + verify the 1.16 pair (GitHub-assets mode, with or without a
#    local --packages-dir supplying some or all of it)
# ---------------------------------------------------------------------------
# find_local_pkg <glob>: the single file in PKGS_DIR matching the glob in
# either version spelling (GitHub's '.' for the dpkg/rpm '~', or the '~' a
# local build writes), or nothing. Two matches are an error, not a guess.
find_local_pkg() {
  local p="$1" alt="${1//$PKGV_SAN/$PKGV}" f
  local -a cand=() m=()
  # nullglob only drops patterns that contain a wildcard; a wildcard-free
  # name that matches nothing comes back verbatim, so keep existing files only.
  shopt -s nullglob
  cand=("$PKGS_DIR"/$p)
  [[ "$alt" != "$p" ]] && cand+=("$PKGS_DIR"/$alt)
  shopt -u nullglob
  for f in ${cand[@]+"${cand[@]}"}; do [[ -f "$f" ]] && m+=("$f"); done
  (( ${#m[@]} <= 1 )) || die "more than one package in ${PKGS_DIR} matches '$p': ${m[*]}"
  (( ${#m[@]} == 1 )) && printf '%s' "${m[0]}"
  return 0
}
MODULE_IS_LOCAL=0
DOWNLOADED=0
declare -A LOCAL_PKG=()
if [[ -z "$REPO_URL" ]]; then
  if [[ "$FAMILY" == deb ]]; then
    PATTERNS=("pagespeed-optimizer_${RC}_${DEB_ARCH}.deb" "mod-pagespeed_${PKGV_SAN}-r*_${DEB_ARCH}.deb")
  else
    PATTERNS=("pagespeed-optimizer-${RC}-1.${RPM_ARCH}.rpm" "mod-pagespeed-${PKGV_SAN}-*.${RPM_ARCH}.rpm")
  fi
  if [[ -n "$PKGS_DIR" ]]; then
    step "taking the ${PKGV} pair for ${ARCH} from ${PKGS_DIR}"
  else
    step "downloading the ${RELEASE_TAG} pair for ${ARCH} from ${RELEASE_REPO}"
  fi
  for p in "${PATTERNS[@]}"; do
    if [[ -n "$PKGS_DIR" ]]; then
      local_pkg="$(find_local_pkg "$p")" || die "cannot pick a local package for '$p' (see above)"
      if [[ -n "$local_pkg" ]]; then
        cp "$local_pkg" "$WORK/pkgs/"
        LOCAL_PKG["$(basename "$local_pkg")"]=1
        note "local package: $(basename "$local_pkg") (not signature-checked)"
        case "$p" in mod-pagespeed[_-]*) MODULE_IS_LOCAL=1 ;; esac
        continue
      fi
      note "nothing in ${PKGS_DIR} matches '$p' (or its '~' spelling); downloading it from release ${RELEASE_TAG}"
      command -v gh >/dev/null 2>&1 || die "missing gh (needed to download the packages ${PKGS_DIR} lacks)"
    fi
    gh_release_download_wait "release asset '$p' for ${RELEASE_TAG}" \
      "$RELEASE_TAG" -R "$RELEASE_REPO" -D "$WORK/pkgs" --clobber -p "$p" -p "${p}.asc"
    DOWNLOADED=1
  done
  ls -la "$WORK/pkgs"
fi
if [[ -z "$REPO_URL" && "$DOWNLOADED" -eq 1 ]]; then
  gh_release_download_wait "SHA256SUMS for ${RELEASE_TAG}" \
    "$RELEASE_TAG" -R "$RELEASE_REPO" -D "$WORK" --clobber -p SHA256SUMS -p weamp-pkg-public.asc

  step "verifying the downloaded pair (sha256 against SHA256SUMS, gpg against the published key)"
  curl -fsSL "$PUBKEY_URL" -o "$WORK/pubkey-site.asc" || die "cannot fetch the public signing key from $PUBKEY_URL"
  export GNUPGHOME="$WORK/gnupg"; mkdir -p "$GNUPGHOME"; chmod 700 "$GNUPGHOME"
  gpg --batch -q --import "$WORK/pubkey-site.asc" 2>/dev/null || die "gpg import of the published key failed"
  IMPORTED_FPR="$(gpg --batch --with-colons --list-keys 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
  check "published signing key id" "$PUBKEY_ID" "${IMPORTED_FPR: -16}"
  if [[ -f "$WORK/weamp-pkg-public.asc" ]]; then
    if cmp -s "$WORK/weamp-pkg-public.asc" "$WORK/pubkey-site.asc"; then
      pass "release-attached public key is byte-identical to the one on the download site"
    else
      # Different armoring is possible in principle; compare the fingerprints.
      REL_FPR="$(gpg --batch --with-colons --show-keys "$WORK/weamp-pkg-public.asc" 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
      check "release-attached public key fingerprint matches the download site's" "$IMPORTED_FPR" "$REL_FPR"
    fi
  fi

  # Map each asset back to its SHA256SUMS line. GitHub rewrites '~' to '.' in
  # asset names while the manifest keeps the dpkg/rpm spelling, so compare
  # with the manifest name normalized the same way.
  for f in "$WORK"/pkgs/*.deb "$WORK"/pkgs/*.rpm; do
    [[ -e "$f" ]] || continue
    name="$(basename "$f")"
    [[ -n "${LOCAL_PKG[$name]:-}" ]] && continue
    want="$(awk -v n="$name" '{ m=$2; gsub(/~/, ".", m); if (m==n || $2==n) { print $1; exit } }' "$WORK/SHA256SUMS")"
    have="$(sha256sum "$f" | awk '{print $1}')"
    if [[ -z "$want" ]]; then
      fail "sha256: no SHA256SUMS entry matches asset $name"
    else
      check "sha256 of $name matches SHA256SUMS" "$want" "$have"
    fi
    if [[ -f "${f}.asc" ]]; then
      status="$(gpg --batch --status-fd 1 --trust-model always --verify "${f}.asc" "$f" 2>/dev/null || true)"
      sig_fpr="$(printf '%s\n' "$status" | awk '$2=="VALIDSIG"{print $3; exit}')"
      if [[ -n "$sig_fpr" && "${sig_fpr: -16}" == "$PUBKEY_ID" ]]; then
        pass "gpg signature on $name is valid and made by ${PUBKEY_ID}"
      else
        fail "gpg signature on $name: $(printf '%s\n' "$status" | grep -E 'BADSIG|ERRSIG|NO_PUBKEY|VALIDSIG' | head -1 || echo 'no VALIDSIG')"
      fi
    else
      fail "no detached signature (${name}.asc) in the release for $name"
    fi
  done
  unset GNUPGHOME
  if [[ "$FAILS" -gt 0 ]]; then die "refusing to install an unverified pair (see FAIL lines above)"; fi
fi

# The drop-in assertion is strict -- a module package without the drop-in
# FAILS -- when the module package under test is a local build (that is what
# --packages-dir is for) or when the version under test is one whose packages
# ship it. Below DROPIN_SINCE the public packages cannot carry it, so the gap
# is noted and the two directives are written by hand as the release notes
# of those candidates instruct; the rest of the run is unchanged.
DROPIN_STRICT=0
if [[ "$MODULE_IS_LOCAL" -eq 1 ]] || semver_ge "$RC" "$DROPIN_SINCE"; then DROPIN_STRICT=1; fi
echo "  daemon drop-in: ${DAEMON_DROPIN} expected from ${DROPIN_SINCE} on -- assertion $( [[ "$DROPIN_STRICT" -eq 1 ]] && echo strict || echo 'advisory (older pair)')"
LICENSE_STRICT=0
if [[ "$MODULE_IS_LOCAL" -eq 1 ]] || semver_ge "$RC" "$LICENSE_SINCE"; then LICENSE_STRICT=1; fi
echo "  license files:  ${PKG_DOCDIR}/{LICENSE,NOTICE} expected from ${LICENSE_SINCE} on -- assertion $( [[ "$LICENSE_STRICT" -eq 1 ]] && echo strict || echo 'advisory (older pair)')"
TPN_STRICT=0
if [[ "$MODULE_IS_LOCAL" -eq 1 ]] || semver_ge "$RC" "$TPN_SINCE"; then TPN_STRICT=1; fi
echo "  notices:        ${PKG_DOCDIR}/THIRD-PARTY-NOTICES -- strict whenever the package under test ships it; an absence fails from ${TPN_SINCE} on (currently $( [[ "$TPN_STRICT" -eq 1 ]] && echo strict || echo 'advisory (older pair)'))"

# ---------------------------------------------------------------------------
# 1. Fixture: a page with an image and a stylesheet
# ---------------------------------------------------------------------------
cp "$REPO_ROOT/install/mod_pagespeed_example/images/$IMAGE_NAME" "$WORK/fixture/$IMAGE_NAME"
cp "$WORK/fixture/$IMAGE_NAME" "$WORK/fixture/$IMAGE_AFTER"
ORIGIN_IMAGE_BYTES="$(stat -c %s "$WORK/fixture/$IMAGE_NAME")"
cat > "$WORK/fixture/style.css" <<'CSS'
/* upgrade rehearsal stylesheet: whitespace and a comment for the minifier */
body {
    margin:  0;
    padding: 0;
}
.puzzle {
    display: block;
}
CSS
cat > "$WORK/fixture/index.html" <<HTML
<!DOCTYPE html>
<html>
<head>
  <title>upgrade rehearsal</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <p>upgrade rehearsal fixture</p>
  <img class="puzzle" src="${IMAGE_NAME}" alt="puzzle">
</body>
</html>
HTML

# ---------------------------------------------------------------------------
# 2. Image: the distro + systemd + the web server, nothing of ours
# ---------------------------------------------------------------------------
step "building the ${DISTRO} systemd image"
DOCKERFILE="$WORK/Dockerfile"
if [[ "$FAMILY" == deb ]]; then
  cat > "$DOCKERFILE" <<EOF
FROM ${IMAGE_BASE}
ENV DEBIAN_FRONTEND=noninteractive
# The stock Ubuntu image excludes /usr/share/doc/*; a real host does not.
RUN rm -f /etc/dpkg/dpkg.cfg.d/excludes
RUN apt-get -qq update \\
    && apt-get -qq -y install --no-install-recommends \\
        systemd systemd-sysv procps apache2 curl ca-certificates gnupg python3 file \\
    && rm -rf /var/lib/apt/lists/*
# A booted systemd in a privileged container shares the HOST kernel's binfmt_misc
# table; stock systemd-binfmt.service registers the image's binfmt.d entries into
# it at boot and unregisters EVERY entry on an orderly shutdown. A guest must not
# manage the host's table: mask the service and the binfmt_misc mount units.
RUN for u in systemd-binfmt.service proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount; do \\
        ln -sf /dev/null "/etc/systemd/system/\$u"; done
CMD ["/sbin/init"]
EOF
else
  cat > "$DOCKERFILE" <<EOF
FROM ${IMAGE_BASE}
# A base image may carry tsflags=nodocs, which makes dnf skip %doc files while
# rpm -ql still lists them; a real host does not. Mirrors the deb image's
# dpkg-exclude removal so the module package's documentation lands on disk.
RUN [ ! -f /etc/dnf/dnf.conf ] || sed -i '/^tsflags=/d' /etc/dnf/dnf.conf
RUN dnf -y install --setopt=install_weak_deps=False \\
        systemd procps-ng httpd python3 shadow-utils util-linux file diffutils \\
    && dnf clean all
# Same host-table protection as the deb image (EL9's minimal systemd may lack the
# unit; masking a missing unit is harmless).
RUN for u in systemd-binfmt.service proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount; do \\
        ln -sf /dev/null "/etc/systemd/system/\$u"; done
CMD ["/sbin/init"]
EOF
fi
IMAGE_HASH="$(sha256sum "$DOCKERFILE" | cut -c1-12)"
IMAGE="mps-upgrade-rehearsal:${DISTRO}-${ARCH}-${IMAGE_HASH}"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build --platform "$PLATFORM" -t "$IMAGE" -f "$DOCKERFILE" "$WORK" >"$WORK/logs/docker-build.log" 2>&1 \
    || { tail -30 "$WORK/logs/docker-build.log"; die "docker build failed (log: $WORK/logs/docker-build.log)"; }
fi
note "image $IMAGE"

# The guest must leave the HOST binfmt_misc table alone (masked units above);
# snapshot it before boot and compare after boot and after shutdown.
HOST_BINFMT_BEFORE="$(docker run --rm --privileged --platform "$PLATFORM" "$IMAGE_BASE" sh -c 'mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null; ls /proc/sys/fs/binfmt_misc' 2>/dev/null | sort | tr "\n" " ")"
step "booting systemd in container ${CTR}"
docker run -d --name "$CTR" --platform "$PLATFORM" --privileged --cgroupns=host \
  -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock --tmpfs /tmp \
  -e container=docker \
  -v "$WORK/pkgs:/pkgs:ro" -v "$WORK/fixture:/fixture:ro" \
  "$IMAGE" /sbin/init >"$WORK/logs/docker-run.log" 2>&1 \
  || { cat "$WORK/logs/docker-run.log"; die "container refused to start"; }
CONTAINER_STARTED=1
ready=""
state=""
for _ in $(seq 1 60); do
  state="$(docker exec "$CTR" systemctl is-system-running 2>/dev/null || true)"
  case "$state" in running|degraded) ready=1; break ;; esac
  sleep 2
done
[[ -n "$ready" ]] || { docker logs "$CTR" 2>&1 | tail -20; die "systemd never reached running/degraded (last: ${state:-?})"; }
pass "systemd is ${state} in the container"
# `systemctl is-enabled` prints the state AND exits non-zero for a masked unit,
# so read the printed state and never fall back to a literal.
BINFMT_UNIT_STATE="$(docker exec "$CTR" systemctl is-enabled systemd-binfmt.service 2>/dev/null || true)"
if [[ "$BINFMT_UNIT_STATE" == masked ]]; then
  pass "guest systemd-binfmt.service is masked (host binfmt_misc table left alone)"
else
  fail "guest systemd-binfmt.service is NOT masked (state: ${BINFMT_UNIT_STATE:-<none>}): an orderly shutdown would unregister every host binfmt entry"
fi
HOST_BINFMT_AFTER_BOOT="$(docker run --rm --privileged --platform "$PLATFORM" "$IMAGE_BASE" sh -c 'mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null; ls /proc/sys/fs/binfmt_misc' 2>/dev/null | sort | tr "\n" " ")"
if [[ "$HOST_BINFMT_AFTER_BOOT" == "$HOST_BINFMT_BEFORE" ]]; then
  pass "host binfmt_misc table unchanged by the guest boot"
else
  fail "host binfmt_misc table changed by the guest boot (before: ${HOST_BINFMT_BEFORE:-<empty>} after: ${HOST_BINFMT_AFTER_BOOT:-<empty>})"
fi

# ---------------------------------------------------------------------------
# helpers that run inside the container
# ---------------------------------------------------------------------------
# fetch_until <test> <timeout s> <command run in the container>
# Re-runs the command until its stdout satisfies `[[ <stdout> <test> ]]`,
# e.g. fetch_until '-lt 200000' 90 "curl -s -o /dev/null -w %{size_download} URL".
# Prints the last value; returns 1 on timeout.
fetch_until() {
  local test_expr="$1" timeout="$2" cmd="$3" deadline v
  deadline=$(( $(date +%s) + timeout ))
  while :; do
    v="$(in_ctr "$cmd" 2>/dev/null || true)"
    v="${v//[$'\r\n']/}"
    if [[ -n "$v" ]] && eval "[[ '$v' $test_expr ]]" 2>/dev/null; then printf '%s' "$v"; return 0; fi
    if (( $(date +%s) >= deadline )); then printf '%s' "${v:-}"; return 1; fi
    sleep 2
  done
}
header_value() { # header_value <url> <header-name-regex>
  in_ctr "curl -sI '$1' | tr -d '\r' | grep -i -E '^($2):' | head -1 | cut -d: -f2- | sed 's/^ *//'" || true
}
version_header() { header_value "$1" 'X-Mod-Pagespeed|X-Page-Speed'; }

configtest() { in_ctr "$WEB_CTL -t 2>&1" | grep -q 'Syntax OK'; }

restart_web() {
  in_ctr "systemctl restart $WEB_UNIT" || return 1
  for _ in $(seq 1 30); do
    if in_ctr "curl -s --max-time 2 -o /dev/null http://localhost/"; then return 0; fi
    sleep 1
  done
  in_ctr "tail -30 $ERROR_LOG" || true
  return 1
}

pkg_version() { # pkg_version <name>
  if [[ "$FAMILY" == deb ]]; then
    in_ctr "dpkg-query -W -f='\${Version}' $1 2>/dev/null" || true
  else
    in_ctr "rpm -q --qf '%{VERSION}-%{RELEASE}' $1 2>/dev/null" || true
  fi
}

# ---------------------------------------------------------------------------
# 3. Baseline: install 1.15 like a customer, serve the fixture, prove it optimizes
# ---------------------------------------------------------------------------
step "installing mod_pagespeed ${BASELINE_PREFIX} from the public repository (install.sh + package manager)"
in_ctr "curl -fsSL '$INSTALL_SH_URL' | sh" >"$WORK/logs/install-sh.log" 2>&1 \
  || { tail -20 "$WORK/logs/install-sh.log"; die "install.sh failed"; }
if [[ "$FAMILY" == deb ]]; then
  in_ctr "apt-get install -y -qq mod-pagespeed" >"$WORK/logs/install-baseline.log" 2>&1 \
    || { tail -30 "$WORK/logs/install-baseline.log"; die "apt-get install mod-pagespeed failed"; }
  in_ctr "grep -E '^deb ' /etc/apt/sources.list.d/modpagespeed.list" | sed 's/^/  apt source: /' || true
else
  in_ctr "dnf install -y -q mod-pagespeed" >"$WORK/logs/install-baseline.log" 2>&1 \
    || { tail -30 "$WORK/logs/install-baseline.log"; die "dnf install mod-pagespeed failed"; }
  in_ctr "grep -E '^baseurl' /etc/yum.repos.d/modpagespeed.repo | head -1" | sed 's/^/  yum baseurl: /' || true
fi
BASELINE_VERSION="$(pkg_version mod-pagespeed)"
case "$BASELINE_VERSION" in
  ${BASELINE_PREFIX}*) pass "baseline package installed: mod-pagespeed ${BASELINE_VERSION}" ;;
  *) fail "baseline package version '${BASELINE_VERSION}' does not start with ${BASELINE_PREFIX}" ;;
esac
if in_ctr "getent group pagespeed" >/dev/null 2>&1; then
  note "group 'pagespeed' already exists before the upgrade (unexpected on a 1.15 host)"
else
  pass "no 'pagespeed' group before the upgrade (the optimizer package creates it)"
fi

step "serving the fixture and customizing the ${BASELINE_PREFIX} configuration"
in_ctr "mkdir -p /var/www/html${FIXTURE_URL_DIR} && cp /fixture/* /var/www/html${FIXTURE_URL_DIR}/ && chmod -R a+rX /var/www/html${FIXTURE_URL_DIR}"
# Cacheable fixture resources (in-place optimization only touches cacheable
# responses); mod_headers is stock on both families.
in_ctr "cat > ${DROPIN_DIR}/upgrade-fixture.conf <<'EOF'
<Directory /var/www/html${FIXTURE_URL_DIR}>
    Header set Cache-Control \"public, max-age=3600\"
</Directory>
EOF"
# A realistic operator customization of the shipped 1.15 conffile: the
# upgrade must keep this file untouched, and 1.16 must still accept every line
# (including a retired filter name, which 1.16 accepts as a no-op).
in_ctr "cat >> ${MODULE_CONF} <<'EOF'

# --- upgrade-rehearsal operator customization (must survive the upgrade) ---
<IfModule pagespeed_module>
    ModPagespeedEnableFilters collapse_whitespace,remove_comments
    ModPagespeedEnableFilters in_place_optimize_for_browser
    ModPagespeedFileCacheSizeKb 204800
    ModPagespeedStatisticsLogging on
    ModPagespeedImageRecompressionQuality 75
</IfModule>
EOF
cp ${MODULE_CONF} /root/pagespeed.conf.before-upgrade"
if [[ "$FAMILY" == deb ]]; then
  in_ctr "a2enmod -q headers pagespeed && a2enconf -q upgrade-fixture" >/dev/null
else
  in_ctr "systemctl enable -q httpd"
fi
if configtest; then
  pass "baseline configtest: Syntax OK with the customized conffile"
else
  fail "baseline configtest failed"; in_ctr "$WEB_CTL -t" || true
fi
restart_web || die "web server did not come up after the baseline install"

step "proving ${BASELINE_PREFIX} optimizes"
BASE_URL="http://localhost${FIXTURE_URL_DIR}"
SIZE_CMD="curl -s -o /dev/null -w %{size_download} '${BASE_URL}/${IMAGE_NAME}'"
REWRITE_CMD="curl -s -H 'Cache-Control: no-cache' '${BASE_URL}/index.html' | grep -c 'pagespeed\\.'"
HDR="$(version_header "$BASE_URL/index.html")"
case "$HDR" in
  *"${BASELINE_PREFIX}"*) pass "baseline version header: ${HDR}" ;;
  *) fail "baseline version header missing or wrong: '${HDR}'" ;;
esac
# CoreFilters rewrites the image/css URLs once the resources are optimized.
if rewritten="$(fetch_until '-ge 1' 120 "$REWRITE_CMD")"; then
  pass "baseline HTML rewriting: ${rewritten} rewritten resource URL(s) in the page"
else
  fail "baseline HTML rewriting: no .pagespeed. URL appeared within 120s"
fi
if size="$(fetch_until "-lt $ORIGIN_IMAGE_BYTES" 120 "$SIZE_CMD")"; then
  pass "baseline in-place optimization: ${IMAGE_NAME} served at ${size} bytes (origin ${ORIGIN_IMAGE_BYTES})"
else
  fail "baseline in-place optimization: ${IMAGE_NAME} still ${size:-?} bytes after 120s (origin ${ORIGIN_IMAGE_BYTES})"
fi

# ---------------------------------------------------------------------------
# 4. Upgrade in place to the 1.16 pair
# ---------------------------------------------------------------------------
if [[ -z "$REPO_URL" ]]; then
  step "upgrading in place from the release assets (both packages in one transaction)"
  if [[ "$FAMILY" == deb ]]; then
    in_ctr "apt-get install -y /pkgs/pagespeed-optimizer_*.deb /pkgs/mod-pagespeed_*.deb" >"$WORK/logs/upgrade.log" 2>&1 \
      || { tail -40 "$WORK/logs/upgrade.log"; die "apt-get install of the pair failed"; }
  else
    in_ctr "dnf install -y /pkgs/pagespeed-optimizer-*.rpm /pkgs/mod-pagespeed-*.rpm" >"$WORK/logs/upgrade.log" 2>&1 \
      || { tail -40 "$WORK/logs/upgrade.log"; die "dnf install of the pair failed"; }
  fi
else
  step "upgrading in place from the package repository ${REPO_URL}"
  # Same bootstrap a customer runs, pointed at the given repository. The
  # credentials ride the environment into the container and end up in the
  # 0600 files install.sh writes, never on a command line.
  in_ctr_env "PACKAGES_URL=${REPO_URL}" "PACKAGES_USER=${PACKAGES_USER:-}" "PACKAGES_PASS=${PACKAGES_PASS:-}" -- \
    "if [ -n \"\$PACKAGES_USER\" ]; then curl -fsSL -u \"\$PACKAGES_USER:\$PACKAGES_PASS\" '${REPO_URL%/staging}/install.sh' | sh; else curl -fsSL '${REPO_URL%/staging}/install.sh' | sh; fi" \
    >"$WORK/logs/install-sh-upgrade.log" 2>&1 \
    || { tail -20 "$WORK/logs/install-sh-upgrade.log"; die "install.sh (upgrade source) failed"; }
  if [[ "$FAMILY" == deb ]]; then
    in_ctr "apt-get update -qq && apt-get install -y mod-pagespeed" >"$WORK/logs/upgrade.log" 2>&1 \
      || { tail -40 "$WORK/logs/upgrade.log"; die "apt-get install mod-pagespeed (repository upgrade) failed"; }
  else
    in_ctr "dnf upgrade -y mod-pagespeed" >"$WORK/logs/upgrade.log" 2>&1 \
      || { tail -40 "$WORK/logs/upgrade.log"; die "dnf upgrade mod-pagespeed (repository upgrade) failed"; }
  fi
fi
grep -i -E 'conffile|keeping|rpmnew|rpmsave|warning' "$WORK/logs/upgrade.log" | sed 's/^/  pkgmgr: /' | head -8 || true

MOD_VERSION="$(pkg_version mod-pagespeed)"
OPT_VERSION="$(pkg_version pagespeed-optimizer)"
case "$MOD_VERSION" in
  "${PKGV}"*) pass "module package upgraded: mod-pagespeed ${MOD_VERSION}" ;;
  *) fail "module package version '${MOD_VERSION}' does not start with ${PKGV}" ;;
esac
case "$OPT_VERSION" in
  "${PKGV}"|"${PKGV}-"*) pass "optimizer package installed: pagespeed-optimizer ${OPT_VERSION}" ;;
  *) fail "optimizer package version '${OPT_VERSION}' is not ${PKGV}" ;;
esac
if [[ "$FAMILY" == rpm ]]; then
  note "rpm signature status: $(in_ctr "rpm -q --qf '%{NAME}: %{SIGPGP:pgpsig}\n' mod-pagespeed pagespeed-optimizer 2>/dev/null | tr '\n' ';'" || true)"
fi

step "the module package's license files: ${PKG_DOCDIR}/{LICENSE,NOTICE,THIRD-PARTY-NOTICES}"
# An Apache-2.0 distribution carries the license text and the attribution
# notices next to the binaries. Each must be listed by the package manager as
# the module package's (dpkg -L / rpm -ql) and be present, non-empty, on disk
# -- the container keeps documentation (the deb image's dpkg exclude and any
# rpm tsflags=nodocs are removed above), so an on-disk miss is the package's
# fault. On rpm the two
# must also be flagged as what they are: rpm -qL (license files) lists LICENSE,
# rpm -qd (documentation) lists NOTICE.
license_files_ok=1
for f in LICENSE NOTICE; do
  if [[ "$FAMILY" == deb ]]; then
    listed="$(in_ctr "dpkg -L mod-pagespeed 2>/dev/null | grep -x ${PKG_DOCDIR}/${f}" || true)"
  else
    listed="$(in_ctr "rpm -ql mod-pagespeed 2>/dev/null | grep -x ${PKG_DOCDIR}/${f}" || true)"
  fi
  if [[ -z "$listed" ]]; then
    license_files_ok=0
    if [[ "$LICENSE_STRICT" -eq 1 ]]; then
      fail "module package ${MOD_VERSION} does not ship ${PKG_DOCDIR}/${f} (packages from ${LICENSE_SINCE} on carry it)"
    else
      note "module package ${MOD_VERSION} predates the packaged ${f} (ships from ${LICENSE_SINCE})"
    fi
  elif in_ctr "test -s ${PKG_DOCDIR}/${f}"; then
    pass "${PKG_DOCDIR}/${f} is owned by mod-pagespeed and present on disk"
  else
    license_files_ok=0
    fail "${PKG_DOCDIR}/${f} is listed by the package but missing or empty on disk"
  fi
done
# THIRD-PARTY-NOTICES ships from 1.16.0 on (the statically linked
# BSD/MIT/Zlib/IJG components ask that their notices accompany a binary
# redistribution). The gate is presence-based: a package that SHIPS the file
# is held to it strictly whatever its version -- there is no legitimate way
# for a shipped file to be empty or stripped of the IJG statement -- while an
# absent file fails only when the package is at/past TPN_SINCE (or a local
# build), and is merely noted for an older published pair. The advisory path
# must not touch license_files_ok: an older pair lacking the file is not a
# defect, and clearing the flag would skip the LICENSE-text and rpm-flag
# assertions below that predated this gate.
f=THIRD-PARTY-NOTICES
if [[ "$FAMILY" == deb ]]; then
  listed="$(in_ctr "dpkg -L mod-pagespeed 2>/dev/null | grep -x ${PKG_DOCDIR}/${f}" || true)"
else
  listed="$(in_ctr "rpm -ql mod-pagespeed 2>/dev/null | grep -x ${PKG_DOCDIR}/${f}" || true)"
fi
if [[ -n "$listed" ]]; then
  if in_ctr "test -s ${PKG_DOCDIR}/${f}"; then
    pass "${PKG_DOCDIR}/${f} is owned by mod-pagespeed and present on disk"
    if in_ctr "grep -q 'Independent JPEG Group' ${PKG_DOCDIR}/${f}"; then
      pass "${PKG_DOCDIR}/${f} carries the Independent JPEG Group statement"
    else
      license_files_ok=0
      fail "${PKG_DOCDIR}/${f} lacks the Independent JPEG Group statement (libjpeg-turbo is statically linked)"
    fi
  else
    license_files_ok=0
    fail "${PKG_DOCDIR}/${f} is listed by the package but missing or empty on disk"
  fi
elif [[ "$TPN_STRICT" -eq 1 ]]; then
  license_files_ok=0
  fail "module package ${MOD_VERSION} does not ship ${PKG_DOCDIR}/${f} (packages from ${TPN_SINCE} on carry it)"
else
  note "module package ${MOD_VERSION} predates the packaged ${f} (ships from ${TPN_SINCE})"
fi
if [[ "$license_files_ok" -eq 1 ]]; then
  if in_ctr "grep -q 'Apache License' ${PKG_DOCDIR}/LICENSE"; then
    pass "${PKG_DOCDIR}/LICENSE is the Apache License text"
  else
    fail "${PKG_DOCDIR}/LICENSE is not the Apache License text"
  fi
  if [[ "$FAMILY" == rpm ]]; then
    if in_ctr "rpm -qL mod-pagespeed 2>/dev/null | grep -qx ${PKG_DOCDIR}/LICENSE"; then
      pass "LICENSE is flagged as a license file in the rpm (rpm -qL)"
    else
      fail "LICENSE is not flagged as a license file in the rpm (rpm -qL does not list it)"
    fi
    if in_ctr "rpm -qd mod-pagespeed 2>/dev/null | grep -qx ${PKG_DOCDIR}/NOTICE"; then
      pass "NOTICE is flagged as documentation in the rpm (rpm -qd)"
    else
      fail "NOTICE is not flagged as documentation in the rpm (rpm -qd does not list it)"
    fi
  fi
fi

step "the module package's daemon drop-in: ${DROPIN_DIR}/${DAEMON_DROPIN}"
# From 1.16.0-rc.14 the module package ships the two directives that point
# the module at the daemon: installed, owned by the package, marked as
# a configuration file so operator edits survive upgrades, enabled, and read
# by the web server AFTER the file that loads the module (on EL conf.d/*.conf
# is read in sort order and a name sorting before pagespeed.conf is skipped).
# For a pair that predates it, the two directives from those candidates'
# release notes are written by hand, as before.
write_dropin_by_hand() {
  in_ctr "cat > ${DROPIN_DIR}/${DAEMON_DROPIN} <<'EOF'
<IfModule pagespeed_module>
    ModPagespeedDaemonSocketPath ${DAEMON_RUN}/notify.sock
    ModPagespeedDaemonVolumePath ${DAEMON_CACHE}/cache
</IfModule>
EOF"
  if [[ "$FAMILY" == deb ]]; then in_ctr "a2enconf -q ${DAEMON_DROPIN%.conf}" >/dev/null; fi
}
if in_ctr "test -f ${DROPIN_DIR}/${DAEMON_DROPIN}"; then
  pass "packaged daemon drop-in present: ${DROPIN_DIR}/${DAEMON_DROPIN}"
  if [[ "$FAMILY" == deb ]]; then
    check "drop-in owned by package" "mod-pagespeed" \
      "$(in_ctr "dpkg -S ${DROPIN_DIR}/${DAEMON_DROPIN} 2>/dev/null | cut -d: -f1" || true)"
    if in_ctr "dpkg-query -W -f='\${Conffiles}\n' mod-pagespeed 2>/dev/null | grep -q ' ${DROPIN_DIR}/${DAEMON_DROPIN} '"; then
      pass "drop-in is a dpkg conffile (operator edits survive upgrades)"
    else
      fail "drop-in is NOT registered as a dpkg conffile: an upgrade would overwrite operator edits"
    fi
    if in_ctr "test -L ${DROPIN_ENABLED_DIR}/${DAEMON_DROPIN}"; then
      pass "drop-in enabled: ${DROPIN_ENABLED_DIR}/${DAEMON_DROPIN} -> $(in_ctr "readlink ${DROPIN_ENABLED_DIR}/${DAEMON_DROPIN}" || true)"
    else
      fail "drop-in NOT enabled: no ${DROPIN_ENABLED_DIR}/${DAEMON_DROPIN} symlink (postinst did not a2enconf it)"
    fi
  else
    check "drop-in owned by package" "mod-pagespeed" \
      "$(in_ctr "rpm -qf --qf '%{NAME}' ${DROPIN_DIR}/${DAEMON_DROPIN} 2>/dev/null" || true)"
    if in_ctr "rpm -qc mod-pagespeed 2>/dev/null | grep -qx ${DROPIN_DIR}/${DAEMON_DROPIN}"; then
      pass "drop-in is an rpm %config file (operator edits survive upgrades)"
    else
      fail "drop-in is NOT an rpm %config file: an upgrade would overwrite operator edits"
    fi
  fi
  check "drop-in socket path" "${DAEMON_RUN}/notify.sock" \
    "$(in_ctr "awk '\$1==\"ModPagespeedDaemonSocketPath\"{print \$2}' ${DROPIN_DIR}/${DAEMON_DROPIN}" || true)"
  check "drop-in volume path" "${DAEMON_CACHE}/cache" \
    "$(in_ctr "awk '\$1==\"ModPagespeedDaemonVolumePath\"{print \$2}' ${DROPIN_DIR}/${DAEMON_DROPIN}" || true)"
  if in_ctr "grep -q '^<IfModule pagespeed_module>' ${DROPIN_DIR}/${DAEMON_DROPIN}"; then
    pass "drop-in directives are inside <IfModule pagespeed_module>"
  else
    fail "drop-in directives are not guarded by <IfModule pagespeed_module>"
  fi
  # The web server's own view: the drop-in is in the include tree, after the
  # file that loads the module. DUMP_INCLUDES lists files in processing order.
  in_ctr "$WEB_CTL -t -D DUMP_INCLUDES 2>/dev/null" > "$WORK/logs/dump-includes.log" || true
  loader_at="$(grep -n -F -- "${LOADER_CONF}" "$WORK/logs/dump-includes.log" | head -1 | cut -d: -f1)"
  dropin_at="$(grep -n -F -- "/${DAEMON_DROPIN}" "$WORK/logs/dump-includes.log" | head -1 | cut -d: -f1)"
  if [[ -z "$dropin_at" ]]; then
    fail "the web server's include dump does not list ${DAEMON_DROPIN} (${WEB_CTL} -t -D DUMP_INCLUDES)"
  elif [[ -z "$loader_at" ]]; then
    fail "the web server's include dump does not list the module loader ${LOADER_CONF}"
  elif (( dropin_at > loader_at )); then
    pass "include dump lists ${DAEMON_DROPIN} after the module loader $(basename "$LOADER_CONF") (the <IfModule> block is read with the module present)"
  else
    fail "include dump lists ${DAEMON_DROPIN} BEFORE the module loader $(basename "$LOADER_CONF"): its <IfModule> block is skipped"
  fi
elif [[ "$DROPIN_STRICT" -eq 1 ]]; then
  fail "module package ${MOD_VERSION} did not install ${DROPIN_DIR}/${DAEMON_DROPIN} (packages from ${DROPIN_SINCE} on ship it)"
  # Written by hand anyway, so the remaining checks tell a missing drop-in
  # from an unrelated silent-degrade failure.
  write_dropin_by_hand
else
  note "module package ${MOD_VERSION} predates the packaged daemon drop-in (ships from ${DROPIN_SINCE}); writing the two directives by hand as the release notes of those candidates instruct"
  write_dropin_by_hand
fi

step "restarting the web server (as the release notes instruct)"
# Turn on the daemon's management API over its group-scoped unix socket -- the
# one-line enablement documented in /etc/default/pagespeed-optimizer -- so the
# notification counter can be read. Optional for serving; used for the proof.
in_ctr "test -f /etc/default/pagespeed-optimizer || install -m 0640 -o root -g pagespeed /dev/null /etc/default/pagespeed-optimizer;
        if grep -q '^OPTIMIZER_OPTS=' /etc/default/pagespeed-optimizer; then sed -i 's/^OPTIMIZER_OPTS=.*/OPTIMIZER_OPTS=--api-socket/' /etc/default/pagespeed-optimizer; else echo 'OPTIMIZER_OPTS=--api-socket' >> /etc/default/pagespeed-optimizer; fi;
        systemctl restart ${DAEMON_UNIT}"
api_ok=""
for _ in $(seq 1 30); do
  if in_ctr "test -S ${DAEMON_RUN}/api.sock"; then api_ok=1; break; fi
  sleep 1
done
if [[ -n "$api_ok" ]]; then
  note "daemon management API socket is up at ${DAEMON_RUN}/api.sock"
else
  fail "daemon management API socket did not appear: without it the size proof cannot tell daemon IPRO from classic in-module IPRO"
fi

# Remember where the error log ends BEFORE the post-upgrade restart so the
# silent-degrade scan only sees lines the upgraded module wrote.
LOG_OFFSET="$(in_ctr "stat -c %s ${ERROR_LOG} 2>/dev/null || echo 0")"
LOG_OFFSET="${LOG_OFFSET//[^0-9]/}"; LOG_OFFSET="${LOG_OFFSET:-0}"
if configtest; then
  pass "post-upgrade configtest: Syntax OK (1.15 conffile + daemon drop-in accepted by ${RC})"
else
  fail "post-upgrade configtest failed (a 1.15 directive rejected?)"; in_ctr "$WEB_CTL -t" || true
fi
restart_web || fail "web server did not come back after the upgrade restart"

# ---------------------------------------------------------------------------
# 5. Assertions: the silent-degrade class is absent and in-place optimization is on
# ---------------------------------------------------------------------------
step "daemon identity, group membership, filesystem modes"
check "daemon unit active" "active" "$(in_ctr "systemctl is-active ${DAEMON_UNIT}" || true)"
DAEMON_PID="$(in_ctr "systemctl show -p MainPID --value ${DAEMON_UNIT}" || true)"
check "daemon runs as user pagespeed" "pagespeed" "$(in_ctr "ps -o user= -p ${DAEMON_PID:-0} | tr -d ' '" || true)"
check "unit User=" "pagespeed" "$(in_ctr "systemctl show -p User --value ${DAEMON_UNIT}" || true)"
check "unit UMask=" "0007" "$(in_ctr "systemctl show -p UMask --value ${DAEMON_UNIT}" || true)"
groups_now="$(in_ctr "id -nG ${WEB_USER}" || true)"
case " $groups_now " in
  *" pagespeed "*) pass "web-server user ${WEB_USER} is in group pagespeed (groups: ${groups_now})" ;;
  *) fail "web-server user ${WEB_USER} is NOT in group pagespeed (groups: ${groups_now})" ;;
esac
check "cache dir ${DAEMON_CACHE} mode" "3770" "$(in_ctr "stat -c %a ${DAEMON_CACHE}" || true)"
check "cache dir owner:group" "pagespeed:pagespeed" "$(in_ctr "stat -c %U:%G ${DAEMON_CACHE}" || true)"
check "runtime dir ${DAEMON_RUN} mode" "750" "$(in_ctr "stat -c %a ${DAEMON_RUN}" || true)"
check "notify socket mode" "660" "$(in_ctr "stat -c %a ${DAEMON_RUN}/notify.sock" || true)"
check "notify socket owner:group" "pagespeed:pagespeed" "$(in_ctr "stat -c %U:%G ${DAEMON_RUN}/notify.sock" || true)"
volumes="$(in_ctr "ls -1 ${DAEMON_CACHE}/cache-* 2>/dev/null" || true)"
check "exactly one cache volume file (no split cache)" "1" "$(printf '%s\n' "$volumes" | grep -c . || true)"
for v in $volumes; do
  check "volume $(basename "$v") mode" "660" "$(in_ctr "stat -c %a $v" || true)"
done
# Group membership is only real inside the serving children (the silent-degrade log
# line came from there, not from the parent).
pagespeed_gid="$(in_ctr "getent group pagespeed | cut -d: -f3" || true)"
child_groups="$(in_ctr "p=\$(pgrep -u ${WEB_USER} -x ${WEB_PROC} | head -1); [ -n \"\$p\" ] && grep -E '^Groups:' /proc/\$p/status" || true)"
case " $(printf '%s' "$child_groups" | sed -E 's/^Groups:[[:space:]]*//') " in
  *" ${pagespeed_gid} "*) pass "running web-server child carries gid ${pagespeed_gid} (pagespeed) -- the restart took effect" ;;
  *) fail "running web-server child does not carry gid ${pagespeed_gid} (pagespeed): ${child_groups:-no child found}" ;;
esac

step "configuration compatibility"
if in_ctr "cmp -s /root/pagespeed.conf.before-upgrade ${MODULE_CONF}"; then
  pass "1.15 conffile ${MODULE_CONF} is byte-identical after the upgrade"
else
  fail "1.15 conffile ${MODULE_CONF} was changed by the upgrade"
  in_ctr "diff /root/pagespeed.conf.before-upgrade ${MODULE_CONF}" | head -20 || true
fi
in_ctr "ls ${MODULE_CONF}.dpkg-dist ${MODULE_CONF}.rpmnew 2>/dev/null" | sed 's/^/  packaged new conffile kept aside as: /' || true
if in_ctr "$WEB_CTL -M 2>/dev/null | grep -q pagespeed_module"; then
  pass "pagespeed_module is loaded"
else
  fail "pagespeed_module is not loaded after the upgrade"
fi

step "version header and the silent-degrade log signatures"
HDR="$(version_header "$BASE_URL/index.html")"
case "$HDR" in
  *"${RC}"*) pass "post-upgrade version header: ${HDR}" ;;
  *) fail "post-upgrade version header does not carry ${RC}: '${HDR}'" ;;
esac
# Traffic first, so any per-child attach failure has had every chance to be
# logged before the scan.
in_ctr "for i in \$(seq 1 12); do curl -s -o /dev/null '${BASE_URL}/index.html'; curl -s -o /dev/null '${BASE_URL}/${IMAGE_NAME}'; curl -s -o /dev/null '${BASE_URL}/${IMAGE_AFTER}'; curl -s -o /dev/null '${BASE_URL}/style.css'; sleep 1; done" || true
in_ctr "tail -c +$((LOG_OFFSET + 1)) ${ERROR_LOG}" > "$WORK/logs/error-log-after-upgrade.log" || true
SIG_RE='nothing will be recorded for in-place optimization|cannot open the optimizer daemon.s cache volume|not being in the .pagespeed. group|Giving up: in-place optimization stays off'
sig_hits="$(grep -c -E "$SIG_RE" "$WORK/logs/error-log-after-upgrade.log" || true)"
check "silent-degrade signature lines in the error log after the upgrade restart" "0" "$sig_hits"
if [[ "$sig_hits" != "0" ]]; then grep -E "$SIG_RE" "$WORK/logs/error-log-after-upgrade.log" | head -5 | sed 's/^/  /'; fi
perm_hits="$(grep -c -i -E 'permission denied.*(pagespeed-optimizer|notify\.sock|shared config)|cache_dir_generation' "$WORK/logs/error-log-after-upgrade.log" || true)"
check "daemon permission/handshake complaints in the error log" "0" "$perm_hits"
if grep -q -i -E 'daemon.*(legacy layout|publishing no generation)' "$WORK/logs/error-log-after-upgrade.log"; then
  fail "module treats the daemon as a pre-privilege-drop legacy layout"
fi

step "in-place optimization through the daemon"
ipro_proven=0
if [[ -n "$api_ok" ]]; then
  # notifications.received counts the module telling the daemon "a new
  # original is in the cache" -- the arm silently disabled.
  NOTIF_CMD="curl -s -o /dev/null '${BASE_URL}/index.html'; curl -s -o /dev/null '${BASE_URL}/${IMAGE_AFTER}'; curl -s --unix-socket ${DAEMON_RUN}/api.sock http://localhost/v1/stats | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"notifications\"][\"received\"])'"
  if notif="$(fetch_until '-ge 1' 90 "$NOTIF_CMD")"; then
    pass "daemon notification counter moved: notifications.received=${notif}"
    ipro_proven=1
  else
    fail "daemon notification counter did not move within 90s (notifications.received='${notif:-?}')"
  fi
  in_ctr "curl -s --unix-socket ${DAEMON_RUN}/api.sock http://localhost/v1/stats" > "$WORK/logs/daemon-stats.json" 2>/dev/null || true
else
  fail "notification counter unavailable (no management API socket): the served-asset proof alone is not evidence of daemon IPRO"
fi
SIZE_AFTER_CMD="curl -s -o /dev/null -w %{size_download} '${BASE_URL}/${IMAGE_AFTER}'"
if size="$(fetch_until "-lt $ORIGIN_IMAGE_BYTES" 150 "$SIZE_AFTER_CMD")"; then
  pass "post-upgrade in-place optimization: ${IMAGE_AFTER} (never seen by 1.15) served at ${size} bytes (origin ${ORIGIN_IMAGE_BYTES})"
  ipro_proven=1
else
  fail "post-upgrade in-place optimization: ${IMAGE_AFTER} still ${size:-?} bytes after 150s (origin ${ORIGIN_IMAGE_BYTES})"
fi
note "for reference, ${IMAGE_NAME} (already in the 1.15 cache) is served at $(in_ctr "$SIZE_CMD" || echo '?') bytes"
[[ "$ipro_proven" -eq 1 ]] || fail "no proof of in-place optimization after the upgrade"

# Diagnostics for the record, whatever the outcome.
in_ctr "journalctl -u ${DAEMON_UNIT} --no-pager -n 40" > "$WORK/logs/daemon-journal.log" 2>&1 || true
in_ctr "tail -40 ${ERROR_LOG}" > "$WORK/logs/error-log-tail.log" 2>&1 || true

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "=== ${DISTRO} (${ARCH}): ${PASSES} passed, ${FAILS} failed -- ${BASELINE_VERSION} -> ${MOD_VERSION} + optimizer ${OPT_VERSION}"
if [[ "$FAILS" -gt 0 ]]; then
  echo "--- error log after the upgrade restart (tail) ---"; tail -20 "$WORK/logs/error-log-after-upgrade.log" 2>/dev/null || true
  echo "--- daemon journal (tail) ---"; tail -20 "$WORK/logs/daemon-journal.log" 2>/dev/null || true
  echo "::error::upgrade rehearsal FAILED on ${DISTRO}: ${FAILS} check(s) -- see FAIL lines above"
  [[ "$KEEP" -eq 1 ]] || echo "(re-run with --keep to inspect the container)"
  exit 1
fi
echo "upgrade rehearsal PASSED on ${DISTRO}"
