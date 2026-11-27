#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the pagespeed-optimizer deb: the factory worker as a host package
# (systemd-supervised, UDS socket, EnvironmentFile-tunable), versioned so a
# serving-module package can declare a hard versioned dependency on it.
#
#   build-optimizer-deb.sh --version VERSION [--binary PATH] [--out DIR]
#                          [--library PATH] [--expect-version STR]
#                          [--glibc-floor X.Y]
#   build-optimizer-deb.sh --self-test
#
# The script does NOT run bazel: the binary is an input (default
# bazel-bin/src/worker/factory_worker), which keeps the packaging layout
# testable without a build (--self-test uses a stub) and keeps "which
# binary went in" an explicit, auditable choice of the caller.
#
# The serving module binds the daemon through the C API client library
# (dlopen of the bare soname libpagespeed.so, no path override), so the
# package ships it too: --library (default
# bazel-bin/lib/pagespeed/libpagespeed.so) lands in the multiarch libdir
# and the maintainer scripts run ldconfig. The glibc Depends floor is the
# max across BOTH ELF objects.
#
# The binary static-links its C++ runtime by design (see the cc_binary's
# linkopts), so the only runtime dependency is glibc. The Depends floor is
# DERIVED from the binary's GLIBC_* dynamic-symbol versions, never declared
# by hand; --glibc-floor exists only for non-ELF stand-ins (the self-test
# stub) and REFUSES to contradict a derivable floor. A GA lane should still
# consider dpkg-shlibdeps.
#
# --expect-version STR: when the binary executes on this host, its
# --version output must contain STR (typically the pinned source revision)
# or the build refuses. Passing it for a binary that cannot execute here is
# refused too: an unverifiable claim is not a tripwire.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PKG=pagespeed-optimizer

multiarch_triplet() { # deb architecture -> multiarch libdir triplet
  case "$1" in
    amd64) echo x86_64-linux-gnu ;;
    arm64) echo aarch64-linux-gnu ;;
    *) echo "error: no multiarch triplet mapping for architecture '$1'" >&2
       return 1 ;;
  esac
}

usage() {
  sed -n '4,35p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 2
}

build_deb() {
  local version="$1" binary="$2" outdir="$3"
  local expect="${4:-}" floor_opt="${5:-}" library="${6:-}"
  [[ -n "$version" ]] || { echo "error: --version is required" >&2; exit 2; }
  # Debian version grammar minus epochs: this package's trains never use
  # them, and an epoch silently changes ordering -- so ':' is refused.
  case "$version" in
    *[!A-Za-z0-9.+~-]*) echo "error: version '$version' has characters" \
      "outside [A-Za-z0-9.+~-]" >&2; exit 2 ;;
  esac
  [[ -f "$binary" ]] || { echo "error: binary not found: $binary" \
    "(build //src/worker:factory_worker first, or pass --binary)" >&2; exit 1; }
  [[ -f "$library" ]] || { echo "error: client library not found: $library" \
    "(build //lib/pagespeed:libpagespeed.so first, or pass --library)" >&2
    exit 1; }

  # Runtime Depends floor: derived from the ELF objects that ship (daemon
  # binary AND client library, max across both), so the control file can
  # never promise a host the payload will not load on. Non-ELF stand-ins
  # (self-test stubs) fall back to --glibc-floor, which still refuses to
  # contradict any derivable floor.
  local glibc_floor="" derived="" obj f
  for obj in "$binary" "$library"; do
    if [[ "$(head -c4 "$obj" | od -An -tx1 | tr -d ' \n')" == "7f454c46" ]]; then
      f="$( { readelf --dyn-syms -W "$obj" 2>/dev/null || true; } \
        | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -u -V | tail -1)"
      [[ -n "$f" ]] || { echo "error: cannot derive a glibc floor" \
        "from ELF object '$obj' (no GLIBC_* dynamic symbols)" >&2; exit 1; }
      derived="$(printf '%s\n%s\n' "$derived" "$f" | sed '/^$/d' \
        | sort -V | tail -1)"
    fi
  done
  if [[ -n "$derived" ]]; then
    glibc_floor="$derived"
    if [[ -n "$floor_opt" && "$floor_opt" != "$glibc_floor" ]]; then
      echo "error: --glibc-floor $floor_opt contradicts the derived floor" \
        "$glibc_floor" >&2; exit 1
    fi
  else
    glibc_floor="$floor_opt"
    [[ -n "$glibc_floor" ]] || { echo "error: neither the binary nor the" \
      "library is ELF; a glibc floor cannot be derived -- pass" \
      "--glibc-floor" >&2; exit 1; }
  fi
  case "$glibc_floor" in
    *[!0-9.]*) echo "error: glibc floor '$glibc_floor' is not a version" >&2
      exit 2 ;;
  esac
  echo "glibc floor: $glibc_floor"
  mkdir -p "$outdir"

  local staging
  staging="$(mktemp -d)"
  trap 'rm -rf "$staging"' RETURN

  local arch multiarch
  arch="$(dpkg --print-architecture)"
  multiarch="$(multiarch_triplet "$arch")" || exit 2

  install -D -m 0755 "$binary" "$staging/usr/bin/$PKG"
  install -D -m 0644 "$library" \
    "$staging/usr/lib/$multiarch/libpagespeed.so"
  install -D -m 0644 "$REPO/deploy/$PKG.service" \
    "$staging/lib/systemd/system/$PKG.service"
  # 0640 root:root in the payload (never world-readable at any point); the
  # tmpfiles.d `z` line re-scopes it to root:pagespeed on install -- no
  # script ever chowns.  An upgrade REPLACES this conffile whenever the
  # operator has not edited it, so the payload mode is the mode an upgrade
  # lands (#1486).
  install -D -m 0640 "$REPO/deploy/$PKG.default" \
    "$staging/etc/default/$PKG"
  # Declarative identity + filesystem layout.  debhelper
  # >=13 would run systemd-sysusers automatically for the sysusers.d
  # drop-in; this package builds with raw dpkg-deb, so postinst calls it
  # (and systemd-tmpfiles) explicitly -- same effect, same conventions.
  install -D -m 0644 "$REPO/deploy/$PKG.sysusers.conf" \
    "$staging/usr/lib/sysusers.d/$PKG.conf"
  install -D -m 0644 "$REPO/deploy/$PKG.tmpfiles.conf" \
    "$staging/usr/lib/tmpfiles.d/$PKG.conf"
  # Secrets env file: shipped as DOCUMENTATION only.  The
  # real /etc/pagespeed-optimizer/daemon.env is CREATED BY POSTINST with
  # generated tokens, and is deliberately NOT a conffile: a
  # package-generated secret must never trigger a dpkg conffile prompt on
  # upgrade or be replaced by a package default.
  install -D -m 0644 "$REPO/deploy/$PKG.daemon.env" \
    "$staging/usr/share/doc/$PKG/daemon.env.example"
  # The shipped unit's syscall profile is ENFORCING (the
  # allow-list is in the unit itself, so no drop-in can ever be installed
  # "alone").  The documented OPT-OUT ships as an example under
  # /usr/share/doc for the operator to copy into /etc/systemd -- a directory
  # this package never writes to, which is what makes the opt-out survive
  # every upgrade.  Deliberately NOT a conffile under /etc: dpkg would honour
  # a deletion, but the RPM sibling would re-create a missing %config on
  # upgrade, and the two packages must carry the same contract.
  install -D -m 0644 "$REPO/deploy/$PKG-syscall-filter-off.conf" \
    "$staging/usr/share/doc/$PKG/90-syscall-filter-off.conf.example"
  # The browser-analysis re-admissions ship ACTIVE, as a VENDOR drop-in next
  # to the unit (2.1, ruling R2: the browser profile is part of the default).
  # Additive to the unit's allow-list, so shipping it installed can never
  # define a bare list; a host without browser analysis tightens by masking
  # it with an empty same-name file under /etc/systemd, which -- like the
  # opt-out -- survives upgrades because the package never writes there.
  # Kept a separate file rather than folded into the unit so this layer can
  # go back to opt-in on its own if the browser path ever shows a kill.
  install -D -m 0644 "$REPO/deploy/$PKG-browser-analysis.conf" \
    "$staging/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf"
  # The container-side counterpart: there is no systemd in a container, so
  # the unit profile does not apply there at all.  This is the runtime
  # profile that lets headless Chrome build its own sandbox under Docker.
  install -D -m 0644 "$REPO/deploy/chrome-seccomp.json" \
    "$staging/usr/share/doc/$PKG/chrome-seccomp.json"
  install -D -m 0644 "$REPO/LICENSE" "$staging/usr/share/doc/$PKG/LICENSE"
  install -D -m 0644 "$REPO/NOTICE" "$staging/usr/share/doc/$PKG/NOTICE"
  # The binary statically links BSD/MIT/Zlib-licensed components whose terms
  # require their notices to accompany a binary redistribution, so the
  # third-party notices ship next to LICENSE and NOTICE (the self-test below
  # asserts all three).
  install -D -m 0644 "$REPO/THIRD-PARTY-NOTICES" \
    "$staging/usr/share/doc/$PKG/THIRD-PARTY-NOTICES"
  install -D -m 0644 /dev/stdin "$staging/usr/share/doc/$PKG/copyright" <<EOF
