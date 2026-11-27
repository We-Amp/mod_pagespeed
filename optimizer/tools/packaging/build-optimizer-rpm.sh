#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the pagespeed-optimizer RPM: the factory worker as a host package
# (systemd-supervised, UDS socket, EnvironmentFile-tunable) plus the C API
# client library (libpagespeed.so) a serving module binds by soname —
# the RPM sibling of build-optimizer-deb.sh, so RPM-based serving-module
# packages can declare an exact-version Requires on it.
#
#   build-optimizer-rpm.sh --version VERSION [--binary PATH] [--out DIR]
#                          [--library PATH] [--expect-version STR]
#                          [--glibc-floor X.Y]
#   build-optimizer-rpm.sh --self-test
#
# The script does NOT run bazel: the binary and library are inputs
# (defaults: bazel-bin/src/worker/factory_worker and
# bazel-bin/lib/pagespeed/libpagespeed.so). It DOES require rpmbuild —
# absent rpmbuild it refuses with a container hint (the CI lane runs it
# in an EL image; local runs can too).
#
# Version mapping: pass the PACKAGE version (tilde pre-release form,
# e.g. 1.16.0~rc.1). rpm's tilde sorts before the final release, the
# same ordering contract the deb carries. The string maps to the spec as
# Version: <part before first '-'> / Release: <part after, or 1> — this
# package's train versions never carry '-', so Release is always 1.
#
# The glibc Requires floor is DERIVED as the max across both ELF objects'
# GLIBC_* dynamic symbols and expressed as an explicit
# "Requires: glibc >= X.Y" (rpmbuild's automatic ELF find-requires adds
# the per-symbol-version requires as well; the explicit floor keeps the
# human-auditable contract identical to the deb). --glibc-floor exists
# only for non-ELF stand-ins (the self-test stubs) and refuses to
# contradict a derivable floor.
#
# --expect-version STR: when the binary executes on this host, its
# --version output must contain STR or the build refuses; passing it for
# a binary that cannot execute here is refused too.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PKG=pagespeed-optimizer

usage() {
  sed -n '4,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 2
}

require_rpmbuild() {
  command -v rpmbuild >/dev/null 2>&1 || {
    echo "error: rpmbuild not found. Run inside an EL container, e.g.:" >&2
    echo "  docker run --rm -v \$PWD:/w -w /w almalinux:9 bash -c \\" >&2
    echo "    'dnf -y install rpm-build systemd-rpm-macros binutils && tools/packaging/build-optimizer-rpm.sh ...'" >&2
    exit 1
  }
}

derive_floor() { # binary library floor_opt -> echoes floor or exits
  local binary="$1" library="$2" floor_opt="$3"
  local derived="" obj f
  for obj in "$binary" "$library"; do
    if [[ "$(head -c4 "$obj" | od -An -tx1 | tr -d ' \n')" == "7f454c46" ]]; then
      f="$( { readelf --dyn-syms -W "$obj" 2>/dev/null || true; } \
        | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -u -V | tail -1)"
      [[ -n "$f" ]] || { echo "error: cannot derive a glibc floor from ELF" \
        "object '$obj' (no GLIBC_* dynamic symbols)" >&2; exit 1; }
      derived="$(printf '%s\n%s\n' "$derived" "$f" | sed '/^$/d' \
        | sort -V | tail -1)"
    fi
  done
  if [[ -n "$derived" ]]; then
    if [[ -n "$floor_opt" && "$floor_opt" != "$derived" ]]; then
      echo "error: --glibc-floor $floor_opt contradicts the derived floor" \
        "$derived" >&2; exit 1
    fi
    echo "$derived"
  else
    [[ -n "$floor_opt" ]] || { echo "error: neither the binary nor the" \
      "library is ELF; a glibc floor cannot be derived -- pass" \
      "--glibc-floor" >&2; exit 1; }
    echo "$floor_opt"
  fi
}

