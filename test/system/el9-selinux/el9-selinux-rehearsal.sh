#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# el9-selinux-rehearsal.sh -- the 1.15 -> 1.16 in-place upgrade rehearsal on
# an ENFORCING-SELinux EL9 host. Runs INSIDE the guest VM as root.
#
# WHY THIS EXISTS. A tracked gap in the container rehearsal, and a
# GA-gate decision for 2.1 that one enforcing-EL9 VM run must close it. The
# container rehearsal (install/upgrade_test/run_upgrade_test.sh) proves the
# upgrade path on a booted-systemd container, but a container shares the
# host kernel and therefore can never be SELinux-enforcing. The 1.16 pair
# puts the serving module (httpd_t) in touch with the optimizer daemon
# through a memory-mapped cache volume and a unix notify socket -- exactly
# the two operations the stock targeted policy is expected to deny for
# httpd_t against a daemon it knows nothing about. The daemon's own policy
# module (.te/.fc in the daemon repo, deploy/selinux/) is a DRAFT and is not
# packaged. This script is the enforcing-host run the GA gate asked for.
#
# WHAT IT DOES. The phases mirror the container rehearsal, minimally, so the
# two reports read alike; anything the container script asserts that does
# not change under SELinux is kept only where it helps tell a SELinux denial
# from an unrelated daemon-startup failure:
#   0. preflight: EL9, SELinux ENFORCING (refuses to run otherwise), auditd
#      up, the SELinux tooling present; records the audit-log position so
#      every denial the run causes is attributable to a phase,
#   1. a fixture page with an image and a stylesheet under /var/www/html,
#   2. baseline: mod_pagespeed 1.15 from the public repository (install.sh +
#      dnf), a customized conffile, the web server enabled; proves 1.15
#      optimizes (version header, rewritten URL, smaller in-place image).
#      Denials during THIS phase are the 1.15 product's own SELinux story
#      and are reported separately (a finding, not the gate),
#   3. upgrade in place to the 1.16 pair -- from --packages-dir (rpms staged
#      by the runner: rc.14 candidates, or locally built) or from --repo-url
#      (the staging rehearsal path). Asserts the packaged daemon drop-in
#      (pagespeed_daemon.conf, from 1.16.0-rc.14 on) like the container
#      script does; writes the two directives by hand for an older pair.
#      Optionally (--policy draft) builds and loads the daemon's draft
#      SELinux module first, so the run measures the policy, not its
#      absence,
#   4. assertions, with SELinux ENFORCING throughout: daemon active as user
#      pagespeed, web-server child carries the pagespeed gid, the module
#      reaches the notify socket (the daemon's notification counter moves),
#      in-place optimization works for a URL 1.15 never saw, ZERO AVC /
#      USER_AVC denials for comm=httpd and comm=pagespeed-optimizer since
#      the upgrade began, and httpd still runs confined as httpd_t (a run
#      where it did not would prove nothing),
#   5. diagnostics into --artifacts on every run (fuller on failure):
#      getenforce/sestatus, ausearch -i of the denials, audit2allow of the
#      same (the exact rules a policy would need -- the gate's decision
#      input), sealert if installed, semodule -l, the daemon journal,
#      httpd -t -D DUMP_INCLUDES, contexts of every path involved.
#
# PASS, for the GA gate, means: SELinux enforcing at start AND end; no AVC
# for httpd_t against the daemon's cache volume or notify socket and no AVC
# for the daemon; the module talks to the daemon and optimizes in place.
#
# Usage (as root, inside the guest):
#   bash el9-selinux-rehearsal.sh [--rc 1.16.0-rc.14]
#       (--packages-dir DIR | --repo-url URL)
#       [--policy none|draft] [--policy-src DIR]
#       [--baseline-selinux rw-content|none] [--disable-dontaudit]
#       [--payload DIR] [--artifacts DIR] [--dry-run]
#
#   --rc VERSION        the 1.16 upstream version under test (SemVer, e.g.
#                       1.16.0-rc.14). With --packages-dir it defaults to the
#                       Version of the module rpm found there.
#   --packages-dir DIR  upgrade with the module + optimizer rpms in DIR
#                       (exactly one of each). Default: <payload>/pkgs when
#                       that directory holds rpms.
#   --repo-url URL      upgrade from this package repository instead (the
#                       staging endpoint); credentials from PACKAGES_USER /
#                       PACKAGES_PASS in the environment, as install.sh takes
#                       them. Mutually exclusive with --packages-dir.
#   --policy MODE       none (default): run against what the packages ship
#                       -- today no policy at all, so the daemon runs as
#                       unconfined_service_t and httpd_t meets var_t files.
#                       draft: build the daemon repo's draft policy module
#                       from --policy-src (pagespeed-optimizer.te + .fc) with
#                       selinux-policy-devel, semodule -i it, restorecon the
#                       daemon's paths and restart the daemon BEFORE the
#                       post-upgrade web-server restart.
#   --policy-src DIR    where the .te/.fc live (default: <payload>/selinux).
#   --baseline-selinux  rw-content (default): before starting 1.15, label the
#                       module's own cache and log directories as web-server
#                       writable content (semanage fcontext + restorecon) --
#                       the correct form of the fix the product docs give
#                       for "SELinux blocks mod_pagespeed". Without it the
#                       module cannot write its own cache under enforcing
#                       and every later daemon-path denial hides behind that.
#                       none: default labels, to measure the raw picture.
#   --disable-dontaudit semodule -DB for the run (re-enabled at exit) so
#                       denials the policy hides by dontaudit rules show up.
#   --payload DIR       where the runner staged this script, the fixture
#                       image (Puzzle.jpg), pkgs/ and selinux/ (default: the
#                       directory this script is in).
#   --artifacts DIR     where diagnostics land (default: <payload>/artifacts).
#   --dry-run           print the resolved plan and exit 0 before touching
#                       the system (also works off-host, for a syntax check).
#   --install-sh-url U  the public 1.15 bootstrap (default: the released one).
#   --baseline-prefix P the version prefix the baseline must have (1.15).
#
# Exit code: 0 on PASS, 1 on any FAIL, 2 on usage/preflight errors. The
# summary line at the end is what the runner keys on.

set -euo pipefail
export LC_ALL=C

# Line-buffer stdout so the ssh-captured stream on the runner side shows
# progress as it happens (same re-exec dance as the cPanel target script;
# routed via `bash "$0"` because scp from a Windows filesystem does not
# carry a +x bit).
if [[ -z "${EL9_REHEARSAL_STDBUF:-}" ]] && command -v stdbuf >/dev/null 2>&1; then
  export EL9_REHEARSAL_STDBUF=1
  exec stdbuf -oL -eL "${BASH:-bash}" "$0" "$@"
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Defaults + flags
# ---------------------------------------------------------------------------
RC=""
PKGS_DIR=""
REPO_URL=""
POLICY="none"
POLICY_SRC=""
BASELINE_SELINUX="rw-content"
DISABLE_DONTAUDIT=0
PAYLOAD="$HERE"
ARTIFACTS=""
DRY_RUN=0
INSTALL_SH_URL="https://packages.modpagespeed.com/install.sh"
BASELINE_PREFIX="1.15"