Package: $PKG
Copyright: We-Amp B.V.
License: Apache-2.0 (full text in LICENSE; attributions in NOTICE and THIRD-PARTY-NOTICES, all in this directory)
EOF

  # Version tripwire: if the binary executes on this host, --version must
  # succeed, and when --expect-version was given its output must contain
  # that string; a mismatch refuses the build.
  if "$staging/usr/bin/$PKG" --version >/dev/null 2>&1; then
    local vout
    vout="$("$staging/usr/bin/$PKG" --version 2>&1 | head -1)"
    echo "binary --version: $vout"
    if [[ -n "$expect" ]]; then
      if [[ "$vout" == *"$expect"* ]]; then
        echo "version tripwire: ok (output contains '$expect')"
      else
        echo "error: --expect-version '$expect' not found in --version" \
          "output '$vout'" >&2
        exit 1
      fi
    fi
  else
    if [[ -n "$expect" ]]; then
      echo "error: --expect-version given but the binary does not execute" \
        "on this host; the claim cannot be verified" >&2
      exit 1
    fi
    echo "notice: binary does not execute on this host (cross-packaging?);" \
      "--version tripwire skipped" >&2
  fi

  mkdir -p "$staging/DEBIAN"
  cat > "$staging/DEBIAN/control" <<EOF
Package: $PKG
Version: $version
Architecture: $arch
Maintainer: We-Amp B.V. <info@we-amp.com>
Section: httpd
Priority: optional
Depends: libc6 (>= $glibc_floor)
Suggests: chromium | google-chrome-stable
Description: mod_pagespeed optimizer worker
 The mod_pagespeed optimizer worker, serving module integrations over a
 local socket. It optimizes images, CSS and JS with quality verification,
 keeps a shared cache volume, and runs as a systemd-supervised service.
 Ships the worker and the C API client library (libpagespeed.so) that
 serving modules bind. Serving-module packages depend on this package at an
 exact version; the two ship together.
 Agent install recipes: https://modpagespeed.com/recipes/README.md