build_rpm() {
  local version="$1" binary="$2" outdir="$3"
  local expect="${4:-}" floor_opt="${5:-}" library="${6:-}"
  [[ -n "$version" ]] || { echo "error: --version is required" >&2; exit 2; }
  # rpm version grammar: no '-' (that separates Release), no ':' (epoch).
  case "$version" in
    *[!A-Za-z0-9.~^+]*) echo "error: version '$version' has characters" \
      "outside [A-Za-z0-9.~^+]" >&2; exit 2 ;;
  esac
  [[ -f "$binary" ]] || { echo "error: binary not found: $binary" \
    "(build //src/worker:factory_worker first, or pass --binary)" >&2; exit 1; }
  [[ -f "$library" ]] || { echo "error: client library not found: $library" \
    "(build //lib/pagespeed:libpagespeed.so first, or pass --library)" >&2
    exit 1; }
  require_rpmbuild

  local glibc_floor
  glibc_floor="$(derive_floor "$binary" "$library" "$floor_opt")"
  case "$glibc_floor" in
    *[!0-9.]*) echo "error: glibc floor '$glibc_floor' is not a version" >&2
      exit 2 ;;
  esac
  echo "glibc floor: $glibc_floor"
  mkdir -p "$outdir"

  # Version tripwire (same contract as the deb script).
  if "$binary" --version >/dev/null 2>&1; then
    local vout
    vout="$("$binary" --version 2>&1 | head -1)"
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

  local arch
  arch="$(rpm --eval '%{_arch}')"

  local top
  top="$(mktemp -d)"
  trap 'rm -rf "$top"' RETURN
  mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS,stage}

  local stage="$top/stage"
  local libdir
  case "$arch" in
    x86_64|aarch64) libdir="/usr/lib64" ;;
    *) echo "error: no libdir mapping for rpm architecture '$arch'" >&2
       exit 2 ;;
  esac
  install -D -m 0755 "$binary" "$stage/usr/bin/$PKG"
  install -D -m 0644 "$library" "$stage$libdir/libpagespeed.so"
  install -D -m 0644 "$REPO/deploy/$PKG.service" \
    "$stage/usr/lib/systemd/system/$PKG.service"
  # 0640 root:root in the payload (never world-readable at any point); the
  # %attr in %files names the pagespeed group directly (%pre creates it
  # before the payload lands), and the tmpfiles.d `z` line re-asserts the
  # scope on every boot.  An upgrade REPLACES this %config whenever the
  # operator has not edited it, so the payload mode is the mode an upgrade
  # lands (#1486).
  install -D -m 0640 "$REPO/deploy/$PKG.default" \
    "$stage/etc/default/$PKG"
  # Declarative identity + filesystem layout.  The
  # sysusers.d drop-in feeds %sysusers_create_compat in %pre; tmpfiles.d
  # owns the versioned cache dir and the env file's group scope.
  install -D -m 0644 "$REPO/deploy/$PKG.sysusers.conf" \
    "$stage/usr/lib/sysusers.d/$PKG.conf"
  install -D -m 0644 "$REPO/deploy/$PKG.tmpfiles.conf" \
    "$stage/usr/lib/tmpfiles.d/$PKG.conf"
  # Secrets env file: shipped as DOCUMENTATION only.  The
  # live /etc/pagespeed-optimizer/daemon.env is generated by %post with real
  # tokens and is a %ghost, never %config: a package-generated secret must
  # never land in an .rpmnew or be replaced by a package default on upgrade.
  install -D -m 0644 "$REPO/deploy/$PKG.daemon.env" \
    "$stage/usr/share/doc/$PKG/daemon.env.example"
  # The shipped unit's syscall profile is ENFORCING (the
  # allow-list is in the unit itself, so no drop-in can ever be installed
  # "alone").  The documented OPT-OUT ships as an example under
  # /usr/share/doc for the operator to copy into /etc/systemd -- a directory
  # this package never writes to, which is what makes the opt-out survive
  # every upgrade.  Deliberately NOT a %config under /etc: rpm re-creates a
  # %config the operator deleted on the next upgrade, so "delete the file"
  # would not have been an opt-out that survives one.
  install -D -m 0644 "$REPO/deploy/$PKG-syscall-filter-off.conf" \
    "$stage/usr/share/doc/$PKG/90-syscall-filter-off.conf.example"
  # The browser-analysis re-admissions ship ACTIVE, as a VENDOR drop-in next
  # to the unit (2.1, ruling R2: the browser profile is part of the default).
  # Additive to the unit's allow-list, so shipping it installed can never
  # define a bare list; a host without browser analysis tightens by masking
  # it with an empty same-name file under /etc/systemd, which -- like the
  # opt-out -- survives upgrades because the package never writes there.
  # Kept a separate file rather than folded into the unit so this layer can
  # go back to opt-in on its own if the browser path ever shows a kill.
  install -D -m 0644 "$REPO/deploy/$PKG-browser-analysis.conf" \
    "$stage/usr/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf"
  # The container-side counterpart: a container has no systemd, so the unit
  # profile does not apply there at all.
  install -D -m 0644 "$REPO/deploy/chrome-seccomp.json" \
    "$stage/usr/share/doc/$PKG/chrome-seccomp.json"
  # The %ghost env file lives here; the directory itself is package-owned so
  # an erase can take it (and the generated secret inside it) away.
  install -d -m 0755 "$stage/etc/pagespeed-optimizer"
  install -D -m 0644 "$REPO/LICENSE" "$stage/usr/share/licenses/$PKG/LICENSE"
  install -D -m 0644 "$REPO/NOTICE" "$stage/usr/share/licenses/$PKG/NOTICE"
  # The binary statically links BSD/MIT/Zlib-licensed components whose terms
  # require their notices to accompany a binary redistribution, so the
  # third-party notices ship as %license next to LICENSE and NOTICE (the
  # self-test below asserts all three).
  install -D -m 0644 "$REPO/THIRD-PARTY-NOTICES" \
    "$stage/usr/share/licenses/$PKG/THIRD-PARTY-NOTICES"

  cat > "$top/SPECS/$PKG.spec" <<EOF
