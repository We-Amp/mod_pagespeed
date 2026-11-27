#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Package-install verification for the unprivileged optimizer daemon
# (design §9, legs 1-3 and 6).  Runs INSIDE a target-distro
# container as root; the host-side driver is daemon-install-rig.sh.
#
#   verify-daemon-install.sh --scenario fresh|legacy \
#       --binary PATH --library PATH [--version V] [--package PATH]
#       [--work DIR] [--package-out DIR]
#
# Scenarios run in SEPARATE containers on purpose: "the install created the
# user, the group and the versioned cache directory" is only a real assertion
# on a machine where they did not exist yet, and a fresh install in a
# container that already ran one asserts nothing.
#
#   fresh   Install onto a clean system, assert the identity + layout the
#           package is supposed to establish, assert the cold-start notice
#           does NOT fire (the legacy scenario's positive is worthless
#           without this negative), then start the daemon under the unit's
#           own identity and assert process identity, secret hygiene, modes
#           and peer access -- the latter both as raw kernel
#           permissions and through the shipped client library, which is
#           the entry point a serving module actually calls.
#   legacy  Plant a root-owned pre-2.1 cache at /var/lib/pagespeed-optimizer
#           FIRST, together with an ENABLED Apache file still naming the
#           old paths (plus a stale operator backup in conf-available/
#           that the web server never loads), then install: the cold-start
#           ACTION REQUIRED notice must fire -- earned by the enabled
#           file -- the legacy tree must come out byte-, inode- and
#           ownership-identical (never chowned, never migrated, never
#           deleted), and the new versioned directory must still be created
#           and owned by the daemon user.  Then repoint the enabled file
#           (the backup stays) and reinstall the same package over the
#           result with /etc/default edited and at 0644 root:root: the
#           ACTION REQUIRED half must stay silent -- backup-only old
#           paths never earn it -- while the upgrade path re-pins BOTH
#           env files to 0640 root:pagespeed and keeps the operator edit.
#
# Two deliberate emulations, both because a container has no PID 1 systemd
# (the booted-container legs live in verify-daemon-systemd.sh):
#
#   1. The maintainer scripts gate `systemd-tmpfiles --create` on
#      [ -d /run/systemd/system ], so in this container the packaged
#      tmpfiles.d drop-in is installed but never applied.  This script runs
#      the SAME command the maintainer script would have run, and says so.
#      What is asserted is the drop-in the package shipped, applied by the
#      distro's own systemd-tmpfiles.
#   2. The daemon is started with setpriv from the unit's OWN ExecStart and
#      EnvironmentFile lines (parsed out of the shipped unit file, never
#      retyped here), with /run/pagespeed-optimizer pre-created the way
#      RuntimeDirectory=/RuntimeDirectoryMode= would create it.  Systemd's
#      other sandboxing (ProtectSystem, PrivateTmp) is NOT emulated: it can
#      only take permissions away, so it cannot mask a mode this rig would
#      otherwise catch.
#
# Modes that tools/packaging/smoke-optimizer-modes.sh already asserts on a
# scratch directory are not re-derived here; this script asserts them where
# the packaged layout puts them and adds the OWNERSHIP half (pagespeed:
# pagespeed), which a non-packaged scratch run cannot show.
set -euo pipefail

SCENARIO=""
BINARY=""
LIBRARY=""
VERSION="0.0.0~rig"
PACKAGE=""
WORK="/work"
PACKAGE_OUT=""
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# Fixture secrets: the two the daemon has -- the management-API bearer token
# and the PURGE token for the .mgmt socket, both read from the environment.
# They are planted in the env file precisely so their ABSENCE from
# /proc/<pid>/cmdline can be asserted while their PRESENCE in
# /proc/<pid>/environ proves they were really delivered to the process --
# without that second half, the cmdline assertion would pass on a daemon
# that never saw a secret at all.
RIG_API_TOKEN="rig-api-token-1d1f6a2c9b"
RIG_PURGE_TOKEN="rig-purge-token-7c4e08b3af"

usage() { sed -n '4,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --library) LIBRARY="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --package) PACKAGE="$2"; shift 2 ;;
    --package-out) PACKAGE_OUT="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[[ "$SCENARIO" == "fresh" || "$SCENARIO" == "legacy" ]] || usage
[[ -n "$BINARY" && -n "$LIBRARY" ]] || usage
[[ "$(id -u)" -eq 0 ]] || { echo "error: must run as root inside the container" >&2; exit 2; }

PKG=pagespeed-optimizer
CACHE_ROOT=/var/cache/$PKG
CACHE_DIR=$CACHE_ROOT/v3
RUN_DIR=/run/$PKG
LEGACY=/var/lib/$PKG
ENVFILE=/etc/$PKG/daemon.env
DEFAULTS=/etc/default/$PKG
mkdir -p "$WORK"

fails=0
LEG_NAME=""
LEG_NOTE=""
LEG_FAILS=0

check() { # label expected actual
  if [[ "$2" == "$3" ]]; then
    echo "ok: $1"
  else
    echo "FAIL: $1 -- expected [$2], got [$3]" >&2
    fails=$((fails + 1))
  fi
}
# One machine-readable line per leg, emitted the moment the leg finishes, so
# the driver's summary reports each leg's own verdict rather than the whole
# phase's exit code.
leg_begin() { LEG_NAME="$1"; LEG_NOTE="$2"; LEG_FAILS="$fails"; echo "-- leg: $1"; }
leg_end() {
  local status=PASS
  [[ "$fails" -gt "$LEG_FAILS" ]] && status=FAIL
  echo "RIG-RESULT: ${LEG_NAME}|${status}|${LEG_NOTE}"
}
mode_of() { stat -c '%a' "$1"; }
owner_of() { stat -c '%U:%G' "$1"; }