usage() { sed -n '/^# Usage/,/^# Exit code/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rc) RC="$2"; shift 2 ;;
    --packages-dir) PKGS_DIR="$2"; shift 2 ;;
    --repo-url) REPO_URL="${2%/}"; shift 2 ;;
    --policy) POLICY="$2"; shift 2 ;;
    --policy-src) POLICY_SRC="$2"; shift 2 ;;
    --baseline-selinux) BASELINE_SELINUX="$2"; shift 2 ;;
    --disable-dontaudit) DISABLE_DONTAUDIT=1; shift ;;
    --payload) PAYLOAD="$2"; shift 2 ;;
    --artifacts) ARTIFACTS="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --install-sh-url) INSTALL_SH_URL="$2"; shift 2 ;;
    --baseline-prefix) BASELINE_PREFIX="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown flag: $1" >&2; usage ;;
  esac
done

case "$POLICY" in none|draft) ;; *) echo "error: --policy must be none or draft" >&2; exit 2 ;; esac
case "$BASELINE_SELINUX" in rw-content|none) ;; *) echo "error: --baseline-selinux must be rw-content or none" >&2; exit 2 ;; esac
if [[ -n "$PKGS_DIR" && -n "$REPO_URL" ]]; then
  echo "error: --packages-dir and --repo-url are mutually exclusive" >&2; exit 2
fi
[[ -d "$PAYLOAD" ]] || { echo "error: --payload '$PAYLOAD' is not a directory" >&2; exit 2; }
PAYLOAD="$(cd "$PAYLOAD" && pwd)"
[[ -n "$ARTIFACTS" ]] || ARTIFACTS="$PAYLOAD/artifacts"
[[ -n "$POLICY_SRC" ]] || POLICY_SRC="$PAYLOAD/selinux"
if [[ -z "$PKGS_DIR" && -z "$REPO_URL" ]]; then
  # The runner stages rpms under <payload>/pkgs; take them when present.
  if compgen -G "$PAYLOAD/pkgs/*.rpm" >/dev/null; then PKGS_DIR="$PAYLOAD/pkgs"; fi
fi
if [[ -z "$PKGS_DIR" && -z "$REPO_URL" ]]; then
  echo "error: no upgrade source: pass --packages-dir DIR or --repo-url URL (or stage rpms in $PAYLOAD/pkgs)" >&2; exit 2