Name: $PKG
Version: $version
Release: 1
Summary: mod_pagespeed optimizer worker
License: Apache-2.0
URL: https://modpagespeed.com/
BuildArch: $arch
# No BuildRequires on systemd-rpm-macros: rpmbuild ENFORCES build deps against
# the rpmdb, and the packaging image is a Debian-family host whose rpmdb knows
# no EL package at all -- the requirement could never be satisfied there, only
# refused. The spec below needs no macro package: it uses the EL sysusers
# macros where they are defined and spells out the same result where they are
# not.
Requires: glibc >= $glibc_floor
# Browser analysis is optional and the package ships no browser. A weak
# dependency: EL9's rpm honours it, and the pinned build container's rpmbuild
# parses the tag (an rpm too old to know it would refuse the spec, not skip
# the line -- moot here, stated so nobody relies on the opposite).
Suggests: chromium
# useradd/groupadd in %pre (sysusers compat expansion) need shadow-utils.
# Stated OUTRIGHT as well as through the macro: rpm on a Debian-family host
# ships no EL macro set at all, so an unconditional %%sysusers_requires_compat
# is an Unknown tag there and the spec never parses. rpm folds the duplicate
# away where both apply, so the dependency is declared exactly once either way.
Requires(pre): shadow-utils
%{?sysusers_requires_compat}
# The payload is pre-built; never strip or re-process it here.
%define __strip /bin/true
%define debug_package %{nil}
%define _build_id_links none

%description
The mod_pagespeed optimizer worker, serving module integrations over a
local socket. It optimizes images, CSS and JS with quality verification,
keeps a shared cache volume, and runs as a systemd-supervised service.
Ships the worker and the C API client library (libpagespeed.so) that
serving modules bind. Serving-module packages depend on this package at an
exact version; the two ship together.
Agent install recipes: https://modpagespeed.com/recipes/README.md

%install
[ -n "%{getenv:OPT_STAGE}" ] || { echo "OPT_STAGE not set" >&2; exit 1; }
cp -a %{getenv:OPT_STAGE}/. %{buildroot}/

# Create the pagespeed user/group BEFORE any file is
# installed (the daemon.env %attr names the group).  Expands at build time
# from the staged sysusers.d drop-in (the macro reads the file while the
# spec is parsed, so it must be handed the staged path, not %{name});
# idempotent, never modifies an existing user.
%pre
%if %{defined sysusers_create_compat}
%sysusers_create_compat $stage/usr/lib/sysusers.d/$PKG.conf
%else
# No EL sysusers macros on the host that built this package, so the scriptlet
# the macro would have GENERATED from the staged drop-in is spelled out
# instead -- byte-identical to that expansion, values tracking
# deploy/$PKG.sysusers.conf.  systemd-sysusers is preferred where it exists
# AND the drop-in is already on disk (an upgrade); on a first install it
# cannot be, since this scriptlet runs before the payload lands, so the two
# getent lines below are what establish the identity.  Every form here is
# idempotent and none of them ever modifies an existing account.
if command -v systemd-sysusers >/dev/null 2>&1 && [ -f /usr/lib/sysusers.d/$PKG.conf ]; then systemd-sysusers /usr/lib/sysusers.d/$PKG.conf || :; fi
getent group 'pagespeed' >/dev/null || groupadd -r 'pagespeed' || :
getent passwd 'pagespeed' >/dev/null || useradd -r -g 'pagespeed' -d '/var/cache/pagespeed-optimizer' -s '/usr/sbin/nologin' -c 'PageSpeed optimizer' 'pagespeed' || :
%endif

%post
# the serving module dlopens libpagespeed.so by soname; keep the loader
# cache current
ldconfig
# Join the web-server user to group pagespeed so it can reach the daemon
# socket. The serving module's package does the same, but only if the group
# already exists, and the two packages have no install-order guarantee.
# Idempotent, only users that exist, never fails the install.
ps_joined=""
for ps_u in www-data nginx apache; do
  getent passwd "\$ps_u" >/dev/null 2>&1 || continue
  if getent group pagespeed | cut -d: -f4 | tr ',' '\n' | grep -qx "\$ps_u"; then
    continue
  fi
  if usermod -a -G pagespeed "\$ps_u" >/dev/null 2>&1 \
     || gpasswd -a "\$ps_u" pagespeed >/dev/null 2>&1; then
    ps_joined="\$ps_joined \$ps_u"
  else
    echo "%{name}: warning: could not add user \$ps_u to group pagespeed; add it by hand" >&2
  fi
done
if [ -n "\$ps_joined" ]; then
  echo "%{name}: added\$ps_joined to group pagespeed; restart the web server for the membership to take effect."
fi
# Provision the management-API and PURGE credentials at
# INSTALL time, once, into the env file systemd hands the daemon.  The
# identity exists already (%pre ran sysusers), and the tmpfiles \`z\` line
# below is what scopes the file to 0640 root:pagespeed -- no scriptlet here
# ever chowns.
#
# Idempotent and upgrade-safe by construction: a key already present is
# NEVER rewritten, so an operator value survives every upgrade and a
# reinstall does not rotate a credential out from under a running console.
# The tokens are provisioned but IDLE -- the package still ships with the
# API disabled.
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
if [ ! -e "\$ps_env" ]; then
  mkdir -p /etc/pagespeed-optimizer
  # 0600 at creation: never world- or group-readable, not even for the
  # instant before the mode is pinned.
  (umask 0177; : > "\$ps_env")
  cat >> "\$ps_env" <<'PSENVHDR'