# ---------------------------------------------------------------- flavor ---
if command -v dpkg-deb >/dev/null 2>&1 && [[ -f /etc/debian_version ]]; then
  FLAVOR=deb
elif command -v rpmbuild >/dev/null 2>&1 || command -v rpm >/dev/null 2>&1; then
  FLAVOR=rpm
else
  echo "error: neither dpkg-deb nor rpm found -- unsupported image" >&2; exit 2
fi
DISTRO="$(. /etc/os-release && echo "$PRETTY_NAME")"
echo "=== ${DISTRO} (${FLAVOR}) -- scenario: ${SCENARIO} ==="

# --------------------------------------------------------- build package ---
if [[ -z "$PACKAGE" ]]; then
  OUT="${PACKAGE_OUT:-$WORK/pkg}"
  mkdir -p "$OUT"
  # The build scripts carry their own self-tests over package CONTENTS and
  # maintainer-script semantics (what ships, at which mode, which lines the
  # scripts contain, which malformed inputs are refused).  None of that is
  # re-derived below -- this rig starts where they stop, at an INSTALLED
  # system -- but they need a distro that can build the flavour, which is
  # exactly this container, so run them here rather than nowhere.
  if [[ "$SCENARIO" == fresh ]]; then
    leg_begin package-self-test \
      "the flavour's own build self-test (package contents + maintainer-script semantics)"
    if "$REPO/tools/packaging/build-optimizer-$FLAVOR.sh" --self-test; then
      echo "ok: build-optimizer-$FLAVOR.sh --self-test"
    else
      echo "FAIL: build-optimizer-$FLAVOR.sh --self-test" >&2
      fails=$((fails + 1))
    fi
    leg_end
  fi
  "$REPO/tools/packaging/build-optimizer-$FLAVOR.sh" \
    --version "$VERSION" --binary "$BINARY" --library "$LIBRARY" --out "$OUT"
  if [[ "$FLAVOR" == deb ]]; then
    PACKAGE="$(find "$OUT" -name "${PKG}_${VERSION}_*.deb" | head -1)"
  else
    PACKAGE="$(find "$OUT" -name "${PKG}-${VERSION}-1.*.rpm" | head -1)"
  fi
  [[ -n "$PACKAGE" ]] || { echo "error: package build produced nothing" >&2; exit 1; }
fi
echo "package under test: $PACKAGE"

if [[ "$SCENARIO" == fresh ]]; then
  leg_begin install-fresh \
    "clean host: user+group+versioned cache dir created, no false cold-start notice"
else
  leg_begin install-over-legacy \
    "planted root-owned legacy tree: notice fires, legacy untouched, v3 created"
fi

# ------------------------------------------------------- pre-install state --
# Non-vacuity guard: every "the install created X" assertion below is only
# worth anything because X provably did not exist a moment ago.
check "pre-install: no pagespeed user" "absent" \
  "$(getent passwd pagespeed >/dev/null && echo present || echo absent)"
check "pre-install: no pagespeed group" "absent" \
  "$(getent group pagespeed >/dev/null && echo present || echo absent)"
check "pre-install: no versioned cache dir" "absent" \
  "$([[ -e "$CACHE_DIR" ]] && echo present || echo absent)"

if [[ "$SCENARIO" == legacy ]]; then
  # A pre-2.1 root-owned cache, as an in-place upgrade would find it.
  mkdir -p "$LEGACY/subdir"
  printf 'legacy volume bytes\n' > "$LEGACY/cache-6-0123456789abcdef"
  printf 'cache_path=/var/lib/pagespeed-optimizer/cache\n' > "$LEGACY/pagespeed-shared.conf"
  printf 'nested\n' > "$LEGACY/subdir/nested.dat"
  chmod 0644 "$LEGACY"/cache-6-* "$LEGACY/pagespeed-shared.conf" "$LEGACY/subdir/nested.dat"
  chown -R 0:0 "$LEGACY"
  LEGACY_BEFORE="$(cd "$LEGACY" && find . -type f | sort | xargs sha256sum)"
  LEGACY_STAT_BEFORE="$(cd "$LEGACY" && find . | sort | xargs stat -c '%n %u:%g %a %i')"

  # The repoint-warning half of the cold-start notice: an ENABLED Apache
  # file still naming the old paths, planted in the exact layout a2enconf
  # produces (a conf-enabled/ symlink to conf-available/), plus the
  # operator backup beside it that the web server never loads.  The
  # Debian Apache layout is planted on every flavour (the check scans it
  # and RHEL's httpd alike); it is the layout from the real upgrade that
  # warned on the backup alone.  The first install below asserts the
  # warning fires, EARNED by the enabled file; leg 1c repoints the
  # enabled file and asserts the leftover backup alone no longer does.
  mkdir -p /etc/apache2/conf-available /etc/apache2/conf-enabled
  printf 'ModPagespeedDaemonVolumePath /var/lib/pagespeed-optimizer/cache\n' \
    > /etc/apache2/conf-available/pagespeed-daemon.conf
  ln -s ../conf-available/pagespeed-daemon.conf \
    /etc/apache2/conf-enabled/pagespeed-daemon.conf
  cp -p /etc/apache2/conf-available/pagespeed-daemon.conf \
    /etc/apache2/conf-available/pagespeed-daemon.conf.bak-pre-rc7