EOF
  printf '/etc/default/%s\n' "$PKG" > "$staging/DEBIAN/conffiles"
  cat > "$staging/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ] || [ "$1" = "abort-remove" ]; then
  # the serving module dlopens libpagespeed.so by soname; keep the loader
  # cache current
  ldconfig
  # Identity + layout: create the pagespeed user/group
  # and the versioned cache directory declaratively.  Both are idempotent
  # and never touch an existing user or existing content.
  if command -v systemd-sysusers >/dev/null 2>&1; then
    systemd-sysusers pagespeed-optimizer.conf || true
  fi
  # Join the web-server user to group pagespeed so it can reach the daemon
  # socket. The serving module's package does the same, but only if the group
  # already exists, and the two packages have no install-order guarantee.
  # Idempotent, only users that exist, never fails the install.
  ps_joined=""
  for ps_u in www-data nginx apache; do
    getent passwd "$ps_u" >/dev/null 2>&1 || continue
    if getent group pagespeed | cut -d: -f4 | tr ',' '\n' | grep -qx "$ps_u"; then
      continue
    fi
    if usermod -a -G pagespeed "$ps_u" >/dev/null 2>&1 \
       || gpasswd -a "$ps_u" pagespeed >/dev/null 2>&1; then
      ps_joined="$ps_joined $ps_u"
    else
      echo "pagespeed-optimizer: warning: could not add user $ps_u to group pagespeed; add it by hand" >&2
    fi
  done
  if [ -n "$ps_joined" ]; then
    echo "pagespeed-optimizer: added$ps_joined to group pagespeed; restart the web server for the membership to take effect."
  fi
  # Provision the management-API and PURGE credentials at
  # INSTALL time, once, into the env file systemd hands the daemon.  Runs
  # after sysusers (the tmpfiles `z` line below needs group `pagespeed` to
  # exist) and before tmpfiles (which is what scopes the file to
  # 0640 root:pagespeed -- no script here ever chowns).
  #
  # Idempotent and upgrade-safe by construction: a key already present in
  # the file is NEVER rewritten, so an operator value survives every
  # upgrade, and a second install does not rotate a credential out from
  # under a running console session.  The tokens are provisioned but IDLE --
  # the package still ships with the API disabled.  A required credential
  # with no provisioning story is a requirement operators route around.
  ps_env=/etc/pagespeed-optimizer/daemon.env
  ps_gen_token() {
    if command -v openssl >/dev/null 2>&1; then
      openssl rand -base64 32 | tr '+/' '-_' | tr -d '=\n'
    elif command -v base64 >/dev/null 2>&1; then
      head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n'
    else
      od -An -tx1 -N32 /dev/urandom | tr -d ' \n'
    fi
  }
  if [ ! -e "$ps_env" ]; then
    mkdir -p /etc/pagespeed-optimizer
    # 0600 at creation: the file must never exist world- or group-readable,
    # not even for the instant before the mode is pinned.
    (umask 0177; : > "$ps_env")
    cat >> "$ps_env" <<'PSENVHDR'
# /etc/pagespeed-optimizer/daemon.env -- secrets for pagespeed-optimizer,
# read by systemd (EnvironmentFile) before the service drops to the
# unprivileged `pagespeed` user, so they never reach /proc/<pid>/cmdline.
#
# NOT a package conffile: values below were generated at install time and
# are never replaced by an upgrade. Edit freely; a key that is present is
# left alone. Removing a key makes the next install generate a new one.
#
# Permissions: 0640 root:pagespeed. Never world-readable.
# See /usr/share/doc/pagespeed-optimizer/daemon.env.example.
PSENVHDR
  fi
  # `^KEY=.` -- a key present but EMPTY is not a provisioned credential; it
  # is a half-written file (an interrupted install, a hand-edit that deleted
  # the value), and leaving it that way ships a daemon that refuses to start
  # with no way to tell why.  Treat empty as absent and heal it.  A generator
  # that produces nothing is a hard error, never a silently empty key.
  for ps_key in PAGESPEED_API_TOKEN PAGESPEED_PURGE_TOKEN; do
    if ! grep -q "^${ps_key}=." "$ps_env" 2>/dev/null; then
      ps_value="$(ps_gen_token)"
      if [ -z "$ps_value" ]; then
        echo "pagespeed-optimizer: FAILED to generate ${ps_key}: no usable" >&2
        echo "pagespeed-optimizer: source of randomness (openssl, base64 and" >&2
        echo "pagespeed-optimizer: od all unavailable or /dev/urandom" >&2
        echo "pagespeed-optimizer: unreadable). Refusing to write an empty" >&2
        echo "pagespeed-optimizer: credential; set ${ps_key} in $ps_env by hand." >&2
        exit 1
      fi
      # Drop any pre-existing empty line for this key before appending, so a
      # healed file has exactly one.
      sed -i "/^${ps_key}=$/d" "$ps_env"
      printf '%s=%s\n' "$ps_key" "$ps_value" >> "$ps_env"
    fi
  done
  chmod 0640 "$ps_env"
  # /etc/default/pagespeed-optimizer is a conffile: an upgrade REPLACES an
  # operator-unedited one with payload mode and root:root ownership, and
  # KEEPS an edited one with whatever mode it had -- either way the env-file
  # gate is 0640 root:pagespeed, so re-pin the mode on every install and
  # every upgrade (#1486).  The group comes from the tmpfiles `z` line
  # below, exactly like the secrets file; no script here ever chowns.  A
  # deleted conffile stays deleted (the guard), only its mode is not
  # re-pinned -- there is nothing to read.
  if [ -e /etc/default/pagespeed-optimizer ]; then
    chmod 0640 /etc/default/pagespeed-optimizer
  fi
  if [ -d /run/systemd/system ]; then
    systemd-tmpfiles --create pagespeed-optimizer.conf || true
  fi
  # Is the ACTION REQUIRED half of the notice below earned?  Only files
  # the web server actually loads may answer: backup and leftover names
  # (*.bak*, *.dpkg-*, *.rpmsave, *.rpmnew, *.orig, *~) are skipped, and
  # only load paths are scanned.  Debian-style layouts load a
  # *-available/ file (Apache conf-, sites-; nginx sites-) only through
  # its *-enabled/ symlink (grep -R follows it), so a stale operator
  # backup or a disabled site beside an already-current live config
  # stays silent.  The root-prefix argument exists for the packaging
  # tests.
  ps_stale_web_config() { # [root]
    ps_scan_root="${1:-/}"
    ps_needle="${2:-/var/lib/pagespeed}"
    ps_paths=""
    for ps_dir in nginx/nginx.conf nginx/conf.d nginx/sites-enabled \
        nginx/modules-enabled nginx/default.d \
        apache2/apache2.conf apache2/ports.conf apache2/conf.d \
        apache2/conf-enabled apache2/sites-enabled apache2/mods-enabled \
        httpd/conf httpd/conf.d httpd/conf.modules.d; do
      if [ -e "$ps_scan_root/etc/$ps_dir" ]; then
        ps_paths="$ps_paths $ps_scan_root/etc/$ps_dir"
      fi
    done
    [ -n "$ps_paths" ] || return 1
    # Include/IncludeOptional targets outside these standard trees are unseen; such an operator keeps the warning behaviour of a manual check.
    # the grep's own exit status is the answer: -q keeps a found match 0
    # even when another entry in the tree errors (a dangling enabled
    # symlink); 1 = no loaded file names an old path, and a bare scan
    # error never warns on its own
    grep -q -R -F "$ps_needle" \
      --exclude='*.bak*' --exclude='*.dpkg-*' --exclude='*.rpmsave' \
      --exclude='*.rpmnew' --exclude='*.orig' --exclude='*~' \
      $ps_paths 2>/dev/null
  }
  # Cold-start notice: a pre-existing ROOT-OWNED pagespeed cache can never
  # be chowned or migrated.  The daemon cold-starts into
  # /var/cache/pagespeed-optimizer/v3.  One line (apt and dnf show only the
  # first line of a maintainer-script message), and only while it is news:
  # the daemon has not yet written a v2 cache volume, or the web-server
  # configuration still names a pre-2.1 path.  A host whose configuration
  # names another volume path altogether is not served by the v2 default and
  # stays silent.  A configuration that still names a pre-2.1 path repeats
  # this notice on every configure until it is fixed (intended; do not turn
  # it into a once-only notice).
  for legacy in /var/lib/pagespeed-optimizer /var/lib/pagespeed; do
    if [ -d "$legacy" ] && \
       [ -n "$(find "$legacy" -mindepth 1 -maxdepth 1 -uid 0 -print -quit 2>/dev/null)" ]; then
      ps_action=""
      if ps_stale_web_config; then
        ps_action=" ACTION REQUIRED: the daemon's default paths moved, but your web-server configuration still points at the old ones; set 'pagespeed DaemonVolumePath' (nginx) or ModPagespeedDaemonVolumePath / ModPagespeedDaemonSocketPath (Apache) to /var/cache/pagespeed-optimizer/v3/cache and /run/pagespeed-optimizer/notify.sock and restart the web server, otherwise in-place optimization stays OFF."
      fi
      ps_cold=1
      if [ -n "$(find /var/cache/pagespeed-optimizer/v3 -name 'cache-*' -print -quit 2>/dev/null)" ]; then ps_cold=""; fi
      # a configuration that names some OTHER volume path is not served by the v2 default
      if ps_stale_web_config / DaemonVolumePath && ! ps_stale_web_config / /var/cache/pagespeed-optimizer/v3; then ps_cold=""; fi
      if [ -n "$ps_action$ps_cold" ]; then
        ps_msg="pre-existing root-owned cache detected at $legacy; the daemon now runs unprivileged (user pagespeed) with a fresh cache at /var/cache/pagespeed-optimizer/v3, so the cache will cold-start; no content is migrated or chowned."
        echo "pagespeed-optimizer: $ps_msg$ps_action See https://modpagespeed.com/docs/deployment/"
      fi
      break
    fi
  done
  # Generation notice: the cache-directory generation moved from v2 to v3
  # (cache format change).  The daemon never reads v2/ again and nothing
  # deletes it.  It is raised only while a loaded web-server file still
  # names the v1 or v2 path (the configured volume path is what decides, not
  # whether the old directory is still on disk), as one line.  It repeats on
  # every configure until the configuration is fixed (intended; do not turn
  # it into a once-only notice).
  for ps_old in v1 v2; do
    if ps_stale_web_config / /var/cache/pagespeed-optimizer/$ps_old; then
      echo "pagespeed-optimizer: ACTION REQUIRED: your web-server configuration still names the cache path /var/cache/pagespeed-optimizer/$ps_old (the cache format changed and the directory moved to v3); set 'pagespeed DaemonVolumePath' (nginx) or ModPagespeedDaemonVolumePath (Apache) to /var/cache/pagespeed-optimizer/v3/cache and restart the web server; the cache starts empty and refills as traffic arrives, and the $ps_old directory is left in place until you delete it."
    fi
  done