# /etc/pagespeed-optimizer/daemon.env -- secrets for pagespeed-optimizer,
# read by systemd (EnvironmentFile) before the service drops to the
# unprivileged \`pagespeed\` user, so they never reach /proc/<pid>/cmdline.
#
# NOT an rpm %config file: values below were generated at install time and
# are never replaced by an upgrade (no .rpmnew, no prompt). Edit freely; a
# key that is present is left alone. Removing a key makes the next install
# generate a new one.
#
# Permissions: 0640 root:pagespeed. Never world-readable.
PSENVHDR
fi
# \`^KEY=.\` -- a key present but EMPTY is not a provisioned credential; it is
# a half-written file, and leaving it that way ships a daemon that refuses to
# start with no way to tell why.  Treat empty as absent and heal it.  A
# generator that produces nothing is a hard error, never an empty key.
for ps_key in PAGESPEED_API_TOKEN PAGESPEED_PURGE_TOKEN; do
  if ! grep -q "^\${ps_key}=." "\$ps_env" 2>/dev/null; then
    ps_value="\$(ps_gen_token)"
    if [ -z "\$ps_value" ]; then
      echo "%{name}: FAILED to generate \${ps_key}: no usable source of" >&2
      echo "%{name}: randomness (openssl, base64 and od all unavailable or" >&2
      echo "%{name}: /dev/urandom unreadable). Refusing to write an empty" >&2
      echo "%{name}: credential; set \${ps_key} in \$ps_env by hand." >&2
      exit 1
    fi
    sed -i "/^\${ps_key}=\$/d" "\$ps_env"
    printf '%s=%s\n' "\$ps_key" "\$ps_value" >> "\$ps_env"
  fi
done
chmod 0640 "\$ps_env"
# /etc/default/$PKG is %config(noreplace): an upgrade REPLACES an
# operator-unedited one with payload mode and ownership, and KEEPS an
# edited one with whatever mode it had -- either way the env-file gate is
# 0640 root:pagespeed, so re-pin the mode on every install and every
# upgrade (#1486).  The group comes from the payload %attr and the tmpfiles
# \`z\` line; no scriptlet here ever chowns.  A deleted config stays
# deleted (the guard), only its mode is not re-pinned.
if [ -e /etc/default/$PKG ]; then
  chmod 0640 /etc/default/$PKG
fi
# filesystem layout (versioned cache dir 3770 pagespeed:pagespeed, env
# file group scope) — declarative, idempotent, never touches content.
if [ -d /run/systemd/system ]; then
  systemd-tmpfiles --create %{name}.conf || true
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
  ps_scan_root="\${1:-/}"
  ps_needle="\${2:-/var/lib/pagespeed}"
  ps_paths=""
  for ps_dir in nginx/nginx.conf nginx/conf.d nginx/sites-enabled \\
      nginx/modules-enabled nginx/default.d \\
      apache2/apache2.conf apache2/ports.conf apache2/conf.d \\
      apache2/conf-enabled apache2/sites-enabled apache2/mods-enabled \\
      httpd/conf httpd/conf.d httpd/conf.modules.d; do
    if [ -e "\$ps_scan_root/etc/\$ps_dir" ]; then
      ps_paths="\$ps_paths \$ps_scan_root/etc/\$ps_dir"
    fi
  done
  [ -n "\$ps_paths" ] || return 1
  # Include/IncludeOptional targets outside these standard trees are unseen; such an operator keeps the warning behaviour of a manual check.
  # the grep's own exit status is the answer: -q keeps a found match 0
  # even when another entry in the tree errors (a dangling enabled
  # symlink); 1 = no loaded file names an old path, and a bare scan
  # error never warns on its own
  grep -q -R -F "\$ps_needle" \\
    --exclude='*.bak*' --exclude='*.dpkg-*' --exclude='*.rpmsave' \\
    --exclude='*.rpmnew' --exclude='*.orig' --exclude='*~' \\
    \$ps_paths 2>/dev/null
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
  if [ -d "\$legacy" ] && \
     [ -n "\$(find "\$legacy" -mindepth 1 -maxdepth 1 -uid 0 -print -quit 2>/dev/null)" ]; then
    ps_action=""
    if ps_stale_web_config; then
      ps_action=" ACTION REQUIRED: the daemon's default paths moved, but your web-server configuration still points at the old ones; set 'pagespeed DaemonVolumePath' (nginx) or ModPagespeedDaemonVolumePath / ModPagespeedDaemonSocketPath (Apache) to /var/cache/pagespeed-optimizer/v3/cache and /run/pagespeed-optimizer/notify.sock and restart the web server, otherwise in-place optimization stays OFF."
    fi
    ps_cold=1
    if [ -n "\$(find /var/cache/pagespeed-optimizer/v3 -name 'cache-*' -print -quit 2>/dev/null)" ]; then ps_cold=""; fi
    # a configuration that names some OTHER volume path is not served by the v2 default
    if ps_stale_web_config / DaemonVolumePath && ! ps_stale_web_config / /var/cache/pagespeed-optimizer/v3; then ps_cold=""; fi
    if [ -n "\$ps_action\$ps_cold" ]; then
      ps_msg="pre-existing root-owned cache detected at \$legacy; the daemon now runs unprivileged (user pagespeed) with a fresh cache at /var/cache/pagespeed-optimizer/v3, so the cache will cold-start; no content is migrated or chowned."
      echo "%{name}: \$ps_msg\$ps_action See https://modpagespeed.com/docs/deployment/"
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
  if ps_stale_web_config / /var/cache/pagespeed-optimizer/\$ps_old; then
    echo "%{name}: ACTION REQUIRED: your web-server configuration still names the cache path /var/cache/pagespeed-optimizer/\$ps_old (the cache format changed and the directory moved to v3); set 'pagespeed DaemonVolumePath' (nginx) or ModPagespeedDaemonVolumePath (Apache) to /var/cache/pagespeed-optimizer/v3/cache and restart the web server; the cache starts empty and refills as traffic arrives, and the \$ps_old directory is left in place until you delete it."
  fi