fi

# --------------------------------------------------------------- install ---
INSTALL_LOG="$WORK/install-${SCENARIO}.log"
set +e
if [[ "$FLAVOR" == deb ]]; then
  dpkg -i "$PACKAGE" >"$INSTALL_LOG" 2>&1
else
  rpm -i "$PACKAGE" >"$INSTALL_LOG" 2>&1
fi
rc=$?
set -e
cat "$INSTALL_LOG"
check "package installs cleanly" "0" "$rc"
[[ "$rc" -eq 0 ]] || { echo "install failed -- nothing further is assertable" >&2; exit 1; }

# The maintainer scripts skip systemd-tmpfiles when /run/systemd/system is
# absent (no booted systemd).  Run exactly what they would have run.
if [[ ! -d /run/systemd/system ]]; then
  echo "note: no booted systemd in this container -- the maintainer script's"
  echo "note: [ -d /run/systemd/system ] gate elided its tmpfiles call; the rig"
  echo "note: runs the identical command against the drop-in the package shipped."
  systemd-tmpfiles --create $PKG.conf
fi

# ------------------------------------------------- leg 1: identity + layout --
check "install created the pagespeed user" "present" \
  "$(getent passwd pagespeed >/dev/null && echo present || echo absent)"
check "install created the pagespeed group" "present" \
  "$(getent group pagespeed >/dev/null && echo present || echo absent)"
check "pagespeed has no supplementary groups" "pagespeed" "$(id -nG pagespeed)"
check "pagespeed has a nologin shell" "1" \
  "$(getent passwd pagespeed | grep -c 'nologin$')"
check "versioned cache dir mode 3770" "3770" "$(mode_of "$CACHE_DIR")"
check "versioned cache dir owner" "pagespeed:pagespeed" "$(owner_of "$CACHE_DIR")"
check "cache root stays root-owned 0755" "root:root 755" \
  "$(owner_of "$CACHE_ROOT") $(mode_of "$CACHE_ROOT")"
check "daemon.env mode 0640" "640" "$(mode_of "$ENVFILE")"
check "daemon.env owner root:pagespeed" "root:pagespeed" "$(owner_of "$ENVFILE")"
# #1486: the /etc/default overrides file is an EnvironmentFile too; the
# env-file gate (0640 root:pagespeed) applies to it from the first install.
check "/etc/default env file mode 0640" "640" "$(mode_of "$DEFAULTS")"
check "/etc/default env file owner root:pagespeed" "root:pagespeed" \
  "$(owner_of "$DEFAULTS")"
check "no world-writable entry under the cache root" "0" \
  "$(find "$CACHE_ROOT" -perm -o+w | wc -l | tr -d ' ')"

UNIT="$(ls /lib/systemd/system/$PKG.service /usr/lib/systemd/system/$PKG.service 2>/dev/null | head -1)"
check "unit file installed" "1" "$([[ -n "$UNIT" ]] && echo 1 || echo 0)"

# The daemon statically links BSD/MIT/Zlib-licensed components whose terms
# require their notices to accompany a binary redistribution, so the three
# notice files must be on the INSTALLED system -- byte-identical to the tree
# the package was built from -- not merely present in the payload listing the
# build self-test reads.  dpkg keeps them under /usr/share/doc, rpm under
# /usr/share/licenses (%license, which even a nodocs install keeps).
if [[ "$FLAVOR" == deb ]]; then LICDIR="/usr/share/doc/$PKG"; else LICDIR="/usr/share/licenses/$PKG"; fi
for f in LICENSE NOTICE THIRD-PARTY-NOTICES; do
  check "installed $LICDIR/$f matches the tree" \
    "$(sha256sum < "$REPO/$f" | cut -d' ' -f1)" \
    "$([[ -f "$LICDIR/$f" ]] && sha256sum < "$LICDIR/$f" | cut -d' ' -f1 || echo missing)"
done

if [[ "$SCENARIO" == fresh ]]; then
  # The negative half of the cold-start notice: with nothing legacy on disk
  # the notice must stay silent, or the legacy scenario proves nothing.
  check "fresh install prints NO cold-start notice" "0" \
    "$(grep -c 'ACTION REQUIRED' "$INSTALL_LOG" || true)"
  leg_end
fi