fi
if [[ -n "$PKGS_DIR" ]]; then
  [[ -d "$PKGS_DIR" ]] || { echo "error: --packages-dir '$PKGS_DIR' is not a directory" >&2; exit 2; }
  PKGS_DIR="$(cd "$PKGS_DIR" && pwd)"
  OPT_RPMS=(); MOD_RPMS=()
  for f in "$PKGS_DIR"/pagespeed-optimizer-*.rpm; do [[ -f "$f" ]] && OPT_RPMS+=("$f"); done
  for f in "$PKGS_DIR"/mod-pagespeed-*.rpm; do [[ -f "$f" ]] && MOD_RPMS+=("$f"); done
  [[ ${#OPT_RPMS[@]} -eq 1 ]] || { echo "error: expected exactly one pagespeed-optimizer-*.rpm in $PKGS_DIR, found ${#OPT_RPMS[@]}" >&2; exit 2; }
  [[ ${#MOD_RPMS[@]} -eq 1 ]] || { echo "error: expected exactly one mod-pagespeed-*.rpm in $PKGS_DIR, found ${#MOD_RPMS[@]}" >&2; exit 2; }
  if [[ -z "$RC" ]]; then
    if command -v rpm >/dev/null 2>&1; then
      v="$(rpm -qp --qf '%{VERSION}' "${MOD_RPMS[0]}" 2>/dev/null || true)"
      TILDE='~'
      RC="${v/$TILDE/-}"
    fi
  fi
fi
[[ -n "$RC" ]] || { echo "error: --rc VERSION is required (could not derive it from the module rpm)" >&2; exit 2; }
if ! printf '%s' "$RC" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$'; then
  echo "error: --rc '$RC' is not X.Y.Z[-prerelease]" >&2; exit 2
fi
if [[ "$POLICY" == draft ]]; then
  [[ -f "$POLICY_SRC/pagespeed-optimizer.te" && -f "$POLICY_SRC/pagespeed-optimizer.fc" ]] \
    || { echo "error: --policy draft needs pagespeed-optimizer.te and .fc in $POLICY_SRC" >&2; exit 2; }
fi

# SemVer prerelease -> rpm tilde form (expanded from a variable, never a
# literal '~' in the replacement: bash tilde-expands that).
TILDE='~'
PKGV="${RC/-/$TILDE}"            # 1.16.0~rc.14

# Web-server + daemon facts (EL / rpm family only).
WEB_USER=apache; WEB_UNIT=httpd; WEB_PROC=httpd; WEB_CTL="httpd"
ERROR_LOG=/var/log/httpd/error_log
MODULE_CONF=/etc/httpd/conf.d/pagespeed.conf
LOADER_CONF=/etc/httpd/conf.d/pagespeed.conf
DROPIN_DIR=/etc/httpd/conf.d
DAEMON_DROPIN=pagespeed_daemon.conf
DROPIN_SINCE="1.16.0-rc.14"
DAEMON_UNIT=pagespeed-optimizer
DAEMON_BIN=/usr/bin/pagespeed-optimizer
DAEMON_RUN=/run/pagespeed-optimizer
DAEMON_CACHE=/var/cache/pagespeed-optimizer/v1
MODULE_CACHE=/var/cache/mod_pagespeed
MODULE_LOG=/var/log/pagespeed
FIXTURE_URL_DIR=/upgrade-fixture
DOCROOT=/var/www/html
IMAGE_NAME=Puzzle.jpg
IMAGE_AFTER=Puzzle-after-upgrade.jpg
AUDIT_LOG=/var/log/audit/audit.log

# ---------------------------------------------------------------------------
# Result bookkeeping
# ---------------------------------------------------------------------------
FAILS=0
PASSES=0
WARNS=0
pass() { PASSES=$((PASSES + 1)); printf 'PASS  %s\n' "$*"; }
fail() { FAILS=$((FAILS + 1)); printf 'FAIL  %s\n' "$*"; }
warn() { WARNS=$((WARNS + 1)); printf 'WARN  %s\n' "$*"; }
note() { printf 'note  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 2; }
check() { # check <label> <expected> <actual>
  if [[ "$2" == "$3" ]]; then pass "$1: $3"; else fail "$1: expected '$2', got '$3'"; fi
}

# semver_ge A B: A >= B for X.Y.Z[-pre] (same rules as the container script).
semver_ge() {
  local a="$1" b="$2" acore bcore apre bpre i x y n
  acore="${a%%-*}"; bcore="${b%%-*}"
  apre="${a#"$acore"}"; apre="${apre#-}"
  bpre="${b#"$bcore"}"; bpre="${bpre#-}"
  local IFS=.
  # shellcheck disable=SC2206  # splitting on IFS=. is the point here
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

echo "enforcing-SELinux upgrade rehearsal: ${BASELINE_PREFIX}.x -> ${RC} (package ${PKGV})"
if [[ -n "$PKGS_DIR" ]]; then
  echo "  upgrade source:   local rpms in ${PKGS_DIR}"
  echo "                    $(basename "${OPT_RPMS[0]}")"
  echo "                    $(basename "${MOD_RPMS[0]}")"
else
  echo "  upgrade source:   package repository ${REPO_URL}"
fi
echo "  policy:           ${POLICY}$( [[ "$POLICY" == draft ]] && echo " (from ${POLICY_SRC})")"
echo "  baseline SELinux: ${BASELINE_SELINUX}"
echo "  dontaudit:        $( [[ "$DISABLE_DONTAUDIT" -eq 1 ]] && echo disabled || echo 'as shipped')"
echo "  payload:          ${PAYLOAD}"
echo "  artifacts:        ${ARTIFACTS}"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "dry run: nothing executed"
  exit 0
fi

# ---------------------------------------------------------------------------
# 0. Preflight
# ---------------------------------------------------------------------------
step "preflight"
[[ "$(id -u)" == 0 ]] || die "must run as root"
# shellcheck disable=SC1091
. /etc/os-release
case "${ID:-}" in
  almalinux|rocky|rhel|centos) ;;
  *) die "this is an EL9 rehearsal; /etc/os-release ID='${ID:-?}'" ;;
esac
case "${VERSION_ID:-}" in 9*) ;; *) die "this is an EL9 rehearsal; VERSION_ID='${VERSION_ID:-?}'" ;; esac
note "host: ${PRETTY_NAME:-?} ($(uname -r), $(uname -m))"
command -v getenforce >/dev/null 2>&1 || die "getenforce missing: SELinux userland not installed"
ENFORCE_AT_START="$(getenforce 2>/dev/null || echo unknown)"
if [[ "$ENFORCE_AT_START" != Enforcing ]]; then
  die "SELinux is '${ENFORCE_AT_START}', not Enforcing -- this rig exists to measure enforcing. Fix /etc/selinux/config (SELINUX=enforcing), reboot, re-run."
fi
pass "SELinux is Enforcing at start"
sestatus 2>/dev/null | sed 's/^/  /' || true

mkdir -p "$ARTIFACTS"
# Tooling: the SELinux diagnostics, auditd for the AVC record, the web server
# the customer already has, python3 for the JSON stats read. Idempotent.
dnf install -y -q --setopt=install_weak_deps=False \
  httpd curl python3 audit policycoreutils policycoreutils-python-utils setools-console \
  >"$ARTIFACTS/preflight-dnf.log" 2>&1 || { tail -20 "$ARTIFACTS/preflight-dnf.log"; die "preflight dnf install failed"; }
systemctl enable -q auditd 2>/dev/null || true
systemctl start auditd 2>/dev/null || true
check "auditd active (AVC records land in ${AUDIT_LOG})" "active" "$(systemctl is-active auditd || true)"
if [[ "$DISABLE_DONTAUDIT" -eq 1 ]]; then
  semodule -DB && note "dontaudit rules disabled for this run (semodule -DB)"
fi
restore_dontaudit() { if [[ "$DISABLE_DONTAUDIT" -eq 1 ]]; then semodule -B >/dev/null 2>&1 || true; fi; }

# AVC accounting. Denials are counted from byte offsets into the audit log,
# so each phase sees exactly the records it caused; the ausearch dumps in the
# artifacts use a timestamp for the human-readable form.
START_TS="$(date '+%m/%d/%Y %H:%M:%S')"
audit_offset() { stat -c %s "$AUDIT_LOG" 2>/dev/null || echo 0; }
# avc_since <offset> [comm-regex]: raw AVC/USER_AVC records since <offset>,
# optionally only those whose comm= matches.
avc_since() {
  local off="$1" comm="${2:-}"
  if [[ -f "$AUDIT_LOG" ]]; then
    tail -c +$((off + 1)) "$AUDIT_LOG" 2>/dev/null | grep -E '^type=(AVC|USER_AVC) ' || true
  else
    journalctl -k -o cat --since "@$(date -d "$START_TS" +%s)" 2>/dev/null | grep -E 'avc: +denied' || true
  fi | { if [[ -n "$comm" ]]; then grep -E "comm=\"?(${comm})" || true; else cat; fi; }
}
avc_count() { avc_since "$@" | grep -c . || true; }
# Stock-stack net_admin exclusion (rehearsal finding 4.3). The root httpd
# parent's setsockopt(SOL_SOCKET, SO_SNDBUFFORCE) is denied CAP_NET_ADMIN at
# every httpd (re)start on a stock EL9 host; the rehearsal's isolation work
# proved it comes from the stock EL9 module stack, not mod_pagespeed, and
# the distro policy dontaudits it deliberately -- it is exactly the class
# --disable-dontaudit exists to surface, so with -DB it appears at every
# (re)start of the run. The shipped pagespeed policy deliberately carries
# NO allow for it: an allow would hand every root-context httpd_t process
# real CAP_NET_ADMIN (netlink, iptables) to silence a harmless hidden EPERM
# in another component's syscall. The gate therefore excludes the class
# from the httpd denial count, scoped to precisely the measured shape
# (capability net_admin, httpd_t -> httpd_t), and reports the excluded
# records separately, split at the moment the shipped policy could first
# exist on the host (POLICY_TS): before it the denial precedes any
# installable policy by construction; after it the record is the same
# dontaudited stock class, visible only because this run disabled dontaudit.
avc_stock_net_admin() {
  grep -E 'avc: +denied +\{ net_admin \}.*scontext=\S+:httpd_t:\S+ +tcontext=\S+:httpd_t:\S+ +tclass=capability' || true
}
avc_drop_stock_net_admin() {
  grep -vE 'avc: +denied +\{ net_admin \}.*scontext=\S+:httpd_t:\S+ +tcontext=\S+:httpd_t:\S+ +tclass=capability' || true
}
# policy_load_ts <byte-offset>: epoch of the first kernel policy load the
# audit log records after <byte-offset>; empty when there is none.
policy_load_ts() {
  [[ -f "$AUDIT_LOG" ]] || return 0
  tail -c +$(($1 + 1)) "$AUDIT_LOG" 2>/dev/null \
    | grep -m1 -E '^type=MAC_POLICY_LOAD ' \
    | sed -E 's/.*msg=audit\(([0-9]+).*/\1/' || true
}
# avc_summary: one line per (comm, class, perms, target type, path) group.
avc_summary() {
  sed -E 's/.*denied +\{ ([^}]*)\}.*comm="([^"]*)".*/\2|\1|&/' \
    | awk -F'|' '{
        line=$3; comm=$1; perms=$2;
        tcls=""; ttype=""; path="";
        if (match(line, /tclass=[a-z_]+/)) tcls=substr(line, RSTART+7, RLENGTH-7);
        if (match(line, /tcontext=[^ ]+/)) { ttype=substr(line, RSTART+9, RLENGTH-9); n=split(ttype, a, ":"); ttype=a[3]; }
        if (match(line, /path="[^"]*"/)) path=substr(line, RSTART+6, RLENGTH-7);
        else if (match(line, /name="[^"]*"/)) path=substr(line, RSTART+6, RLENGTH-7);
        gsub(/ +$/, "", perms);
        key=comm " " tcls " {" perms "} " ttype " " path;
        c[key]++
      } END { for (k in c) printf "  %3d x %s\n", c[k], k }' | sort -rn
}
# avc_classes: which of the gate's surfaces a set of records touches.
avc_classes() {
  local in; in="$(cat)"
  [[ -n "$in" ]] || return 0
  printf '%s\n' "$in" | grep -q -E "connectto" && echo "  class: connectto (httpd_t -> the daemon's unix stream socket)"
  printf '%s\n' "$in" | grep -q -E "${DAEMON_RUN}|notify\.sock|pagespeed_runtime_t" && echo "  class: notify socket (${DAEMON_RUN})"
  printf '%s\n' "$in" | grep -q -E "pagespeed-optimizer/v1|pagespeed_cache_t|cache-[0-9]" && echo "  class: daemon cache volume (${DAEMON_CACHE})"
  printf '%s\n' "$in" | grep -q -E "mod_pagespeed|/var/log/pagespeed" && echo "  class: the module's own cache/log directories (1.15-era layout)"
  printf '%s\n' "$in" | grep -q -E 'comm="pagespeed-optim' && echo "  class: the daemon itself (comm=pagespeed-optimizer)"
  # A record set matching none of the classes above is still a valid outcome
  # (e.g. the stock-module-stack startup denials); without this the last
  # grep's status leaks out of the function and kills the set -e/pipefail
  # run right here, mid-report, before the gate phase.
  return 0
}

OFF_START="$(audit_offset)"

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
# fetch_until <test> <timeout s> <command>: re-runs the command until its
# stdout satisfies `[[ <stdout> <test> ]]`; prints the last value; 1 on timeout.
fetch_until() {
  local test_expr="$1" timeout="$2" cmd="$3" deadline v
  deadline=$(( $(date +%s) + timeout ))
  while :; do
    v="$(bash -c "$cmd" 2>/dev/null || true)"
    v="${v//[$'\r\n']/}"
    if [[ -n "$v" ]] && eval "[[ '$v' $test_expr ]]" 2>/dev/null; then printf '%s' "$v"; return 0; fi
    if (( $(date +%s) >= deadline )); then printf '%s' "${v:-}"; return 1; fi
    sleep 2
  done
}
header_value() { curl -sI "$1" | tr -d '\r' | grep -i -E "^($2):" | head -1 | cut -d: -f2- | sed 's/^ *//' || true; }
version_header() { header_value "$1" 'X-Mod-Pagespeed|X-Page-Speed'; }
configtest() { $WEB_CTL -t 2>&1 | grep -q 'Syntax OK'; }
restart_web() {
  systemctl restart "$WEB_UNIT" || return 1
  for _ in $(seq 1 30); do
    if curl -s --max-time 2 -o /dev/null http://localhost/; then return 0; fi
    sleep 1
  done
  tail -30 "$ERROR_LOG" || true
  return 1
}
pkg_version() { rpm -q --qf '%{VERSION}-%{RELEASE}' "$1" 2>/dev/null || true; }
label_of() { ls -Zd "$1" 2>/dev/null | awk '{print $1}' || true; }
domain_of_pid() { ps -o label= -p "${1:-0}" 2>/dev/null | tr -d ' ' || true; }

# Diagnostics for the record; the whole set on failure, the light set always.
collect_diagnostics() {
  local full="$1" d="$ARTIFACTS"
  {
    echo "getenforce: $(getenforce 2>/dev/null || echo ?)"
    sestatus 2>/dev/null || true
  } > "$d/selinux-status.txt"
  ausearch -m AVC,USER_AVC -ts "${START_TS% *}" "${START_TS#* }" -i > "$d/avc-since-start.txt" 2>&1 || true
  avc_since "$OFF_START" > "$d/avc-since-start.raw" 2>/dev/null || true
  ausearch -m AVC,USER_AVC -ts "${START_TS% *}" "${START_TS#* }" 2>/dev/null | audit2allow -m pagespeed_rehearsal > "$d/audit2allow.te" 2>&1 || true
  semodule -l 2>/dev/null | grep -i pagespeed > "$d/semodule-pagespeed.txt" || echo "(no pagespeed module loaded)" > "$d/semodule-pagespeed.txt"
  journalctl -u "$DAEMON_UNIT" --no-pager -n 200 > "$d/daemon-journal.log" 2>&1 || true
  $WEB_CTL -t -D DUMP_INCLUDES > "$d/httpd-dump-includes.txt" 2>&1 || true
  $WEB_CTL -M > "$d/httpd-modules.txt" 2>&1 || true
  tail -200 "$ERROR_LOG" > "$d/httpd-error-log-tail.txt" 2>/dev/null || true
  {
    echo "--- processes ---"; ps -eo pid,user,comm,label 2>/dev/null | grep -E 'httpd|pagespeed|PID' || true
    echo "--- paths ---"
    ls -laZd "$DAEMON_BIN" "$DAEMON_RUN" "$DAEMON_RUN"/* "$DAEMON_CACHE" "$DAEMON_CACHE"/* \
             "$MODULE_CACHE" "$MODULE_LOG" "$DOCROOT$FIXTURE_URL_DIR" 2>/dev/null || true
    echo "--- fcontext rules mentioning pagespeed ---"; semanage fcontext -l 2>/dev/null | grep -i pagespeed || true
    echo "--- httpd booleans ---"; getsebool -a 2>/dev/null | grep -E '^httpd_' || true
  } > "$d/contexts.txt"
  rpm -qa 'mod-pagespeed*' 'pagespeed-optimizer*' > "$d/rpm-installed.txt" 2>&1 || true
  if [[ "$full" -eq 1 ]]; then
    if command -v sealert >/dev/null 2>&1; then
      sealert -a "$AUDIT_LOG" > "$d/sealert.txt" 2>&1 || true
    else
      echo "sealert not installed (dnf install setroubleshoot-server to get it)" > "$d/sealert.txt"
    fi
    journalctl -k --no-pager -n 300 > "$d/kernel-journal-tail.log" 2>&1 || true
    dnf history list --reverse 2>/dev/null | tail -10 > "$d/dnf-history.txt" || true
    cp -f "$MODULE_CONF" "$d/pagespeed.conf.after" 2>/dev/null || true
    cp -f "$DROPIN_DIR/$DAEMON_DROPIN" "$d/$DAEMON_DROPIN.after" 2>/dev/null || true
  fi
}

on_exit() {
  local rc=$?
  restore_dontaudit
  exit "$rc"
}
trap on_exit EXIT

# ---------------------------------------------------------------------------
# 1. Fixture
# ---------------------------------------------------------------------------
step "fixture: a page with an image and a stylesheet"
[[ -f "$PAYLOAD/$IMAGE_NAME" ]] || die "fixture image missing: $PAYLOAD/$IMAGE_NAME (the runner stages install/mod_pagespeed_example/images/$IMAGE_NAME)"
mkdir -p "$DOCROOT$FIXTURE_URL_DIR"
# cp, not mv: files created inside the docroot inherit httpd_sys_content_t;
# restorecon afterwards makes that explicit.
cp -f "$PAYLOAD/$IMAGE_NAME" "$DOCROOT$FIXTURE_URL_DIR/$IMAGE_NAME"
cp -f "$PAYLOAD/$IMAGE_NAME" "$DOCROOT$FIXTURE_URL_DIR/$IMAGE_AFTER"
ORIGIN_IMAGE_BYTES="$(stat -c %s "$DOCROOT$FIXTURE_URL_DIR/$IMAGE_NAME")"
cat > "$DOCROOT$FIXTURE_URL_DIR/style.css" <<'CSS'
/* upgrade rehearsal stylesheet: whitespace and a comment for the minifier */
body {
    margin:  0;
    padding: 0;
}
.puzzle {
    display: block;
}
CSS
cat > "$DOCROOT$FIXTURE_URL_DIR/index.html" <<HTML
<!DOCTYPE html>
<html>
<head>
  <title>upgrade rehearsal</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <p>upgrade rehearsal fixture (enforcing SELinux)</p>
  <img class="puzzle" src="${IMAGE_NAME}" alt="puzzle">
</body>
</html>
HTML
chmod -R a+rX "$DOCROOT$FIXTURE_URL_DIR"
restorecon -R "$DOCROOT" >/dev/null 2>&1 || true
note "fixture labels: $(label_of "$DOCROOT$FIXTURE_URL_DIR/$IMAGE_NAME")"

# ---------------------------------------------------------------------------
# 2. Baseline: 1.15 like a customer, on an enforcing host
# ---------------------------------------------------------------------------
step "installing mod_pagespeed ${BASELINE_PREFIX} from the public repository (install.sh + dnf)"
curl -fsSL "$INSTALL_SH_URL" | sh >"$ARTIFACTS/install-sh.log" 2>&1 \
  || { tail -20 "$ARTIFACTS/install-sh.log"; die "install.sh failed"; }
dnf install -y -q mod-pagespeed >"$ARTIFACTS/install-baseline.log" 2>&1 \
  || { tail -30 "$ARTIFACTS/install-baseline.log"; die "dnf install mod-pagespeed failed"; }
grep -E '^baseurl' /etc/yum.repos.d/modpagespeed.repo 2>/dev/null | head -1 | sed 's/^/  yum baseurl: /' || true
BASELINE_VERSION="$(pkg_version mod-pagespeed)"
case "$BASELINE_VERSION" in
  ${BASELINE_PREFIX}*) pass "baseline package installed: mod-pagespeed ${BASELINE_VERSION}" ;;
  *) fail "baseline package version '${BASELINE_VERSION}' does not start with ${BASELINE_PREFIX}" ;;
esac
if getent group pagespeed >/dev/null 2>&1; then
  note "group 'pagespeed' already exists before the upgrade (unexpected on a 1.15 host)"
else
  pass "no 'pagespeed' group before the upgrade (the optimizer package creates it)"
fi
note "module cache label as packaged: $(label_of "$MODULE_CACHE")   log dir: $(label_of "$MODULE_LOG")"

step "serving the fixture and customizing the ${BASELINE_PREFIX} configuration"
cat > "$DROPIN_DIR/upgrade-fixture.conf" <<EOF
<Directory ${DOCROOT}${FIXTURE_URL_DIR}>
    Header set Cache-Control "public, max-age=3600"
</Directory>
EOF
cat >> "$MODULE_CONF" <<'EOF'

# --- upgrade-rehearsal operator customization (must survive the upgrade) ---
<IfModule pagespeed_module>
    ModPagespeedEnableFilters collapse_whitespace,remove_comments
    ModPagespeedEnableFilters in_place_optimize_for_browser
    ModPagespeedFileCacheSizeKb 204800
    ModPagespeedStatisticsLogging on
    ModPagespeedImageRecompressionQuality 75
</IfModule>
EOF
cp "$MODULE_CONF" /root/pagespeed.conf.before-upgrade

if [[ "$BASELINE_SELINUX" == rw-content ]]; then
  step "baseline SELinux: labelling the module's own cache and log as web-server writable content"
  # The product docs' "SELinux blocks mod_pagespeed" fix, in the form that
  # actually works on EL9: a persistent fcontext rule (chcon does not survive
  # a relabel), a WRITABLE type (httpd_sys_content_t is read-only for
  # httpd_t unless the httpd_unified boolean is on), and the paths the rpm
  # really uses. Without this the module cannot write its own cache under
  # enforcing, and every daemon-path denial after the upgrade would hide
  # behind that.
  semanage fcontext -a -t httpd_sys_rw_content_t "${MODULE_CACHE}(/.*)?" 2>/dev/null \
    || semanage fcontext -m -t httpd_sys_rw_content_t "${MODULE_CACHE}(/.*)?"
  semanage fcontext -a -t httpd_log_t "${MODULE_LOG}(/.*)?" 2>/dev/null \
    || semanage fcontext -m -t httpd_log_t "${MODULE_LOG}(/.*)?"
  mkdir -p "$MODULE_CACHE" "$MODULE_LOG"
  restorecon -R "$MODULE_CACHE" "$MODULE_LOG"
  note "module cache label now: $(label_of "$MODULE_CACHE")   log dir: $(label_of "$MODULE_LOG")"
else
  note "baseline SELinux: default labels kept (--baseline-selinux none)"
fi

systemctl enable -q httpd
if configtest; then
  pass "baseline configtest: Syntax OK with the customized conffile"
else
  fail "baseline configtest failed"; $WEB_CTL -t || true
fi
restart_web || die "web server did not come up after the baseline install"
HTTPD_CHILD="$(pgrep -u "$WEB_USER" -x "$WEB_PROC" | head -1 || true)"
HTTPD_DOMAIN="$(domain_of_pid "$HTTPD_CHILD")"
case "$HTTPD_DOMAIN" in
  *:httpd_t:*) pass "web-server child runs confined as httpd_t (${HTTPD_DOMAIN})" ;;
  *) fail "web-server child is NOT httpd_t (${HTTPD_DOMAIN:-no child}): a run in another domain proves nothing about the policy" ;;
esac

step "proving ${BASELINE_PREFIX} optimizes (enforcing)"
BASE_URL="http://localhost${FIXTURE_URL_DIR}"
SIZE_CMD="curl -s -o /dev/null -w %{size_download} '${BASE_URL}/${IMAGE_NAME}'"
REWRITE_CMD="curl -s -H 'Cache-Control: no-cache' '${BASE_URL}/index.html' | grep -c 'pagespeed\\.'"
HDR="$(version_header "$BASE_URL/index.html")"
case "$HDR" in
  *"${BASELINE_PREFIX}"*) pass "baseline version header: ${HDR}" ;;
  *) fail "baseline version header missing or wrong: '${HDR}'" ;;
esac
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
BASELINE_AVCS="$(avc_count "$OFF_START" 'httpd')"
if [[ "$BASELINE_AVCS" == 0 ]]; then
  pass "no AVC denials for comm=httpd during the ${BASELINE_PREFIX} baseline"
else
  # The 1.15 product's own SELinux story (documented fix or not): a finding
  # for the record, kept apart from the gate, which is about the 1.16 pair.
  warn "${BASELINE_AVCS} AVC denial(s) for comm=httpd during the ${BASELINE_PREFIX} baseline (not the gate's subject; see artifacts/avc-baseline.txt)"
  avc_since "$OFF_START" 'httpd' | avc_summary | head -10
  avc_since "$OFF_START" 'httpd' | avc_classes
fi
avc_since "$OFF_START" > "$ARTIFACTS/avc-baseline.raw" 2>/dev/null || true
ausearch -m AVC,USER_AVC -ts "${START_TS% *}" "${START_TS#* }" -i > "$ARTIFACTS/avc-baseline.txt" 2>&1 || true

# Everything from here on is the upgrade's doing.
OFF_UPGRADE="$(audit_offset)"
UPGRADE_TS="$(date '+%m/%d/%Y %H:%M:%S')"

# ---------------------------------------------------------------------------
# 3. Upgrade in place to the 1.16 pair
# ---------------------------------------------------------------------------
if [[ -n "$PKGS_DIR" ]]; then
  step "upgrading in place from the staged rpms (both packages in one transaction)"
  dnf install -y "${OPT_RPMS[0]}" "${MOD_RPMS[0]}" >"$ARTIFACTS/upgrade.log" 2>&1 \
    || { tail -40 "$ARTIFACTS/upgrade.log"; die "dnf install of the pair failed"; }
else
  step "upgrading in place from the package repository ${REPO_URL}"
  PACKAGES_URL="$REPO_URL" PACKAGES_USER="${PACKAGES_USER:-}" PACKAGES_PASS="${PACKAGES_PASS:-}" \
    sh -c "if [ -n \"\$PACKAGES_USER\" ]; then curl -fsSL -u \"\$PACKAGES_USER:\$PACKAGES_PASS\" '${REPO_URL%/staging}/install.sh' | sh; else curl -fsSL '${REPO_URL%/staging}/install.sh' | sh; fi" \
    >"$ARTIFACTS/install-sh-upgrade.log" 2>&1 \
    || { tail -20 "$ARTIFACTS/install-sh-upgrade.log"; die "install.sh (upgrade source) failed"; }
  dnf upgrade -y mod-pagespeed >"$ARTIFACTS/upgrade.log" 2>&1 \
    || { tail -40 "$ARTIFACTS/upgrade.log"; die "dnf upgrade mod-pagespeed (repository upgrade) failed"; }
fi
grep -i -E 'rpmnew|rpmsave|warning' "$ARTIFACTS/upgrade.log" | sed 's/^/  pkgmgr: /' | head -8 || true

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
note "rpm signature status: $(rpm -q --qf '%{NAME}: %{SIGPGP:pgpsig}\n' mod-pagespeed pagespeed-optimizer 2>/dev/null | tr '\n' ';')"
note "daemon binary label: $(label_of "$DAEMON_BIN")   cache dir: $(label_of "$DAEMON_CACHE")"

step "the module package's daemon drop-in: ${DROPIN_DIR}/${DAEMON_DROPIN}"
write_dropin_by_hand() {
  cat > "${DROPIN_DIR}/${DAEMON_DROPIN}" <<EOF
<IfModule pagespeed_module>
    ModPagespeedDaemonSocketPath ${DAEMON_RUN}/notify.sock
    ModPagespeedDaemonVolumePath ${DAEMON_CACHE}/cache
</IfModule>
EOF
}
DROPIN_STRICT=0
if [[ -n "$PKGS_DIR" ]] || semver_ge "$RC" "$DROPIN_SINCE"; then DROPIN_STRICT=1; fi
if [[ -f "${DROPIN_DIR}/${DAEMON_DROPIN}" ]]; then
  pass "packaged daemon drop-in present: ${DROPIN_DIR}/${DAEMON_DROPIN}"
  check "drop-in owned by package" "mod-pagespeed" "$(rpm -qf --qf '%{NAME}' "${DROPIN_DIR}/${DAEMON_DROPIN}" 2>/dev/null || true)"
  if rpm -qc mod-pagespeed 2>/dev/null | grep -qx "${DROPIN_DIR}/${DAEMON_DROPIN}"; then
    pass "drop-in is an rpm %config file"
  else
    fail "drop-in is NOT an rpm %config file: an upgrade would overwrite operator edits"
  fi
  check "drop-in socket path" "${DAEMON_RUN}/notify.sock" "$(awk '$1=="ModPagespeedDaemonSocketPath"{print $2}' "${DROPIN_DIR}/${DAEMON_DROPIN}")"
  check "drop-in volume path" "${DAEMON_CACHE}/cache" "$(awk '$1=="ModPagespeedDaemonVolumePath"{print $2}' "${DROPIN_DIR}/${DAEMON_DROPIN}")"
  $WEB_CTL -t -D DUMP_INCLUDES > "$ARTIFACTS/dump-includes.log" 2>/dev/null || true
  loader_at="$(grep -n -F -- "${LOADER_CONF}" "$ARTIFACTS/dump-includes.log" | head -1 | cut -d: -f1 || true)"
  dropin_at="$(grep -n -F -- "/${DAEMON_DROPIN}" "$ARTIFACTS/dump-includes.log" | head -1 | cut -d: -f1 || true)"
  if [[ -z "$dropin_at" || -z "$loader_at" ]]; then
    fail "the web server's include dump does not list both ${DAEMON_DROPIN} and the module loader"
  elif (( dropin_at > loader_at )); then
    pass "include dump lists ${DAEMON_DROPIN} after the module loader (the <IfModule> block is read with the module present)"
  else
    fail "include dump lists ${DAEMON_DROPIN} BEFORE the module loader: its <IfModule> block is skipped"
  fi
elif [[ "$DROPIN_STRICT" -eq 1 ]]; then
  fail "module package ${MOD_VERSION} did not install ${DROPIN_DIR}/${DAEMON_DROPIN} (packages from ${DROPIN_SINCE} on ship it); writing it by hand so the SELinux checks still run"
  write_dropin_by_hand
else
  note "module package ${MOD_VERSION} predates the packaged daemon drop-in (ships from ${DROPIN_SINCE}); writing the two directives by hand"
  write_dropin_by_hand
fi

# The moment the shipped policy could first exist on this host, used to
# split the excluded stock net_admin records (see avc_stock_net_admin).
# With a policy-carrying rpm it is the module's %post semodule -i inside
# the upgrade transaction (the first kernel policy load since OFF_UPGRADE);
# with --policy draft it is the driver's own install below. Empty when no
# policy load was recorded (dependency-free packages): every excluded
# record then counts as pre-policy.
POLICY_TS=""
if [[ "$POLICY" == none ]]; then
  POLICY_TS="$(policy_load_ts "$OFF_UPGRADE")"
fi

if [[ "$POLICY" == draft ]]; then
  step "loading the daemon's DRAFT SELinux policy module (--policy draft)"
  dnf install -y -q --setopt=install_weak_deps=False selinux-policy-devel make >"$ARTIFACTS/policy-devel-dnf.log" 2>&1 \
    || { tail -20 "$ARTIFACTS/policy-devel-dnf.log"; die "dnf install selinux-policy-devel failed"; }
  rm -rf /root/selinux-build && mkdir -p /root/selinux-build
  cp "$POLICY_SRC/pagespeed-optimizer.te" "$POLICY_SRC/pagespeed-optimizer.fc" /root/selinux-build/
  if ( cd /root/selinux-build && make -f /usr/share/selinux/devel/Makefile pagespeed-optimizer.pp ) >"$ARTIFACTS/policy-build.log" 2>&1; then
    pass "draft policy compiled: pagespeed-optimizer.pp"
  else
    tail -30 "$ARTIFACTS/policy-build.log"; fail "draft policy did not compile (see artifacts/policy-build.log)"
  fi
  if [[ -f /root/selinux-build/pagespeed-optimizer.pp ]] && semodule -i /root/selinux-build/pagespeed-optimizer.pp >"$ARTIFACTS/policy-install.log" 2>&1; then
    pass "draft policy installed: $(semodule -l | grep -i pagespeed | tr '\n' ' ')"
    POLICY_TS="$(date +%s)"
    restorecon -Rv "$DAEMON_BIN" /var/cache/pagespeed-optimizer "$DAEMON_RUN" > "$ARTIFACTS/policy-restorecon.log" 2>&1 || true
    note "after restorecon: binary $(label_of "$DAEMON_BIN")  cache $(label_of "$DAEMON_CACHE")  run $(label_of "$DAEMON_RUN")"
    systemctl restart "$DAEMON_UNIT" || fail "daemon did not restart under the draft policy"
    sleep 2
    DAEMON_PID="$(systemctl show -p MainPID --value "$DAEMON_UNIT" || true)"
    DAEMON_DOMAIN="$(domain_of_pid "$DAEMON_PID")"
    case "$DAEMON_DOMAIN" in
      *:pagespeed_t:*) pass "daemon runs confined as pagespeed_t (${DAEMON_DOMAIN})" ;;
      *) fail "daemon is not in pagespeed_t under the draft policy (${DAEMON_DOMAIN:-not running}): the file context or init_daemon_domain did not take" ;;
    esac
  else
    tail -20 "$ARTIFACTS/policy-install.log" 2>/dev/null || true
    fail "draft policy not installed; continuing with the shipped (absent) policy"
  fi