fi
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
  systemctl enable pagespeed-optimizer.service >/dev/null 2>&1 || true
  # abort-remove: a failed removal already ran prerm's stop; bring the
  # service back up just as a fresh configure would.
  if [ "$1" = "configure" ] || [ "$1" = "abort-remove" ]; then
    systemctl restart pagespeed-optimizer.service || true
  fi
fi
EOF
  cat > "$staging/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ] && [ "$1" = "remove" ]; then
  systemctl stop pagespeed-optimizer.service || true
fi
EOF
  cat > "$staging/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig || true
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
fi
if [ "$1" = "purge" ]; then
  # Only the cache this package manages: regenerable content in the
  # versioned directory.  A legacy pre-2.1 cache at
  # /var/lib/pagespeed-optimizer is NEVER auto-deleted — the
  # migration notes document the reclaim command; a relocated CACHE_DIR
  # (see /etc/default/pagespeed-optimizer) is the admin's to remove.
  rm -rf /var/cache/pagespeed-optimizer
  # The generated credentials are package-owned state, not operator config
  # the way a conffile is: purge takes them with it.
  rm -rf /etc/pagespeed-optimizer
  if [ -d /run/systemd/system ]; then
    systemctl disable pagespeed-optimizer.service >/dev/null 2>&1 || true
  fi
fi
EOF
  chmod 0755 "$staging/DEBIAN/postinst" "$staging/DEBIAN/prerm" \
    "$staging/DEBIAN/postrm"
  for s in postinst prerm postrm; do sh -n "$staging/DEBIAN/$s"; done

  local deb="$outdir/${PKG}_${version}_${arch}.deb"
  dpkg-deb --build --root-owner-group "$staging" "$deb" >/dev/null
  echo "built: $deb"
  echo "sha256: $(sha256sum "$deb" | cut -d' ' -f1)"
}