# ------------------------------------------ leg 1b: install over legacy tree --
if [[ "$SCENARIO" == legacy ]]; then
  check "cold-start notice names the legacy path" "1" \
    "$(grep -c "pre-existing root-owned cache detected at $LEGACY" "$INSTALL_LOG")"
  check "cold-start notice says ACTION REQUIRED (enabled file still stale)" "1" \
    "$(grep -c 'ACTION REQUIRED' "$INSTALL_LOG")"
  check "cold-start notice states no migration and no chown" "1" \
    "$(grep -c 'cold-start; no content is migrated or chowned.' "$INSTALL_LOG")"
  check "legacy tree still present" "present" \
    "$([[ -d "$LEGACY" ]] && echo present || echo absent)"
  check "legacy content byte-identical" "$LEGACY_BEFORE" \
    "$(cd "$LEGACY" && find . -type f | sort | xargs sha256sum)"
  check "legacy ownership/mode/inode untouched" "$LEGACY_STAT_BEFORE" \
    "$(cd "$LEGACY" && find . | sort | xargs stat -c '%n %u:%g %a %i')"
  check "legacy tree has no pagespeed-owned entry" "0" \
    "$(find "$LEGACY" -user pagespeed | wc -l | tr -d ' ')"
  check "nothing was migrated into the versioned dir" "0" \
    "$(find "$CACHE_DIR" -mindepth 1 | wc -l | tr -d ' ')"
  leg_end

  # --------------------------------- leg 1c: upgrade re-pins env-file perms --
  # #1486, the observed rc.10 -> rc.11 defect: an upgrade REPLACES an
  # operator-unedited conffile with payload ownership (root:root), and KEEPS
  # an edited one with whatever mode it had -- either way the env files must
  # come out of the maintainer script at 0640 root:pagespeed.  Reinstall the
  # same package over the install above (a same-version reinstall takes the
  # exact code path a version-bump upgrade does: dpkg re-runs postinst
  # configure, rpm -U --force re-runs %post) with /etc/default carrying an
  # operator edit at the observed 0644 root:root, and assert both env files
  # converge on the gate -- and that the edit survives.
  leg_begin upgrade-env-file-perms \
    "reinstall over an edited 0644 root:root /etc/default: both env files end 0640 root:pagespeed, edit survives (#1486)"
  printf '# operator edit\nLOG_LEVEL=debug\n' >> "$DEFAULTS"
  chmod 0644 "$DEFAULTS"
  chown root:root "$DEFAULTS"
  # Operator repoints the live configuration to v2; the stale operator backup
  # stays behind in conf-available/.  The reinstall's notice must keep the
  # cold-start facts but DROP the ACTION REQUIRED half: a file the web
  # server does not load may never earn the repoint warning (a real
  # upgrade warned on exactly such a backup).
  printf 'ModPagespeedDaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache\n' \
    > /etc/apache2/conf-available/pagespeed-daemon.conf
  UPGRADE_LOG="$WORK/upgrade-${SCENARIO}.log"
  set +e
  if [[ "$FLAVOR" == deb ]]; then
    DEBIAN_FRONTEND=noninteractive dpkg -i "$PACKAGE" >"$UPGRADE_LOG" 2>&1
  else
    rpm -U --force "$PACKAGE" >"$UPGRADE_LOG" 2>&1
  fi
  urc=$?
  set -e
  cat "$UPGRADE_LOG"
  check "upgrade/reinstall exits cleanly" "0" "$urc"
  check "reinstall keeps the cold-start facts" "1" \
    "$(grep -c 'pre-existing root-owned cache detected' "$UPGRADE_LOG" || true)"
  check "repointed config: backup-only old paths earn no ACTION REQUIRED" "0" \
    "$(grep -c 'ACTION REQUIRED' "$UPGRADE_LOG" || true)"
  # Same no-booted-systemd emulation as after the first install: run exactly
  # what the maintainer script's tmpfiles call would have run.
  if [[ ! -d /run/systemd/system ]]; then
    systemd-tmpfiles --create $PKG.conf
  fi
  check "post-upgrade /etc/default mode 0640" "640" "$(mode_of "$DEFAULTS")"
  check "post-upgrade /etc/default owner root:pagespeed" "root:pagespeed" \
    "$(owner_of "$DEFAULTS")"
  check "post-upgrade daemon.env mode 0640" "640" "$(mode_of "$ENVFILE")"
  check "post-upgrade daemon.env owner root:pagespeed" "root:pagespeed" \
    "$(owner_of "$ENVFILE")"
  check "operator edit in /etc/default survives the upgrade" "1" \
    "$(grep -c '^LOG_LEVEL=debug$' "$DEFAULTS")"
  leg_end
  # The remaining legs need a running daemon; they run in the fresh
  # scenario's container, which is the one that mirrors a supported install.
  if [[ "$fails" -gt 0 ]]; then
    echo "verify-daemon-install: $fails FAILURE(S)" >&2; exit 1
  fi
  echo "verify-daemon-install: all checks passed"
  exit 0
fi

# ------------------------------------------------ leg 2: process identity ----
leg_begin process-identity \
  "setpriv emulation of User=/Group=; secrets off argv but present in environ"
# Plant the secrets in the env file the unit reads (0640 root:pagespeed --
# re-assert the mode after the write, the way an operator filling in the
# template must leave it).
cat > "$ENVFILE" <<EOF
PAGESPEED_API_TOKEN=$RIG_API_TOKEN
PAGESPEED_PURGE_TOKEN=$RIG_PURGE_TOKEN
EOF
chown root:pagespeed "$ENVFILE"
chmod 0640 "$ENVFILE"

# RuntimeDirectory=/RuntimeDirectoryMode= emulation.
install -d -m 0750 -o pagespeed -g pagespeed "$RUN_DIR"

# Parse the SHIPPED unit: Environment= defaults, EnvironmentFile= overrides
# (in unit order, leading '-' = optional), and the ExecStart argv with
# systemd's expansion rules ($VAR splits into words, ${VAR} does not).
python3 - "$UNIT" "$WORK/argv.nul" "$WORK/env.nul" <<'PYEOF'
import os, shlex, sys