done
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
  systemctl enable $PKG.service >/dev/null 2>&1 || true
  systemctl restart $PKG.service || true
fi

%preun
if [ "\$1" -eq 0 ] && [ -d /run/systemd/system ]; then
  systemctl stop $PKG.service || true
fi

%postun
ldconfig || true
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
fi
if [ "\$1" -eq 0 ]; then
  # Only the cache this package manages.  A legacy pre-2.1 cache at
  # /var/lib/$PKG is NEVER auto-deleted; the migration notes
  # document the reclaim command.
  rm -rf /var/cache/$PKG
  # The generated credentials are package-owned state, not operator config
  # the way a %config file is: erase takes them with it.
  rm -rf /etc/pagespeed-optimizer
  if [ -d /run/systemd/system ]; then
    systemctl disable $PKG.service >/dev/null 2>&1 || true
  fi
fi

%files
%license /usr/share/licenses/$PKG/LICENSE
%license /usr/share/licenses/$PKG/NOTICE
%license /usr/share/licenses/$PKG/THIRD-PARTY-NOTICES
/usr/bin/$PKG
$libdir/libpagespeed.so
/usr/lib/systemd/system/$PKG.service
# Vendor drop-in, plain payload (never %config): the operator's off switch is
# a mask under /etc/systemd, which this package does not own.
%dir /usr/lib/systemd/system/$PKG.service.d
/usr/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf
/usr/lib/sysusers.d/$PKG.conf
/usr/lib/tmpfiles.d/$PKG.conf
# %config(noreplace): operator edits survive an upgrade untouched.  The
# %attr names the pagespeed group directly -- %pre creates it before the
# payload lands -- so a REPLACED (unedited) file also lands at the env-file
# gate, 0640 root:pagespeed, without waiting for tmpfiles (#1486).
%config(noreplace) %attr(0640,root,pagespeed) /etc/default/$PKG
/usr/share/doc/$PKG/daemon.env.example
/usr/share/doc/$PKG/90-syscall-filter-off.conf.example
/usr/share/doc/$PKG/chrome-seccomp.json
%dir /etc/pagespeed-optimizer
# %ghost: generated by %post, owned by the package for removal, never
# installed from the payload and never a %config.
%ghost %attr(0640,root,pagespeed) /etc/pagespeed-optimizer/daemon.env
EOF

  OPT_STAGE="$stage" rpmbuild --quiet --define "_topdir $top" -bb \
    "$top/SPECS/$PKG.spec" >/dev/null
  local rpm_file
  rpm_file="$(find "$top/RPMS" -name "${PKG}-${version}-1.*.rpm" | head -1)"
  [[ -n "$rpm_file" ]] || { echo "error: rpmbuild produced no rpm" >&2; exit 1; }
  local out="$outdir/$(basename "$rpm_file")"
  cp "$rpm_file" "$out"
  echo "built: $out"
  echo "sha256: $(sha256sum "$out" | cut -d' ' -f1)"
}