self_test() {
  local tmp fails=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  printf '#!/bin/sh\n[ "$1" = --version ] && { echo stub 0.0.0; exit 0; }\nexit 1\n' \
    > "$tmp/stub"
  chmod +x "$tmp/stub"
  printf 'not an ELF object\n' > "$tmp/stub.so"

  local out
  out="$(build_deb "0.0.0~selftest" "$tmp/stub" "$tmp/out" "0.0.0" "2.34" \
    "$tmp/stub.so")" || {
    echo "FAIL: build_deb with a stub binary" >&2; return 1; }
  local deb="$tmp/out/${PKG}_0.0.0~selftest_$(dpkg --print-architecture).deb"

  check() { # label expected actual
    if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
      echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails+1)); fi
  }
  check "deb exists" "yes" "$([[ -f "$deb" ]] && echo yes)"
  check "control Package" "$PKG" \
    "$(dpkg-deb -f "$deb" Package)"
  check "control Version" "0.0.0~selftest" \
    "$(dpkg-deb -f "$deb" Version)"
  check "Depends exact value" "libc6 (>= 2.34)" \
    "$(dpkg-deb -f "$deb" Depends)"
  # Browser analysis is optional and the package ships no browser: a
  # Suggests (never Recommends -- apt installs Recommends by default, and a
  # 300 MB Chromium is not something an optimizer install pulls in unasked).
  check "Suggests names a browser" "chromium | google-chrome-stable" \
    "$(dpkg-deb -f "$deb" Suggests)"
  local contents
  contents="$(dpkg-deb -c "$deb")"
  for path in "./usr/bin/$PKG" "./lib/systemd/system/$PKG.service" \
              "./etc/default/$PKG" "./usr/share/doc/$PKG/copyright" \
              "./usr/share/doc/$PKG/LICENSE" "./usr/share/doc/$PKG/NOTICE" \
              "./usr/share/doc/$PKG/THIRD-PARTY-NOTICES" \
              "./usr/lib/sysusers.d/$PKG.conf" \
              "./usr/lib/tmpfiles.d/$PKG.conf" \
              "./usr/share/doc/$PKG/daemon.env.example" \
              "./usr/share/doc/$PKG/90-syscall-filter-off.conf.example" \
              "./lib/systemd/system/$PKG.service.d/20-browser-analysis.conf" \
              "./usr/share/doc/$PKG/chrome-seccomp.json"; do
    check "ships $path" "1" "$(printf '%s' "$contents" | grep -c " $path\$")"
  done
  # The browser profile is a VENDOR drop-in, active on install, world-readable
  # (systemctl cat shows it), and NOT a conffile: a conffile would make dpkg
  # prompt on every change to the measured list, and the operator's off
  # switch is a mask under /etc, not an edit to this file.
  check "browser drop-in ships 0644 as a vendor drop-in" "1" \
    "$(printf '%s' "$contents" \
      | grep -c "^-rw-r--r-- .* ./lib/systemd/system/$PKG.service.d/20-browser-analysis.conf\$")"
  check "browser drop-in is NOT a conffile" "0" \
    "$(dpkg-deb --ctrl-tarfile "$deb" | tar -xO ./conffiles \
      | grep -c "20-browser-analysis" || true)"
  check "no browser-analysis example under /usr/share/doc any more" "0" \
    "$(printf '%s' "$contents" | grep -c "usr/share/doc/$PKG/20-browser-analysis" || true)"
  # The retired opt-in enforce example must not come back: the allow-list is
  # in the unit, and a second copy that "installs enforcement" would mislead.
  check "no retired 10-syscall-filter-enforce example in the payload" "0" \
    "$(printf '%s' "$contents" | grep -c "10-syscall-filter-enforce" || true)"
  # The upgrade-proof half of the opt-out contract: the
  # payload writes NOTHING under /etc/systemd.  An operator's
  # 90-syscall-filter-off.conf lives there, and a package that owned any
  # file in that tree could replace, prompt on, or re-create it on upgrade.
  check "payload ships nothing under /etc/systemd (operator territory)" "0" \
    "$(printf '%s' "$contents" | grep -c " ./etc/systemd/" || true)"
  check "binary mode 0755" "1" \
    "$(printf '%s' "$contents" | grep -c "^-rwxr-xr-x .* ./usr/bin/$PKG\$")"
  # #1486: the /etc/default conffile rides the payload at 0640 root:root --
  # an upgrade replaces an unedited conffile with exactly these bits, so the
  # payload mode IS the post-upgrade mode before tmpfiles re-scopes the
  # group.
  check "/etc/default env file mode 0640 in the payload" "1" \
    "$(printf '%s' "$contents" | grep -c "^-rw-r----- .* ./etc/default/$PKG\$")"
  # The payload must NOT carry /etc/.../daemon.env -- it is
  # postinst-generated (with real tokens) and must never be a conffile whose
  # replacement dpkg would offer on upgrade.
  check "payload does not ship the live daemon.env" "0" \
    "$(printf '%s' "$contents" \
      | grep -c " ./etc/pagespeed-optimizer/daemon.env\$" || true)"
  local triplet
  triplet="$(multiarch_triplet "$(dpkg --print-architecture)")"
  # the cache dir must be setgid AND sticky: group-writable by design, so a
  # group member must not be able to remove another's files
  check "tmpfiles.d creates the cache dir 3770 pagespeed:pagespeed" "1" \
    "$(dpkg-deb --fsys-tarfile "$deb" \
      | tar -xO ./usr/lib/tmpfiles.d/$PKG.conf \
      | grep -cE "^d +/var/cache/pagespeed-optimizer/v3 +3770 +pagespeed +pagespeed")"
  # #1486: both env files get their group scope from `z` lines, so an
  # upgrade that replaced (or kept) either file still converges on
  # 0640 root:pagespeed at the next install/upgrade/boot.
  check "tmpfiles.d re-scopes the daemon.env secrets file" "1" \
    "$(dpkg-deb --fsys-tarfile "$deb" \
      | tar -xO ./usr/lib/tmpfiles.d/$PKG.conf \
      | grep -cE "^z +/etc/pagespeed-optimizer/daemon.env +0640 +root +pagespeed")"
  check "tmpfiles.d re-scopes the /etc/default env file" "1" \
    "$(dpkg-deb --fsys-tarfile "$deb" \
      | tar -xO ./usr/lib/tmpfiles.d/$PKG.conf \
      | grep -cE "^z +/etc/default/pagespeed-optimizer +0640 +root +pagespeed")"
  check "ships client library in multiarch libdir" "1" \
    "$(printf '%s' "$contents" | grep -c " ./usr/lib/$triplet/libpagespeed.so\$")"
  check "client library mode 0644" "1" \
    "$(printf '%s' "$contents" \
      | grep -c "^-rw-r--r-- .* ./usr/lib/$triplet/libpagespeed.so\$")"
  check "env file is a conffile" "1" \
    "$(dpkg-deb --ctrl-tarfile "$deb" | tar -xO ./conffiles | grep -c "^/etc/default/$PKG\$")"
  check "daemon.env is NOT a conffile" "0" \
    "$(dpkg-deb --ctrl-tarfile "$deb" | tar -xO ./conffiles \
      | grep -c "^/etc/pagespeed-optimizer/daemon.env\$" || true)"
  # first-boot regression pins: the unit must let systemd create the
  # runtime dir BEFORE sandboxing (RuntimeDirectory), must run the daemon
  # unprivileged, and must not rely on ExecStartPre.
  local unit
  unit="$(dpkg-deb --fsys-tarfile "$deb" | tar -xO ./lib/systemd/system/$PKG.service)"
  check "unit runs as pagespeed" "1" \
    "$(printf '%s' "$unit" | grep -c "^User=pagespeed\$")"
  check "unit group pagespeed" "1" \
    "$(printf '%s' "$unit" | grep -c "^Group=pagespeed\$")"
  check "unit has RuntimeDirectory" "1" \
    "$(printf '%s' "$unit" | grep -c "^RuntimeDirectory=pagespeed-optimizer\$")"
  check "unit umask backstop 0007" "1" \
    "$(printf '%s' "$unit" | grep -c "^UMask=0007\$")"
  check "unit drops all capabilities" "1" \
    "$(printf '%s' "$unit" | grep -c "^CapabilityBoundingSet=\$")"
  check "unit reads the secrets env file" "1" \
    "$(printf '%s' "$unit" \
      | grep -c "^EnvironmentFile=-/etc/pagespeed-optimizer/daemon.env\$")"
  check "unit has no StateDirectory" "0" \
    "$(printf '%s' "$unit" | grep -c "^StateDirectory=" || true)"
  check "unit never runs root" "0" \
    "$(printf '%s' "$unit" | grep -c "^User=root\$" || true)"
  # StartLimit* are only honoured in [Unit]; in [Service] systemd ignores
  # them and a permanent refusal would restart forever instead of latching
  check "unit rate-limits starts in [Unit]" "1" \
    "$(printf '%s' "$unit" | sed -n '/^\[Unit\]/,/^\[Service\]/p' \
      | grep -c "^StartLimitBurst=5\$")"
  check "unit has no StartLimit in [Service]" "0" \
    "$(printf '%s' "$unit" | sed -n '/^\[Service\]/,$p' \
      | grep -c "^StartLimit" || true)"
  check "unit has no ExecStartPre mkdir" "0" \
    "$(printf '%s' "$unit" | grep -c 'ExecStartPre.*mkdir' || true)"
  # #1465: reload must refuse honestly (an ExecReload= that fails loudly and
  # points at restart), never fall through to systemd's SIGHUP default --
  # the daemon ignores SIGHUP, so the fallback would be a silent no-op.
  check "unit refuses reload honestly" "1" \
    "$(printf '%s' "$unit" \
      | grep -c '^ExecReload=.*reload is not supported' || true)"
  # Amended in 2.1: the shipped unit ENFORCES the syscall
  # allow-list.  Until 2.1 the load-bearing assertion here was the negative
  # one (no SystemCallFilter= may appear, the flip has to be deliberate); the
  # flip has now been made, on the strength of the census plus the rig's
  # enforcing legs, so the load-bearing assertion inverts: enforcement must
  # not silently DISAPPEAR, and it must be exactly the measured profile --
  # two group lines and nothing else, no reset, no errno conversion.
  check "shipped unit enforces the @system-service allow-list (+ mincore)" "1" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallFilter=@system-service mincore\$")"
  check "shipped unit subtracts @privileged and @resources" "1" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallFilter=~@privileged @resources\$")"
  check "shipped unit carries exactly those two filter lines" "2" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallFilter=" || true)"
  check "shipped unit never resets its own filter" "0" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallFilter=\$" || true)"
  check "shipped unit keeps logging calls outside @system-service" "1" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallLog=~@system-service mincore\$")"
  check "shipped unit converts no denial to an errno" "0" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallErrorNumber=" || true)"
  check "unit restricts syscall architectures to native" "1" \
    "$(printf '%s' "$unit" | grep -c "^SystemCallArchitectures=native\$")"
  # The four families, exactly: AF_UNIX (sockets + --api-socket), AF_NETLINK
  # (NSS/getaddrinfo, libuv interface enumeration), AF_INET/AF_INET6 (the TCP
  # API and the curl child that inherits this restriction).
  check "unit restricts address families to the measured four" "1" \
    "$(printf '%s' "$unit" \
      | grep -c "^RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_INET AF_INET6\$")"
  for d in ProtectKernelTunables ProtectKernelModules ProtectKernelLogs \
           ProtectControlGroups ProtectClock ProtectHostname RestrictSUIDSGID \
           RestrictRealtime LockPersonality; do
    check "unit sets $d=yes" "1" \
      "$(printf '%s' "$unit" | grep -c "^$d=yes\$")"
  done
  check "unit hides other users' processes" "1" \
    "$(printf '%s' "$unit" | grep -c "^ProtectProc=invisible\$")"
  check "unit disables core dumps" "1" \
    "$(printf '%s' "$unit" | grep -c "^LimitCORE=0\$")"
  # PrivateUsers= would kill the headless-Chrome sandbox (H4); the comment
  # in the unit says so, and this keeps it true.
  check "unit does not set PrivateUsers" "0" \
    "$(printf '%s' "$unit" | grep -c "^PrivateUsers=" || true)"
  # The daemon needs no namespace of its own; the browser-analysis drop-in
  # is what resets this for the Chrome sandbox.
  check "unit restricts namespaces (the browser drop-in re-widens)" "1" \
    "$(printf '%s' "$unit" | grep -c "^RestrictNamespaces=yes\$")"

  # ...the documented opt-out is exactly two resets and nothing else: a
  # directive smuggled into this file would be applied on every host that
  # opts out, under the name of turning something OFF.
  local optout browser
  optout="$(dpkg-deb --fsys-tarfile "$deb" \
    | tar -xO ./usr/share/doc/$PKG/90-syscall-filter-off.conf.example)"
  check "opt-out example resets the syscall filter" "1" \
    "$(printf '%s' "$optout" | grep -c "^SystemCallFilter=\$")"
  check "opt-out example resets RestrictNamespaces" "1" \
    "$(printf '%s' "$optout" | grep -c "^RestrictNamespaces=\$")"
  check "opt-out example carries no other directive" "3" \
    "$(printf '%s' "$optout" | grep -cv '^[[:space:]]*#\|^[[:space:]]*$' || true)"
  browser="$(dpkg-deb --fsys-tarfile "$deb" \
    | tar -xO ./lib/systemd/system/$PKG.service.d/20-browser-analysis.conf)"
  # By PLAIN NAME, never by group: @sandbox does not exist before systemd
  # 254, and systemd drops an unknown group with only a warning -- which
  # would silently remove seccomp(2) from the allow-list on Debian 12 and
  # EL9 and kill Chrome's layer-2 sandbox there.
  check "browser drop-in re-admits the Chrome sandbox calls by name" "1" \
    "$(printf '%s' "$browser" | grep -c "^SystemCallFilter=seccomp chroot capset setuid setresuid setgroups\$")"
  # The measured second line (#1472): three @resources members the log-only
  # soak can never surface, the two logged names outside @system-service,
  # and mincore -- log-visible but on a minutes-scale MemoryInfra timer, so any
  # window shorter than that records zero calls (found via an rc.11 CI kill).
  check "browser drop-in re-admits what a running Chrome was measured to need" "1" \
    "$(printf '%s' "$browser" | grep -c "^SystemCallFilter=setpriority sched_setaffinity sched_setattr pkey_alloc landlock_create_ruleset mincore\$")"
  check "browser drop-in names no syscall GROUP" "0" \
    "$(printf '%s' "$browser" | grep -c "^SystemCallFilter=.*@" || true)"
  check "browser drop-in resets RestrictNamespaces" "1" \
    "$(printf '%s' "$browser" | grep -c "^RestrictNamespaces=\$")"
  # The container profile is Docker's default plus four calls; anything from
  # the @mount family leaking into it would make it barely tighter than
  # --privileged.
  local chromeprof
  chromeprof="$(dpkg-deb --fsys-tarfile "$deb" \
    | tar -xO ./usr/share/doc/$PKG/chrome-seccomp.json)"
  check "container profile still defaults to deny" "1" \
    "$(printf '%s' "$chromeprof" | grep -c '"defaultAction": "SCMP_ACT_ERRNO"')"
  check "container profile unblocks the four Chrome calls" "1" \
    "$(printf '%s' "$chromeprof" | python3 -c '