unit_path, argv_out, env_out = sys.argv[1:4]
section = None
env = {}
env_files = []
exec_start = None
pending = ""
for raw in open(unit_path, encoding="utf-8"):
    line = raw.rstrip("\n")
    if pending:
        line = pending + " " + line.strip()
        pending = ""
    stripped = line.strip()
    if not stripped or stripped.startswith("#") or stripped.startswith(";"):
        continue
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1]
        continue
    if section != "Service":
        continue
    if stripped.endswith("\\"):
        pending = stripped[:-1].rstrip()
        continue
    key, _, value = stripped.partition("=")
    key, value = key.strip(), value.strip()
    if key == "Environment":
        for token in shlex.split(value):
            k, _, v = token.partition("=")
            env[k] = v
    elif key == "EnvironmentFile":
        env_files.append(value)
    elif key == "ExecStart":
        exec_start = value

if exec_start is None:
    sys.exit("ExecStart not found in %s" % unit_path)

for spec in env_files:
    optional = spec.startswith("-")
    path = spec.lstrip("-")
    if not os.path.exists(path):
        if optional:
            continue
        sys.exit("EnvironmentFile missing: %s" % path)
    for raw in open(path, encoding="utf-8"):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        k, _, v = s.partition("=")
        env[k.strip()] = v.strip().strip('"')

# systemd's variable expansion in ExecStart.
argv = []
for token in shlex.split(exec_start):
    if token.startswith("${") and token.endswith("}"):
        argv.append(env.get(token[2:-1], ""))
    elif token.startswith("$"):
        argv.extend(shlex.split(env.get(token[1:], "")))
    elif "$" in token:
        sys.exit("unsupported inline expansion in ExecStart token: %s" % token)
    else:
        argv.append(token)
argv = [a for a in argv if a != ""]

# What systemd itself injects for a User= unit, and nothing else: the
# process must come out of a scrubbed environment or "the secret reached
# the process" would be an artefact of the rig's own shell.
runtime = dict(env)
runtime.update({
    "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    "USER": "pagespeed", "LOGNAME": "pagespeed",
    "HOME": "/var/cache/pagespeed-optimizer", "SHELL": "/usr/sbin/nologin",
})
with open(argv_out, "wb") as fh:
    fh.write(b"\0".join(a.encode() for a in argv))
with open(env_out, "wb") as fh:
    fh.write(b"\0".join(("%s=%s" % kv).encode() for kv in runtime.items()))
print("ExecStart argv:", " ".join(argv))
PYEOF

mapfile -d '' -t ARGV < "$WORK/argv.nul"
mapfile -d '' -t ENVV < "$WORK/env.nul"
check "argv comes from the unit's own ExecStart" "/usr/bin/$PKG" "${ARGV[0]}"

SETPRIV_OPTS=(--reuid "$(id -u pagespeed)" --regid "$(id -g pagespeed)"
              --clear-groups --no-new-privs --bounding-set=-all)
if ! setpriv "${SETPRIV_OPTS[@]}" /bin/true 2>/dev/null; then
  echo "note: setpriv --bounding-set=-all unavailable here; dropping that option"
  SETPRIV_OPTS=(--reuid "$(id -u pagespeed)" --regid "$(id -g pagespeed)"
                --clear-groups --no-new-privs)
fi
setpriv "${SETPRIV_OPTS[@]}" /usr/bin/env -i "${ENVV[@]}" "${ARGV[@]}" \
  >"$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!
cleanup() { kill "$DAEMON_PID" 2>/dev/null || true; }
trap cleanup EXIT

# Wait for ALL THREE sockets this leg stats below, not just the first.  The
# daemon binds notify, health and mgmt one after the other; notify.sock
# appearing says nothing about the other two, and the files the next loop
# waits on are written before any bind, so they do not cover the gap either.
# The binds are normally microseconds apart, but on a host where bind() is
# slow the gap grows to tens of milliseconds and the stat below found
# notify.sock.mgmt (sometimes .health too) simply not there yet.  Same wait
# as smoke-optimizer-modes.sh.
DAEMON_SOCKS=(notify.sock notify.sock.health notify.sock.mgmt)
for _ in $(seq 1 100); do
  MISSING_SOCK=""
  for s in "${DAEMON_SOCKS[@]}"; do
    [[ -S "$RUN_DIR/$s" ]] || { MISSING_SOCK="$s"; break; }
  done
  [[ -z "$MISSING_SOCK" ]] && break
  sleep 0.2
done
# Name the socket that never showed up, so the reader goes after the one bind
# that lagged rather than after "the daemon".
if [[ -n "$MISSING_SOCK" ]]; then
  echo "FAIL: daemon did not create $RUN_DIR/$MISSING_SOCK within 20s; log follows" >&2
  cat "$WORK/daemon.log" >&2
  exit 1
fi
for _ in $(seq 1 100); do
  [[ -f "$CACHE_DIR/pagespeed-shared.conf" && -f "$CACHE_DIR/.pagespeed-serve-stats" ]] && break
  sleep 0.2
done

STATUS=/proc/$DAEMON_PID/status
check "daemon runs as uid pagespeed" "$(id -u pagespeed)" \
  "$(awk '/^Uid:/{print $2}' "$STATUS")"
check "daemon runs as gid pagespeed" "$(id -g pagespeed)" \
  "$(awk '/^Gid:/{print $2}' "$STATUS")"