self_test() {
  require_rpmbuild
  local tmp fails=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  printf '#!/bin/sh\n[ "$1" = --version ] && { echo stub 0.0.0; exit 0; }\nexit 1\n' \
    > "$tmp/stub"
  chmod +x "$tmp/stub"
  printf 'not an ELF object\n' > "$tmp/stub.so"

  local out
  out="$(build_rpm "0.0.0~selftest" "$tmp/stub" "$tmp/out" "0.0.0" "2.34" \
    "$tmp/stub.so")" || {
    echo "FAIL: build_rpm with stub inputs" >&2; return 1; }
  local arch rpmf
  arch="$(rpm --eval '%{_arch}')"
  rpmf="$tmp/out/${PKG}-0.0.0~selftest-1.${arch}.rpm"

  check() { # label expected actual
    if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
      echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails+1)); fi
  }
  check "rpm exists" "yes" "$([[ -f "$rpmf" ]] && echo yes)"
  check "Name" "$PKG" "$(rpm -qp --qf '%{NAME}' "$rpmf" 2>/dev/null)"
  check "Version" "0.0.0~selftest" \
    "$(rpm -qp --qf '%{VERSION}' "$rpmf" 2>/dev/null)"
  check "explicit glibc floor requires" "1" \
    "$(rpm -qp --requires "$rpmf" 2>/dev/null | grep -c "^glibc >= 2.34\$")"
  # The %pre identity creation needs useradd/groupadd. Declared once, whether
  # or not this rpmbuild host has the EL sysusers macros -- a host without
  # them silently dropped the dependency (and failed to parse the spec at all)
  # before the requirement was also stated outright.
  check "pre requires shadow-utils" "1" \
    "$(rpm -qp --requires "$rpmf" 2>/dev/null | grep -c "^shadow-utils\$")"
  local files
  files="$(rpm -qpl "$rpmf" 2>/dev/null)"
  local libdir
  case "$arch" in x86_64|aarch64) libdir="/usr/lib64" ;; esac
  for path in "/usr/bin/$PKG" "$libdir/libpagespeed.so" \
              "/usr/lib/systemd/system/$PKG.service" "/etc/default/$PKG" \
              "/usr/lib/sysusers.d/$PKG.conf" \
              "/usr/lib/tmpfiles.d/$PKG.conf" \
              "/usr/share/doc/$PKG/daemon.env.example" \
              "/usr/share/doc/$PKG/90-syscall-filter-off.conf.example" \
              "/usr/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf" \
              "/usr/share/doc/$PKG/chrome-seccomp.json"; do
    check "ships $path" "1" "$(printf '%s\n' "$files" | grep -c "^$path\$")"
  done
  # The browser profile is a VENDOR drop-in, active on install, 0644, plain
  # payload: never %config (no .rpmnew, no prompt -- the operator's off switch
  # is a mask under /etc, not an edit to this file).
  check "browser drop-in ships 0644 root:root as plain payload" "1" \
    "$(rpm -qp --qf '[%{FILENAMES} %{FILEMODES:octal} %{FILEUSERNAME} %{FILEGROUPNAME} %{FILEFLAGS:fflags}\n]' "$rpmf" 2>/dev/null \
      | grep -c "^/usr/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf 100644 root root \$")"
  check "no browser-analysis example under /usr/share/doc any more" "0" \
    "$(printf '%s\n' "$files" | grep -c "usr/share/doc/$PKG/20-browser-analysis" || true)"
  # The retired opt-in enforce example must not come back: the allow-list is
  # in the unit, and a second copy that "installs enforcement" would mislead.
  check "no retired 10-syscall-filter-enforce example in the payload" "0" \
    "$(printf '%s\n' "$files" | grep -c "10-syscall-filter-enforce" || true)"
  # The upgrade-proof half of the opt-out contract: the
  # payload owns NOTHING under /etc/systemd.  An operator's
  # 90-syscall-filter-off.conf lives there, and an rpm that owned any file
  # in that tree could replace or re-create it on upgrade.
  check "payload ships nothing under /etc/systemd (operator territory)" "0" \
    "$(printf '%s\n' "$files" | grep -c "^/etc/systemd/" || true)"
  local scripts
  scripts="$(rpm -qp --scripts "$rpmf" 2>/dev/null)"
  check "post runs ldconfig" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "^ldconfig\$")"
  check "pre creates the pagespeed user (sysusers)" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "useradd")"
  check "pre creates the pagespeed group (sysusers)" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "groupadd")"
  check "post applies tmpfiles" "1" \
    "$(printf '%s\n' "$scripts" \
      | grep -c "systemd-tmpfiles --create $PKG.conf")"
  check "post prints the cold-start notice" "1" \
    "$(printf '%s\n' "$scripts" \
      | grep -c "cold-start; no content is migrated or chowned.")"
  check "post tells the operator to repoint the web server" "1" \
    "$(printf '%s\n' "$scripts" \
      | grep -c "ACTION REQUIRED: the daemon.s default paths moved")"
  # The repoint warning must be earned before it is printed: the scriptlet
  # scans only web-server configuration the server actually loads (the
  # *-enabled/ trees, never *-available/ on its own) and skips backup and
  # leftover file names, so a stale .bak cannot warn on a host whose live
  # config is already current.
  check "post has the loaded-config scan helper" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'ps_stale_web_config() {')"
  check "scan skips the six backup/leftover name patterns" "6" \
    "$(printf '%s\n' "$scripts" | grep -o -- '--exclude=' | wc -l | tr -d ' ')"
  check "scan reads Apache through its *-enabled/ trees only" "1" \
    "$(printf '%s\n' "$scripts" \
      | grep -c 'apache2/conf-enabled apache2/sites-enabled apache2/mods-enabled')"
  check "repoint warning is gated on the scan" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'if ps_stale_web_config; then')"
  check "post never names the container module's pagespeed_cache_path" "0" \
    "$(printf '%s\n' "$scripts" | grep -c 'pagespeed_cache_path' || true)"
  check "post names the native nginx directive" "2" \
    "$(printf '%s\n' "$scripts" | grep -c "'pagespeed DaemonVolumePath'")"
  check "stale-path notice loops over v1 and v2" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'for ps_old in v1 v2; do')"
  check "stale-path notice is gated on the configured path, not the directory" "1" \
    "$(printf '%s\n' "$scripts" | grep -c -F 'if ps_stale_web_config / /var/cache/pagespeed-optimizer/$ps_old; then')"
  check "stale-path notice has no directory-presence clause" "0" \
    "$(printf '%s\n' "$scripts" | grep -c -F -- '-d /var/cache/pagespeed-optimizer/' || true)"
  check "cold-start notice stays silent for another configured volume path" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'ps_stale_web_config / DaemonVolumePath && ! ps_stale_web_config / /var/cache/pagespeed-optimizer/v3')"
  check "post has no redundant ps_stale" "0" \
    "$(printf '%s\n' "$scripts" | grep -c 'ps_stale=' || true)"
  check "post restarts service" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "systemctl restart $PKG.service")"
  check "post enables service" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "systemctl enable $PKG.service")"
  check "post reloads systemd" "1" \
    "$(printf '%s\n' "$scripts" | sed -n '/^postinstall scriptlet/,/^preuninstall scriptlet/p' | grep -c "^  systemctl daemon-reload || true\$")"
  check "ships LICENSE" "1" \
    "$(printf '%s\n' "$files" | grep -c "^/usr/share/licenses/$PKG/LICENSE\$")"
  check "ships NOTICE" "1" \
    "$(printf '%s\n' "$files" | grep -c "^/usr/share/licenses/$PKG/NOTICE\$")"
  check "ships THIRD-PARTY-NOTICES" "1" \
    "$(printf '%s\n' "$files" | grep -c "^/usr/share/licenses/$PKG/THIRD-PARTY-NOTICES\$")"
  check "postun runs ldconfig" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "^ldconfig || true\$")"
  check "postun erase removes the managed cache dir" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "rm -rf /var/cache/$PKG\$")"
  check "postun never deletes the legacy cache" "0" \
    "$(printf '%s\n' "$scripts" | grep -c "rm -rf /var/lib/$PKG" || true)"
  check "preun stops on erase" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "systemctl stop $PKG.service")"
  check "env file is config(noreplace)" "1" \
    "$(rpm -qp --qf '[%{FILENAMES} %{FILEFLAGS:fflags}\n]' "$rpmf" 2>/dev/null \
      | grep -c "^/etc/default/$PKG .*cn")"
  # #1486: the env-file gate is 0640 root:pagespeed and an upgrade replaces
  # an unedited %config with exactly these bits -- the %attr names the
  # group, which %pre creates before the payload lands.
  check "/etc/default is 0640 root:pagespeed in the payload" "1" \
    "$(rpm -qp --qf '[%{FILENAMES} %{FILEMODES:octal} %{FILEUSERNAME} %{FILEGROUPNAME} %{FILEFLAGS:fflags}\n]' "$rpmf" 2>/dev/null \
      | grep -c "^/etc/default/$PKG 100640 root pagespeed cn\$")"
  # The live env file is a %ghost -- owned by the package
  # for removal, generated by %post with real tokens, and NEVER %config (a
  # generated secret must not produce an .rpmnew or a replacement on
  # upgrade).  `g` is the ghost flag; `c` (config) must be absent.
  check "daemon.env is a 0640 root:pagespeed ghost, not config" "1" \
    "$(rpm -qp --qf '[%{FILENAMES} %{FILEMODES:octal} %{FILEUSERNAME} %{FILEGROUPNAME} %{FILEFLAGS:fflags}\n]' "$rpmf" 2>/dev/null \
      | grep -c "^/etc/pagespeed-optimizer/daemon.env 100640 root pagespeed g\$")"
  check "post provisions the API and PURGE tokens" "1" \
    "$(printf '%s\n' "$scripts" \
      | grep -c 'PAGESPEED_API_TOKEN PAGESPEED_PURGE_TOKEN')"
  check "post only writes a key that is absent" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'if ! grep -q "\^\${ps_key}=\."')"
  check "post creates the env file with a 0177 umask" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'umask 0177')"
  check "post pins the env file to 0640" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'chmod 0640 "\$ps_env"')"
  check "post pins /etc/default to 0640 on every upgrade" "1" \
    "$(printf '%s\n' "$scripts" | grep -c "chmod 0640 /etc/default/$PKG")"
  check "post never chowns" "0" \
    "$(printf '%s\n' "$scripts" \
      | grep -cE "^[[:space:]]*(chown|chgrp)[[:space:]]" || true)"
  check "postun erase removes the generated secrets" "1" \
    "$(printf '%s\n' "$scripts" | grep -c 'rm -rf /etc/pagespeed-optimizer')"
  # the unit must run the daemon unprivileged
  local unit
  unit="$(rpm2cpio "$rpmf" 2>/dev/null \
    | cpio -i --quiet --to-stdout "./usr/lib/systemd/system/$PKG.service" 2>/dev/null)"
  check "unit runs as pagespeed" "1" \
    "$(printf '%s' "$unit" | grep -c "^User=pagespeed\$")"
  check "unit rate-limits starts in [Unit]" "1" \
    "$(printf '%s' "$unit" | sed -n '/^\[Unit\]/,/^\[Service\]/p' \
      | grep -c "^StartLimitBurst=5\$")"
  check "unit has no StartLimit in [Service]" "0" \
    "$(printf '%s' "$unit" | sed -n '/^\[Service\]/,$p' \
      | grep -c "^StartLimit" || true)"
  # #1465: reload must refuse honestly (an ExecReload= that fails loudly and
  # points at restart), never fall through to systemd's SIGHUP default --
  # the daemon ignores SIGHUP, so the fallback would be a silent no-op.
  check "unit refuses reload honestly" "1" \
    "$(printf '%s' "$unit" \
      | grep -c '^ExecReload=.*reload is not supported' || true)"
  check "tmpfiles.d creates the cache dir 3770 pagespeed:pagespeed" "1" \
    "$(rpm2cpio "$rpmf" 2>/dev/null \
      | cpio -i --quiet --to-stdout "./usr/lib/tmpfiles.d/$PKG.conf" 2>/dev/null \
      | grep -cE "^d +/var/cache/pagespeed-optimizer/v3 +3770 +pagespeed +pagespeed")"
  # #1486: both env files get their group scope re-asserted by `z` lines on
  # every install/upgrade/boot.
  check "tmpfiles.d re-scopes the daemon.env secrets file" "1" \
    "$(rpm2cpio "$rpmf" 2>/dev/null \
      | cpio -i --quiet --to-stdout "./usr/lib/tmpfiles.d/$PKG.conf" 2>/dev/null \
      | grep -cE "^z +/etc/pagespeed-optimizer/daemon.env +0640 +root +pagespeed")"
  check "tmpfiles.d re-scopes the /etc/default env file" "1" \
    "$(rpm2cpio "$rpmf" 2>/dev/null \
      | cpio -i --quiet --to-stdout "./usr/lib/tmpfiles.d/$PKG.conf" 2>/dev/null \
      | grep -cE "^z +/etc/default/pagespeed-optimizer +0640 +root +pagespeed")"
  check "ships the /etc/pagespeed-optimizer dir" "1" \
    "$(printf '%s\n' "$files" | grep -c "^/etc/pagespeed-optimizer\$")"
  check "unit has RuntimeDirectory" "1" \
    "$(printf '%s' "$unit" | grep -c "^RuntimeDirectory=pagespeed-optimizer\$")"
  check "unit has no StateDirectory" "0" \
    "$(printf '%s' "$unit" | grep -c "^StateDirectory=" || true)"
  check "unit never runs root" "0" \
    "$(printf '%s' "$unit" | grep -c "^User=root\$" || true)"
  # Amended in 2.1: the shipped unit ENFORCES the syscall
  # allow-list.  Until 2.1 the load-bearing assertion here was the negative
  # one (no SystemCallFilter= may appear, the flip has to be deliberate); the
  # flip has now been made, so the assertion inverts: enforcement must not
  # silently DISAPPEAR, and it must be exactly the measured profile -- two
  # group lines and nothing else, no reset, no errno conversion.
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
  check "unit does not set PrivateUsers" "0" \
    "$(printf '%s' "$unit" | grep -c "^PrivateUsers=" || true)"
  # The daemon needs no namespace of its own; the browser-analysis drop-in
  # is what resets this for the Chrome sandbox.
  check "unit restricts namespaces (the browser drop-in re-widens)" "1" \
    "$(printf '%s' "$unit" | grep -c "^RestrictNamespaces=yes\$")"
  # ...the documented opt-out is exactly two resets and nothing else: a
  # directive smuggled into this file would be applied on every host that
  # opts out, under the name of turning something OFF.
  local optout browser chromeprof
  optout="$(rpm2cpio "$rpmf" 2>/dev/null | cpio -i --quiet --to-stdout \
    "./usr/share/doc/$PKG/90-syscall-filter-off.conf.example" 2>/dev/null)"
  check "opt-out example resets the syscall filter" "1" \
    "$(printf '%s' "$optout" | grep -c "^SystemCallFilter=\$")"
  check "opt-out example resets RestrictNamespaces" "1" \
    "$(printf '%s' "$optout" | grep -c "^RestrictNamespaces=\$")"
  check "opt-out example carries no other directive" "3" \
    "$(printf '%s' "$optout" | grep -cv '^[[:space:]]*#\|^[[:space:]]*$' || true)"
  browser="$(rpm2cpio "$rpmf" 2>/dev/null | cpio -i --quiet --to-stdout \
    "./usr/lib/systemd/system/$PKG.service.d/20-browser-analysis.conf" 2>/dev/null)"
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
  chromeprof="$(rpm2cpio "$rpmf" 2>/dev/null | cpio -i --quiet --to-stdout \
    "./usr/share/doc/$PKG/chrome-seccomp.json" 2>/dev/null)"
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
  # a bad version must refuse before staging anything
  if (build_rpm "bad-version" "$tmp/stub" "$tmp/out2" "" "2.34" "$tmp/stub.so") \
      >/dev/null 2>&1; then
    echo "FAIL: a malformed version was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: malformed version refused"
  fi
  # a missing binary must refuse
  if (build_rpm "0.0.1" "$tmp/no-such-binary" "$tmp/out3" "" "2.34" \
      "$tmp/stub.so") >/dev/null 2>&1; then
    echo "FAIL: a missing binary was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: missing binary refused"
  fi
  # a missing client library must refuse
  if (build_rpm "0.0.2" "$tmp/stub" "$tmp/out4" "" "2.34" "$tmp/no-such-lib") \
      >/dev/null 2>&1; then
    echo "FAIL: a missing client library was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: missing client library refused"
  fi
  # a version-tripwire mismatch must refuse
  if (build_rpm "0.0.3" "$tmp/stub" "$tmp/out5" "9.9.9" "2.34" "$tmp/stub.so") \
      >/dev/null 2>&1; then
    echo "FAIL: a version-tripwire mismatch was accepted" >&2; fails=$((fails+1))
  else
    echo "ok: version-tripwire mismatch refused"
  fi
  # non-ELF inputs with no explicit floor must refuse
  if (build_rpm "0.0.4" "$tmp/stub" "$tmp/out6" "" "" "$tmp/stub.so") \
      >/dev/null 2>&1; then
    echo "FAIL: underivable glibc floor was accepted" >&2; fails=$((fails+1))
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
build_rpm "$VERSION" "$BINARY" "$OUT" "$EXPECT" "$FLOOR" "$LIBRARY"