fi

step "enabling the daemon's management API socket and restarting the daemon"
# Same one-line enablement the container rehearsal uses (documented in
# /etc/default/pagespeed-optimizer): the notification counter is the proof
# that the module reaches the socket.
test -f /etc/default/pagespeed-optimizer || install -m 0640 -o root -g pagespeed /dev/null /etc/default/pagespeed-optimizer
if grep -q '^OPTIMIZER_OPTS=' /etc/default/pagespeed-optimizer; then
  sed -i 's/^OPTIMIZER_OPTS=.*/OPTIMIZER_OPTS=--api-socket/' /etc/default/pagespeed-optimizer
else
  echo 'OPTIMIZER_OPTS=--api-socket' >> /etc/default/pagespeed-optimizer
fi
systemctl restart "$DAEMON_UNIT" || fail "systemctl restart ${DAEMON_UNIT} failed"
api_ok=""
for _ in $(seq 1 30); do
  if [[ -S "${DAEMON_RUN}/api.sock" ]]; then api_ok=1; break; fi
  sleep 1
done
if [[ -n "$api_ok" ]]; then
  note "daemon management API socket is up at ${DAEMON_RUN}/api.sock ($(label_of "${DAEMON_RUN}/api.sock"))"
else
  fail "daemon management API socket did not appear: the notification proof cannot run (journal in artifacts)"