check "daemon holds no effective capabilities" "0000000000000000" \
  "$(awk '/^CapEff:/{print $2}' "$STATUS")"
check "daemon holds no permitted capabilities" "0000000000000000" \
  "$(awk '/^CapPrm:/{print $2}' "$STATUS")"

CMDLINE="$(tr '\0' ' ' < /proc/$DAEMON_PID/cmdline)"
echo "cmdline: $CMDLINE"
check "no --api-token on the command line" "0" \
  "$(grep -c -- '--api-token' <<<"$CMDLINE" || true)"
check "no api-token VALUE on the command line" "0" \
  "$(grep -c -- "$RIG_API_TOKEN" <<<"$CMDLINE" || true)"
check "no purge-token VALUE on the command line" "0" \
  "$(grep -c -- "$RIG_PURGE_TOKEN" <<<"$CMDLINE" || true)"
# Delivery proof: the secrets DID reach the process, through the env file.
ENVIRON="$(tr '\0' '\n' < /proc/$DAEMON_PID/environ)"
check "api token reached the process via the env file" "1" \
  "$(grep -c "^PAGESPEED_API_TOKEN=$RIG_API_TOKEN$" <<<"$ENVIRON" || true)"
check "purge token reached the process via the env file" "1" \
  "$(grep -c "^PAGESPEED_PURGE_TOKEN=$RIG_PURGE_TOKEN$" <<<"$ENVIRON" || true)"

for s in "${DAEMON_SOCKS[@]}"; do
  check "$s mode 0660" "660" "$(mode_of "$RUN_DIR/$s")"
  check "$s owner pagespeed:pagespeed" "pagespeed:pagespeed" "$(owner_of "$RUN_DIR/$s")"
done
VOLUME="$(find "$CACHE_DIR" -maxdepth 1 -name 'cache-*' -type f | head -1)"
check "daemon wrote its volume into the packaged cache dir" "1" \
  "$([[ -n "$VOLUME" ]] && echo 1 || echo 0)"
check "volume mode 0660" "660" "$(mode_of "$VOLUME")"
check "volume owner pagespeed:pagespeed" "pagespeed:pagespeed" "$(owner_of "$VOLUME")"
check "shared config mode 0640" "640" "$(mode_of "$CACHE_DIR/pagespeed-shared.conf")"
check "shared config owner pagespeed:pagespeed" "pagespeed:pagespeed" \
  "$(owner_of "$CACHE_DIR/pagespeed-shared.conf")"
check "serve-stats mode 0660" "660" "$(mode_of "$CACHE_DIR/.pagespeed-serve-stats")"
check "no world-writable entry under the cache dir" "0" \
  "$(find "$CACHE_ROOT" -perm -o+w | wc -l | tr -d ' ')"
check "no world-writable entry under the runtime dir" "0" \
  "$(find "$RUN_DIR" -perm -o+w | wc -l | tr -d ' ')"
leg_end

# ---------------------------------------------------- leg 3: peer access ----
leg_begin peer-access \
  "group join grants volume+sockets; removal returns EACCES, never ENOENT"
# What this proves: group `pagespeed` -- and nothing else -- is what lets a
# web-server user mmap the volume read-write and connect the daemon's
# sockets.  The probe does exactly what the serving module does (O_RDWR +
# MAP_SHARED on the volume, connect() on the notify and mgmt sockets) and
# reports the errno CLASS, so the negative leg can distinguish "permission
# denied" (the module's remediation line: web user not in the group) from
# "absent" (the substrate really is down).  The join itself is what the
# MODULE package's postinst performs on a real host.
cat > "$WORK/peer-probe.py" <<'PYEOF'
import json, mmap, os, socket, sys

def klass(err):
    return {1: "EPERM", 2: "ENOENT", 13: "EACCES"}.get(err.errno, "errno%d" % err.errno)

volume, notify, mgmt = sys.argv[1:4]
res = {"uid": os.getuid(), "groups": sorted(os.getgroups())}
try:
    fd = os.open(volume, os.O_RDWR)
    try:
        m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        m.close()
        res["volume"] = "ok"
    finally:
        os.close(fd)
except OSError as err:
    res["volume"] = klass(err)
for name, path in (("notify", notify), ("mgmt", mgmt)):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(path)
        res[name] = "ok"
    except OSError as err:
        res[name] = klass(err)
    finally:
        sock.close()
print(json.dumps(res))
PYEOF

# Stand-in for www-data / apache / nginx: a stock, unprivileged web-server
# account that is NOT the daemon user.
useradd --system --no-create-home --shell /usr/sbin/nologin rigweb 2>/dev/null \
  || useradd --system --no-create-home --shell /sbin/nologin rigweb
probe() { runuser -u rigweb -- python3 "$WORK/peer-probe.py" \
  "$VOLUME" "$RUN_DIR/notify.sock" "$RUN_DIR/notify.sock.mgmt"; }

# Negative FIRST, before the join, so the positive cannot be an accident of
# something else on this box already granting access.
BEFORE="$(probe)"
echo "peer probe (not in group): $BEFORE"
check "non-member cannot open the volume (EACCES, not absent)" "EACCES" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["volume"])' "$BEFORE")"
check "non-member cannot connect the notify socket (EACCES)" "EACCES" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["notify"])' "$BEFORE")"
check "non-member cannot connect the mgmt socket (EACCES)" "EACCES" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["mgmt"])' "$BEFORE")"