import json,sys
d=json.load(sys.stdin)
a=set()
for e in d["syscalls"]:
    if e.get("action")=="SCMP_ACT_ALLOW" and "args" not in e \
       and not e.get("includes") and not e.get("excludes"):
        a.update(e.get("names",[]))
need={"clone","clone3","unshare","chroot"}
leak={"mount","umount2","pivot_root","setns"} & a
print(1 if need <= a and not leak else 0)')"
  # maintainer-script SEMANTICS, not just syntax: the load-bearing lines.
  local ctrl="$tmp/ctrl"
  mkdir -p "$ctrl"
  dpkg-deb --ctrl-tarfile "$deb" | tar -x -C "$ctrl"
  check "postinst runs ldconfig" "1" \
    "$(grep -c "^  ldconfig\$" "$ctrl/postinst" || true)"
  check "postinst creates the sysusers identity" "1" \
    "$(grep -c "systemd-sysusers pagespeed-optimizer.conf" "$ctrl/postinst")"
  check "postinst applies tmpfiles" "1" \
    "$(grep -c "systemd-tmpfiles --create pagespeed-optimizer.conf" "$ctrl/postinst")"
  check "postinst prints the cold-start notice" "1" \
    "$(grep -c "cold-start; no content is migrated or chowned." "$ctrl/postinst")"
  check "postinst tells the operator to repoint the web server" "1" \
    "$(grep -c "ACTION REQUIRED: the daemon.s default paths moved" "$ctrl/postinst")"
  # The repoint warning must be earned before it is printed: the postinst
  # scans only web-server configuration the server actually loads (the
  # *-enabled/ trees, never *-available/ on its own) and skips backup and
  # leftover file names, so a stale .bak cannot warn on a host whose live
  # config is already current.
  check "postinst has the loaded-config scan helper" "1" \
    "$(grep -c "ps_stale_web_config() {" "$ctrl/postinst")"
  check "scan skips the six backup/leftover name patterns" "6" \
    "$(grep -o -- "--exclude=" "$ctrl/postinst" | wc -l | tr -d ' ')"
  check "scan reads Apache through its *-enabled/ trees only" "1" \
    "$(grep -c "apache2/conf-enabled apache2/sites-enabled apache2/mods-enabled" \
      "$ctrl/postinst")"
  check "repoint warning is gated on the scan" "1" \
    "$(grep -c "if ps_stale_web_config; then" "$ctrl/postinst")"
  check "postinst never names the container module's pagespeed_cache_path" "0" \
    "$(grep -c 'pagespeed_cache_path' "$ctrl/postinst" || true)"
  check "postinst names the native nginx directive" "2" \
    "$(grep -c "'pagespeed DaemonVolumePath'" "$ctrl/postinst")"
  check "stale-path notice loops over v1 and v2" "1" \
    "$(grep -c 'for ps_old in v1 v2; do' "$ctrl/postinst")"
  check "stale-path notice is gated on the configured path, not the directory" "1" \
    "$(grep -c -F 'if ps_stale_web_config / /var/cache/pagespeed-optimizer/$ps_old; then' "$ctrl/postinst")"
  check "stale-path notice has no directory-presence clause" "0" \
    "$(grep -c -F -- '-d /var/cache/pagespeed-optimizer/' "$ctrl/postinst" || true)"
  check "cold-start notice stays silent for another configured volume path" "1" \
    "$(grep -c 'ps_stale_web_config / DaemonVolumePath && ! ps_stale_web_config / /var/cache/pagespeed-optimizer/v3' "$ctrl/postinst")"
  check "postinst has no redundant ps_stale" "0" \
    "$(grep -c 'ps_stale=' "$ctrl/postinst" || true)"
  check "postinst never chowns" "0" \
    "$(grep -cE "^[[:space:]]*(chown|chgrp)[[:space:]]" "$ctrl/postinst" || true)"
  # Token provisioning: generated once, never rewritten, and
  # the file is created 0600 before it is widened to 0640 root:pagespeed by
  # the tmpfiles `z` line -- it is never world- or group-readable in between.
  check "postinst provisions the API token" "1" \
    "$(grep -c 'PAGESPEED_API_TOKEN PAGESPEED_PURGE_TOKEN' "$ctrl/postinst")"
  check "postinst only writes a key that is absent" "1" \
    "$(grep -c 'if ! grep -q "\^\${ps_key}=\." "\$ps_env"' "$ctrl/postinst")"
  check "postinst creates the env file with a 0177 umask" "1" \
    "$(grep -c 'umask 0177' "$ctrl/postinst")"
  check "postinst pins the env file to 0640" "1" \
    "$(grep -c 'chmod 0640 "\$ps_env"' "$ctrl/postinst")"
  check "postinst pins /etc/default to 0640 on every upgrade" "1" \
    "$(grep -c 'chmod 0640 /etc/default/pagespeed-optimizer' "$ctrl/postinst")"
  check "postinst generates tokens before applying tmpfiles" "1" \
    "$(awk '/chmod 0640 "\$ps_env"/{gen=NR} /systemd-tmpfiles --create/{if (gen && NR>gen) {print 1; exit}}' \
      "$ctrl/postinst")"
  check "postrm purge removes the generated secrets" "1" \
    "$(grep -c 'rm -rf /etc/pagespeed-optimizer' "$ctrl/postrm")"
  check "postrm runs ldconfig" "1" \
    "$(grep -c "^ldconfig || true\$" "$ctrl/postrm" || true)"
  check "postinst restarts on configure" "1" \
    "$(grep -c 'systemctl restart pagespeed-optimizer.service' "$ctrl/postinst")"
  # both the ldconfig gate and the service-restart gate name abort-remove
  check "postinst handles abort-remove" "2" \
    "$(grep -c '"\$1" = "abort-remove"' "$ctrl/postinst")"
  check "prerm stops on remove" "1" \
    "$(grep -c 'systemctl stop pagespeed-optimizer.service' "$ctrl/prerm")"
  check "postrm purge removes the managed cache dir" "1" \
    "$(grep -c 'rm -rf /var/cache/pagespeed-optimizer' "$ctrl/postrm")"
  check "postrm never deletes the legacy cache" "0" \
    "$(grep -c 'rm -rf /var/lib/pagespeed-optimizer' "$ctrl/postrm" || true)"
  # a bad version must refuse before staging anything
  if (build_deb "bad version!" "$tmp/stub" "$tmp/out2" "" "2.34" "$tmp/stub.so") >/dev/null 2>&1; then
    echo "FAIL: a malformed version was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: malformed version refused"
  fi
  # a missing binary must refuse
  if (build_deb "0.0.1" "$tmp/no-such-binary" "$tmp/out3" "" "2.34" "$tmp/stub.so") >/dev/null 2>&1; then
    echo "FAIL: a missing binary was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: missing binary refused"
  fi
  # a --version output missing the expected string must refuse
  if (build_deb "0.0.2" "$tmp/stub" "$tmp/out4" "9.9.9" "2.34" "$tmp/stub.so") >/dev/null 2>&1; then
    echo "FAIL: a version-tripwire mismatch was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: version-tripwire mismatch refused"
  fi
  # a missing client library must refuse
  if (build_deb "0.0.4" "$tmp/stub" "$tmp/out6" "" "2.34" "$tmp/no-such-lib") \
      >/dev/null 2>&1; then
    echo "FAIL: a missing client library was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: missing client library refused"
  fi
  # a non-ELF binary with no explicit floor must refuse (no hand-me-down
  # default floor exists to fall back to)
  if (build_deb "0.0.3" "$tmp/stub" "$tmp/out5" "" "" "$tmp/stub.so") >/dev/null 2>&1; then
    echo "FAIL: a non-ELF binary without --glibc-floor was accepted" >&2
    fails=$((fails+1))
  else
    echo "ok: underivable glibc floor refused"
  fi
  # The shipped scan helper's behaviour (backup and leftover files never
  # earn the repoint warning) is asserted against fixture trees by the
  # standalone test next to this script; run it here so every lane that
  # exercises the packaging self-tests exercises it too.
  if bash "$HERE/test-postinst-stale-config.sh" >/dev/null; then
    echo "ok: postinst stale-config scan unit test"
  else
    echo "FAIL: postinst stale-config scan unit test" >&2
    fails=$((fails+1))
  fi
  if [[ "$fails" -gt 0 ]]; then
    echo "self-test: $fails FAILURE(S)" >&2; return 1
  fi
  echo "self-test: all checks passed"
}

VERSION="" BINARY="" LIBRARY="" OUT="$REPO/bazel-bin/packaging" EXPECT="" FLOOR=""
case "${1:-}" in
  --self-test) self_test; exit $? ;;
  "") usage ;;
esac
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --library) LIBRARY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --expect-version) EXPECT="$2"; shift 2 ;;
    --glibc-floor) FLOOR="$2"; shift 2 ;;
    *) usage ;;
  esac
done
BINARY="${BINARY:-$REPO/bazel-bin/src/worker/factory_worker}"
LIBRARY="${LIBRARY:-$REPO/bazel-bin/lib/pagespeed/libpagespeed.so}"
build_deb "$VERSION" "$BINARY" "$OUT" "$EXPECT" "$FLOOR" "$LIBRARY"