fi

LOG_OFFSET="$(stat -c %s "$ERROR_LOG" 2>/dev/null || echo 0)"
if configtest; then
  pass "post-upgrade configtest: Syntax OK (1.15 conffile + daemon drop-in accepted by ${RC})"
else
  fail "post-upgrade configtest failed"; $WEB_CTL -t || true
fi
restart_web || fail "web server did not come back after the upgrade restart"

# ---------------------------------------------------------------------------
# 4. Assertions
# ---------------------------------------------------------------------------
step "daemon identity, group membership, filesystem modes and labels"
check "daemon unit active" "active" "$(systemctl is-active "$DAEMON_UNIT" || true)"
DAEMON_PID="$(systemctl show -p MainPID --value "$DAEMON_UNIT" || true)"
check "daemon runs as user pagespeed" "pagespeed" "$(ps -o user= -p "${DAEMON_PID:-0}" 2>/dev/null | tr -d ' ' || true)"
DAEMON_DOMAIN="$(domain_of_pid "$DAEMON_PID")"
note "daemon SELinux domain: ${DAEMON_DOMAIN:-?} (expected: unconfined_service_t with no policy, pagespeed_t with the draft)"
groups_now="$(id -nG "$WEB_USER" || true)"
case " $groups_now " in
  *" pagespeed "*) pass "web-server user ${WEB_USER} is in group pagespeed (groups: ${groups_now})" ;;
  *) fail "web-server user ${WEB_USER} is NOT in group pagespeed (groups: ${groups_now})" ;;