usermod -a -G pagespeed rigweb
AFTER="$(probe)"
echo "peer probe (in group): $AFTER"
check "group member maps the volume read-write" "ok" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["volume"])' "$AFTER")"
check "group member connects the notify socket" "ok" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["notify"])' "$AFTER")"
check "group member connects the mgmt socket" "ok" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["mgmt"])' "$AFTER")"

gpasswd -d rigweb pagespeed >/dev/null
REMOVED="$(probe)"
echo "peer probe (removed from group): $REMOVED"
check "removing the group membership revokes the volume (EACCES)" "EACCES" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["volume"])' "$REMOVED")"
check "removing the group membership revokes the notify socket (EACCES)" "EACCES" \
  "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["notify"])' "$REMOVED")"
leg_end

# ------------------------------- leg 3b: peer opens through the real client --
leg_begin peer-cache-open \
  "group member opens the volume through the shipped libpagespeed.so; a non-member is refused"
# Leg 3 proves the KERNEL grants a group member the volume and the sockets.
# It does not prove the CLIENT LIBRARY the serving module actually links will
# accept that grant: its probe re-implements the peer in Python (open + mmap +
# connect), so every line of ps_cache_open -- the one entry point a module
# uses to attach to the volume -- stayed unexecuted, and a client-side refusal
# of a perfectly-permissioned volume passed this rig unnoticed.  This leg
# calls the real entry point, in the shipped .so, as the real peer identity.
#
# Both halves are asserted so neither can be vacuous: a group member must get
# PS_OK, and a NON-member must be refused.  Without the negative half a probe
# that never reached the library at all would report the same green.
cat > "$WORK/cache-open-probe.py" <<'PYEOF'
"""Open the daemon's cache volume through the SHIPPED client library.

Modes:
  volume-size LIB STEM        print the volume size the worker published
  open LIB STEM SIZE          ps_cache_open(STEM, SIZE); print a JSON verdict
"""
import ctypes, json, os, sys

PS_OK = 0
PS_ERR = {
    0: "PS_OK", 1: "PS_ERR_NOT_FOUND", 2: "PS_ERR_IO", 3: "PS_ERR_CORRUPTED",
    4: "PS_ERR_NO_SPACE", 5: "PS_ERR_INVALID_ARG", 6: "PS_ERR_BUSY",
    7: "PS_ERR_CLOSED", 8: "PS_ERR_TOO_MANY_ALTERNATES", 9: "PS_ERR_EXISTS",
    10: "PS_ERR_NOT_OWNED", 11: "PS_ERR_VERSION_MISMATCH",
    99: "PS_ERR_INTERNAL",
}


class Config(ctypes.Structure):
    # The published ps_cache_config_t prefix, plus slack.  ps_cache_open does
    # not consult struct_size and reads every field the LIBRARY defines, so
    # the buffer must be large enough for a newer library's layout -- the
    # header prescribes an over-allocated buffer as the portable spelling.
    # Field ORDER is ABI-stable: moving a field within a published struct is
    # a major bump (lib/pagespeed/ABI.md), so these offsets cannot drift
    # under us without the ABI gate saying so.
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("volume_path", ctypes.c_char_p),
        ("volume_size", ctypes.c_uint64),
        ("enable_checksum", ctypes.c_int),
        ("ram_cache_size", ctypes.c_size_t),
        ("max_metadata_size", ctypes.c_size_t),
        ("growth_slack", ctypes.c_ubyte * 256),
    ]


def load(path):
    lib = ctypes.CDLL(path)
    lib.ps_cache_config_init.restype = None
    lib.ps_cache_config_init.argtypes = [ctypes.POINTER(Config)]
    lib.ps_cache_open.restype = ctypes.c_int
    lib.ps_cache_open.argtypes = [ctypes.POINTER(Config),
                                  ctypes.POINTER(ctypes.c_void_p)]
    lib.ps_cache_close.restype = None
    lib.ps_cache_close.argtypes = [ctypes.c_void_p]
    lib.ps_last_error_message.restype = ctypes.c_char_p
    lib.ps_last_error_message.argtypes = []
    lib.ps_read_shared_config_volume_size.restype = ctypes.c_uint64
    lib.ps_read_shared_config_volume_size.argtypes = [ctypes.c_char_p]
    return lib


mode, libpath, stem = sys.argv[1:4]
lib = load(libpath)
stem_bytes = stem.encode()

if mode == "volume-size":
    # The `volume_size` key the worker published in pagespeed-shared.conf,
    # read through the library's own accessor -- the same call the serving
    # module makes, rather than a re-parse of the file this rig would then
    # be asserting against itself.  0 means "not known"; a caller that
    # substitutes a default silently opens a DIFFERENT file and shares
    # nothing, so the rig refuses to guess as well.
    print(lib.ps_read_shared_config_volume_size(stem_bytes))
    sys.exit(0)

if mode != "open":
    sys.exit("unknown probe mode: %s" % mode)

config = Config()
lib.ps_cache_config_init(ctypes.byref(config))
config.volume_path = stem_bytes
config.volume_size = int(sys.argv[4])
cache = ctypes.c_void_p()
rc = lib.ps_cache_open(ctypes.byref(config), ctypes.byref(cache))
message = lib.ps_last_error_message() or b""
if rc == PS_OK:
    lib.ps_cache_close(cache)