esac
check "cache dir ${DAEMON_CACHE} mode" "3770" "$(stat -c %a "$DAEMON_CACHE" 2>/dev/null || true)"
check "notify socket mode" "660" "$(stat -c %a "${DAEMON_RUN}/notify.sock" 2>/dev/null || true)"
note "labels: volume dir $(label_of "$DAEMON_CACHE")  notify.sock $(label_of "${DAEMON_RUN}/notify.sock")"
volumes="$(ls -1 "${DAEMON_CACHE}"/cache-* 2>/dev/null || true)"
check "exactly one cache volume file" "1" "$(printf '%s\n' "$volumes" | grep -c . || true)"
for v in $volumes; do note "volume $(basename "$v"): mode $(stat -c %a "$v") label $(label_of "$v")"; done
pagespeed_gid="$(getent group pagespeed | cut -d: -f3 || true)"
HTTPD_CHILD="$(pgrep -u "$WEB_USER" -x "$WEB_PROC" | head -1 || true)"
child_groups="$( [[ -n "$HTTPD_CHILD" ]] && grep -E '^Groups:' "/proc/$HTTPD_CHILD/status" || true)"
case " $(printf '%s' "$child_groups" | sed -E 's/^Groups:[[:space:]]*//') " in
  *" ${pagespeed_gid} "*) pass "running web-server child carries gid ${pagespeed_gid} (pagespeed)" ;;
  *) fail "running web-server child does not carry gid ${pagespeed_gid} (pagespeed): ${child_groups:-no child found}" ;;