print(json.dumps({
    "uid": os.getuid(),
    "groups": sorted(os.getgroups()),
    "rc": rc,
    "status": PS_ERR.get(rc, "PS_ERR_%d" % rc),
    "verdict": "opened" if rc == PS_OK else "refused",
    "message": message.decode("utf-8", "replace"),
}))
PYEOF
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$1" "$2"; }

# Where the flavour's own packaging rule put the client library: the deb
# stages it in the multiarch libdir, the rpm in /usr/lib64.  Resolve it
# rather than hardcoding either, and assert there is EXACTLY one -- two
# copies on a box is its own defect, and picking one of them silently would
# hide it.
# `|| true`: /usr/lib64 does not exist on arm64 Debian (it does on x86_64,
# for the dynamic loader), so find exits 1 -- and with `set -o pipefail` in
# force that status reaches the assignment and `set -e` ends the phase right
# here, silently, before this leg prints anything. Found by the aarch64 rig
# lane; the search itself is unchanged, and a genuinely missing library is
# still caught by the count assertion below.
LIBSO_FOUND="$(find /usr/lib /usr/lib64 -maxdepth 2 -name libpagespeed.so -type f 2>/dev/null | sort -u || true)"
check "the package installed exactly one libpagespeed.so in a system libdir" "1" \
  "$(printf '%s' "$LIBSO_FOUND" | grep -c . || true)"
LIBSO="$(printf '%s\n' "$LIBSO_FOUND" | head -1)"
echo "client library under test: $LIBSO"
# The serving module dlopens the BARE SONAME and depends on the postinst's
# ldconfig having run; assert that resolves before probing through the
# resolved path (which makes a probe failure name a file).
check "the bare soname resolves through the loader cache" "ok" \
  "$(python3 -c 'import ctypes; ctypes.CDLL("libpagespeed.so"); print("ok")' 2>/dev/null \
    || echo unresolved)"

# The module is configured with the volume STEM, not the resolved
# cache-<generation>-<geometry> file: the library derives the filename from
# the stem plus the geometry, which is why the size below must be the
# daemon's own and not a plausible-looking default.
STEM="$CACHE_DIR/cache"
VOLUME_SIZE="$(python3 "$WORK/cache-open-probe.py" \
  volume-size "$LIBSO" "$STEM" 2>/dev/null || echo 0)"
echo "volume_size published by the daemon: $VOLUME_SIZE"
check "the daemon published a volume size a peer can match" "1" \
  "$([[ "$VOLUME_SIZE" =~ ^[1-9][0-9]*$ ]] && echo 1 || echo 0)"

volume_files() { find "$CACHE_DIR" -maxdepth 1 -name 'cache-*' -type f | sort; }
if [[ "$VOLUME_SIZE" =~ ^[1-9][0-9]*$ ]]; then
  VOLUMES_BEFORE="$(volume_files)"
  cache_open() { runuser -u rigweb -- python3 "$WORK/cache-open-probe.py" \
    open "$LIBSO" "$STEM" "$VOLUME_SIZE"; }

  # Negative control FIRST.  Leg 3 left rigweb outside the group; assert that
  # rather than assume it, then require the library to refuse -- this is what
  # makes the positive below a live assertion instead of a call that could
  # have returned PS_OK without ever touching the volume.
  check "rigweb enters this leg outside the group" "0" \
    "$(id -nG rigweb | tr ' ' '\n' | grep -cx pagespeed || true)"
  DENIED="$(cache_open)"
  echo "ps_cache_open (not in group): $DENIED"
  check "a non-member is REFUSED by ps_cache_open" "refused" "$(field "$DENIED" verdict)"

  usermod -a -G pagespeed rigweb
  GRANTED="$(cache_open)"
  echo "ps_cache_open (in group): $GRANTED"
  GRANTED_STATUS="$(field "$GRANTED" status)"
  if [[ "$GRANTED_STATUS" != PS_OK ]]; then
    echo "ps_last_error_message(): $(field "$GRANTED" message)" >&2
    echo "note: the volume is $(owner_of "$VOLUME") mode $(mode_of "$VOLUME") and the" >&2
    echo "note: peer IS in group pagespeed, so this is the CLIENT refusing a" >&2
    echo "note: volume the kernel already granted it." >&2
  fi
  check "a group member opens the volume through ps_cache_open" "PS_OK" "$GRANTED_STATUS"

  # The peer must have attached to the DAEMON'S volume.  A geometry mismatch
  # does not fail: it creates a second file and runs a permanently cold cache
  # on both sides with no error anywhere, so "PS_OK" alone would be a green
  # light on exactly the outcome this leg exists to catch.
  check "the peer attached to the daemon's volume, creating no second one" \
    "$VOLUMES_BEFORE" "$(volume_files)"
  check "the volume is still the daemon's, at the mode it was granted at" \
    "pagespeed:pagespeed 660" "$(owner_of "$VOLUME") $(mode_of "$VOLUME")"

  gpasswd -d rigweb pagespeed >/dev/null
else
  echo "FAIL: no usable volume size -- opening with a guessed geometry would" >&2
  echo "FAIL: create a second volume rather than assert anything" >&2
  fails=$((fails + 1))
fi
leg_end

kill "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
trap - EXIT

if [[ "$fails" -gt 0 ]]; then
  echo "verify-daemon-install: $fails FAILURE(S)" >&2
  exit 1
fi
echo "verify-daemon-install: all checks passed"
exit 0