esac
HTTPD_DOMAIN="$(domain_of_pid "$HTTPD_CHILD")"
case "$HTTPD_DOMAIN" in
  *:httpd_t:*) pass "web-server child still runs confined as httpd_t after the upgrade" ;;
  *) fail "web-server child is NOT httpd_t after the upgrade (${HTTPD_DOMAIN:-no child})" ;;
esac

step "configuration compatibility"
if cmp -s /root/pagespeed.conf.before-upgrade "$MODULE_CONF"; then
  pass "1.15 conffile ${MODULE_CONF} is byte-identical after the upgrade"
else
  fail "1.15 conffile ${MODULE_CONF} was changed by the upgrade"
  diff /root/pagespeed.conf.before-upgrade "$MODULE_CONF" | head -20 || true
fi
ls "${MODULE_CONF}.rpmnew" 2>/dev/null | sed 's/^/  packaged new conffile kept aside as: /' || true
if $WEB_CTL -M 2>/dev/null | grep -q pagespeed_module; then
  pass "pagespeed_module is loaded"
else
  fail "pagespeed_module is not loaded after the upgrade"
fi

step "version header and the daemon-startup log signatures"
HDR="$(version_header "$BASE_URL/index.html")"
case "$HDR" in
  *"${RC}"*) pass "post-upgrade version header: ${HDR}" ;;
  *) fail "post-upgrade version header does not carry ${RC}: '${HDR}'" ;;
esac
for _ in $(seq 1 12); do
  curl -s -o /dev/null "${BASE_URL}/index.html"; curl -s -o /dev/null "${BASE_URL}/${IMAGE_NAME}"
  curl -s -o /dev/null "${BASE_URL}/${IMAGE_AFTER}"; curl -s -o /dev/null "${BASE_URL}/style.css"
  sleep 1
done
tail -c +$((LOG_OFFSET + 1)) "$ERROR_LOG" > "$ARTIFACTS/error-log-after-upgrade.log" 2>/dev/null || true
SIG_RE='nothing will be recorded for in-place optimization|cannot open the optimizer daemon.s cache volume|not being in the .pagespeed. group|Giving up: in-place optimization stays off'
sig_hits="$(grep -c -E "$SIG_RE" "$ARTIFACTS/error-log-after-upgrade.log" || true)"
check "daemon-startup signature lines in the error log after the upgrade restart" "0" "$sig_hits"
if [[ "$sig_hits" != "0" ]]; then grep -E "$SIG_RE" "$ARTIFACTS/error-log-after-upgrade.log" | head -5 | sed 's/^/  /'; fi
perm_hits="$(grep -c -i -E 'permission denied.*(pagespeed-optimizer|notify\.sock|shared config)|cache_dir_generation' "$ARTIFACTS/error-log-after-upgrade.log" || true)"
check "daemon permission/handshake complaints in the error log" "0" "$perm_hits"
if [[ "$perm_hits" != "0" ]]; then
  grep -i -E 'permission denied' "$ARTIFACTS/error-log-after-upgrade.log" | head -3 | sed 's/^/  /'
  note "under enforcing SELinux a 'Permission denied' here with correct DAC modes/groups is the AVC, not the group hint the message gives"
fi

step "in-place optimization through the daemon (enforcing)"
ipro_proven=0
if [[ -n "$api_ok" ]]; then
  NOTIF_CMD="curl -s -o /dev/null '${BASE_URL}/index.html'; curl -s -o /dev/null '${BASE_URL}/${IMAGE_AFTER}'; curl -s --unix-socket ${DAEMON_RUN}/api.sock http://localhost/v1/stats | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"notifications\"][\"received\"])'"
  if notif="$(fetch_until '-ge 1' 90 "$NOTIF_CMD")"; then
    pass "daemon notification counter moved: notifications.received=${notif} (the module reaches the notify socket)"
    ipro_proven=1
  else
    fail "daemon notification counter did not move within 90s (notifications.received='${notif:-?}'): the module does not reach the notify socket"
  fi
  curl -s --unix-socket "${DAEMON_RUN}/api.sock" http://localhost/v1/stats > "$ARTIFACTS/daemon-stats.json" 2>/dev/null || true
else
  fail "notification counter unavailable (no management API socket)"
fi
SIZE_AFTER_CMD="curl -s -o /dev/null -w %{size_download} '${BASE_URL}/${IMAGE_AFTER}'"
if size="$(fetch_until "-lt $ORIGIN_IMAGE_BYTES" 150 "$SIZE_AFTER_CMD")"; then
  pass "post-upgrade in-place optimization: ${IMAGE_AFTER} (never seen by 1.15) served at ${size} bytes (origin ${ORIGIN_IMAGE_BYTES})"
  ipro_proven=1
else
  fail "post-upgrade in-place optimization: ${IMAGE_AFTER} still ${size:-?} bytes after 150s (origin ${ORIGIN_IMAGE_BYTES})"
fi
[[ "$ipro_proven" -eq 1 ]] || fail "no proof of in-place optimization after the upgrade (the daemon-startup failure class)"

step "SELinux verdict: denials since the upgrade began"
ENFORCE_AT_END="$(getenforce 2>/dev/null || echo unknown)"
check "SELinux still Enforcing at the end" "Enforcing" "$ENFORCE_AT_END"
# The gate counts httpd denials minus the excluded stock net_admin class
# (see avc_stock_net_admin for the scoping and the reasoning); the excluded
# records are reported, split at POLICY_TS, never silently dropped.
STOCK_NA="$(avc_since "$OFF_UPGRADE" 'httpd' | avc_stock_net_admin)"
HTTPD_AVCS="$(avc_since "$OFF_UPGRADE" 'httpd' | avc_drop_stock_net_admin | grep -c . || true)"
DAEMON_AVCS="$(avc_count "$OFF_UPGRADE" 'pagespeed-optim')"
ALL_AVCS="$(avc_count "$OFF_UPGRADE")"
STOCK_NA_N=0 STOCK_NA_PRE=0 STOCK_NA_POST=0
if [[ -n "$STOCK_NA" ]]; then
  STOCK_NA_N="$(printf '%s\n' "$STOCK_NA" | grep -c . || true)"
  while read -r ts; do
    [[ -n "$ts" ]] || continue
    if [[ -z "$POLICY_TS" || "$ts" -lt "$POLICY_TS" ]]; then
      STOCK_NA_PRE=$((STOCK_NA_PRE + 1))
    else
      STOCK_NA_POST=$((STOCK_NA_POST + 1))
    fi
  done < <(printf '%s\n' "$STOCK_NA" | sed -E 's/.*msg=audit\(([0-9]+).*/\1/')
  note "excluded stock-stack net_admin denial(s): ${STOCK_NA_N} total, ${STOCK_NA_PRE} before the shipped policy could first exist, ${STOCK_NA_POST} after (httpd parent SO_SNDBUFFORCE, rehearsal 4.3, dontaudited by the distro; only visible under --disable-dontaudit)"
fi
check "AVC/USER_AVC denials for comm=httpd since the upgrade (the gate)" "0" "$HTTPD_AVCS"
check "AVC/USER_AVC denials for comm=pagespeed-optimizer since the upgrade (the gate)" "0" "$DAEMON_AVCS"
note "all AVC/USER_AVC denials since the upgrade, any process: ${ALL_AVCS}"
if [[ "$HTTPD_AVCS" != 0 || "$DAEMON_AVCS" != 0 ]]; then
  echo "  --- denial groups (count x comm class {perms} target-type path) ---"
  avc_since "$OFF_UPGRADE" 'httpd|pagespeed-optim' | avc_summary | head -20
  avc_since "$OFF_UPGRADE" 'httpd|pagespeed-optim' | avc_classes
  echo "  --- the rules a policy would need (audit2allow, since the upgrade) ---"
  ausearch -m AVC,USER_AVC -ts "${UPGRADE_TS% *}" "${UPGRADE_TS#* }" 2>/dev/null | audit2allow 2>/dev/null | sed 's/^/  /' | head -40 || true
fi
avc_since "$OFF_UPGRADE" > "$ARTIFACTS/avc-since-upgrade.raw" 2>/dev/null || true
ausearch -m AVC,USER_AVC -ts "${UPGRADE_TS% *}" "${UPGRADE_TS#* }" -i > "$ARTIFACTS/avc-since-upgrade.txt" 2>&1 || true
ausearch -m AVC,USER_AVC -ts "${UPGRADE_TS% *}" "${UPGRADE_TS#* }" 2>/dev/null | audit2allow -m pagespeed_rehearsal > "$ARTIFACTS/audit2allow-since-upgrade.te" 2>&1 || true

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
collect_diagnostics "$( [[ "$FAILS" -gt 0 ]] && echo 1 || echo 0 )"
echo
echo "=== el9-selinux (${POLICY} policy, baseline-selinux ${BASELINE_SELINUX}): ${PASSES} passed, ${FAILS} failed, ${WARNS} warning(s) -- ${BASELINE_VERSION} -> ${MOD_VERSION} + optimizer ${OPT_VERSION}"
echo "    SELinux: start=${ENFORCE_AT_START} end=${ENFORCE_AT_END}; AVCs since upgrade: httpd=${HTTPD_AVCS} daemon=${DAEMON_AVCS} all=${ALL_AVCS}; baseline httpd AVCs=${BASELINE_AVCS}; stock net_admin excluded=${STOCK_NA_N} (pre-policy ${STOCK_NA_PRE}, post-policy ${STOCK_NA_POST})"
echo "    artifacts: ${ARTIFACTS}"
if [[ "$FAILS" -gt 0 ]]; then
  echo "--- error log after the upgrade restart (tail) ---"; tail -20 "$ARTIFACTS/error-log-after-upgrade.log" 2>/dev/null || true
  echo "--- daemon journal (tail) ---"; tail -20 "$ARTIFACTS/daemon-journal.log" 2>/dev/null || true
  echo "::error::enforcing-SELinux upgrade rehearsal FAILED: ${FAILS} check(s) -- see FAIL lines above"
  exit 1
fi
echo "enforcing-SELinux upgrade rehearsal PASSED"
