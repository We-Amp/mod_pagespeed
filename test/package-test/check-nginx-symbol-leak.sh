#!/usr/bin/env bash
#
# check-nginx-symbol-leak.sh  —  the design record GATE 2 (symbol-leak gate)
#
# Independent, deterministic CI gate that asserts the packaged nginx
# mod_pagespeed module (.so) does NOT leak the bundled OpenSSL/BoringSSL.
# Two failure classes are fatal:
#
#   (1) DT_NEEDED on libssl/libcrypto — the module would dynamically link
#       against the host's OpenSSL at runtime. mod_pagespeed statically
#       links BoringSSL; a NEEDED on system libssl means the static link
#       broke and the module would pick up (and could clash with) the
#       host nginx's OpenSSL ABI. This has historically caused silent
#       symbol interposition / crashes.
#
#   (2) Any *exported* (dynamic, defined) symbol other than the nginx
#       module struct(s) `ngx_module*`. An nginx dynamic module must
#       export ONLY its module descriptor; any other global (e.g. a
#       leaked BoringSSL/protobuf/abseil symbol) can interpose on nginx's
#       own symbols of the same name and corrupt the process.
#
# This is the SAME assertion the Phase-B build scripts run in-build, but
# wired as an INDEPENDENT, BLOCKING CI gate per the design record D5.3 (defence in
# depth: the gate must catch a build script whose internal assertion was
# accidentally disabled or which produced a package out-of-band).
#
# Needs only binutils (readelf, nm) + dpkg-deb/rpm2cpio for extraction.
# NO nginx required — deterministic, fast, runs anywhere.
#
# Usage:
#   check-nginx-symbol-leak.sh <path>
#     <path> is one of:
#       *.so   — check the shared object directly
#       *.deb  — extract the module .so from the deb, then check
#       *.rpm  — extract the module .so from the rpm, then check
#
# Exit codes:
#   0  clean
#   1  usage / extraction error
#   2  symbol-leak assertion failed (THE blocking condition)
#
set -euo pipefail

die() { echo "::error::$*" >&2; exit "${2:-1}"; }

[[ $# -eq 1 ]] || die "usage: $0 <module.so|package.deb|package.rpm>" 1
INPUT="$1"
[[ -e "$INPUT" ]] || die "no such file: $INPUT" 1

for tool in readelf nm; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool (install binutils)" 1
done

WORK="$(mktemp -d -t ngx-symcheck.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Resolve the .so to check (extract from package if needed).
# Module layout per the design record Phase-B build-script contract:
#   deb: /usr/lib/nginx/modules/ngx_pagespeed_module.so
#   rpm: /usr/lib64/nginx/modules/ngx_pagespeed_module.so
# We don't hard-code the path — we glob for ngx_pagespeed*_module.so under
# the extracted tree so a layout tweak in Phase B doesn't silently skip the
# check (a not-found .so is a hard error, never a pass).
# ---------------------------------------------------------------------------
resolve_so() {
  case "$INPUT" in
    *.so)
      printf '%s\n' "$INPUT"
      ;;
    *.deb)
      command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not found (needed to extract .deb)" 1
      dpkg-deb -x "$INPUT" "$WORK/deb"
      _find_so "$WORK/deb"
      ;;
    *.rpm)
      mkdir -p "$WORK/rpm"
      local rpmfile; rpmfile="$(readlink -f "$INPUT")"
      if command -v rpm2cpio >/dev/null 2>&1 && command -v cpio >/dev/null 2>&1; then
        ( cd "$WORK/rpm" && rpm2cpio "$rpmfile" | cpio -idm --quiet )
      elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -x -f "$rpmfile" -C "$WORK/rpm"
      elif command -v docker >/dev/null 2>&1; then
        # Host lacks rpm2cpio/bsdtar (e.g. the Ubuntu release runner) and el9
        # rpms use a zstd payload (no pure-python path). The almalinux:9 base
        # image (already cached on the release runner) ships rpm2archive + tar
        # but NOT cpio, so extract via rpm2archive|tar (no in-container install).
        docker run --rm --user "$(id -u):$(id -g)" -v "$rpmfile":/pkg.rpm:ro -v "$WORK/rpm":/out almalinux:9 \
          bash -c "cd /out && rpm2archive - < /pkg.rpm | tar -xz"
      else
        die "no rpm extractor available (need rpm2cpio+cpio, bsdtar, or docker)" 1
      fi
      _find_so "$WORK/rpm"
      ;;
    *)
      die "unsupported input (want .so/.deb/.rpm): $INPUT" 1
      ;;
  esac
}

_find_so() {
  local root="$1" found
  found="$(find "$root" -type f -name 'ngx_pagespeed*module.so' -path '*nginx/modules/*' 2>/dev/null | head -1)"
  # Fallback: any ngx_pagespeed*.so anywhere in the package (catches a
  # layout change while still failing loudly if there is no .so at all).
  [[ -z "$found" ]] && found="$(find "$root" -type f -name 'ngx_pagespeed*.so' 2>/dev/null | head -1)"
  [[ -n "$found" ]] || die "no ngx_pagespeed module .so found inside $INPUT" 1
  printf '%s\n' "$found"
}

SO="$(resolve_so)"
echo "==> checking module: $SO"

fail=0

# ---------------------------------------------------------------------------
# Assertion 1: no DT_NEEDED on libssl / libcrypto.
# ---------------------------------------------------------------------------
echo "--- readelf -d (DT_NEEDED) ---"
NEEDED="$(readelf -d "$SO" 2>/dev/null | awk '/\(NEEDED\)/ {print}')"
printf '%s\n' "$NEEDED"
if printf '%s\n' "$NEEDED" | grep -E 'lib(ssl|crypto)\.so' >/dev/null 2>&1; then
  echo "::error::SYMBOL LEAK — module has a DT_NEEDED on libssl/libcrypto:" >&2
  printf '%s\n' "$NEEDED" | grep -E 'lib(ssl|crypto)\.so' >&2
  echo "         mod_pagespeed must statically link BoringSSL; a NEEDED on" >&2
  echo "         system libssl/libcrypto means the static link broke." >&2
  fail=1
else
  echo "OK: no libssl/libcrypto in DT_NEEDED"
fi

# ---------------------------------------------------------------------------
# Assertion 2: exported dynamic symbols are ONLY ngx module descriptors.
# `nm -D --defined-only` lists defined dynamic (exported) symbols. The only
# legitimately-exported symbols from an nginx dynamic module are its module
# struct(s) — conventionally named `ngx_<name>_module` (e.g.
# `ngx_pagespeed_module`, `ngx_http_pagespeed_module`) and, on older
# toolchains, the `ngx_modules`/`ngx_module*` table. Anything else is a leak
# that can interpose on nginx.
# ---------------------------------------------------------------------------
echo "--- nm -D --defined-only (exported symbols) ---"
# Columns: <addr> <type> <name>. We want the name (field 3) for entries that
# have an address+type; skip undefined (U) / no-name lines defensively.
EXPORTED="$(nm -D --defined-only "$SO" 2>/dev/null | awk 'NF>=3 {print $3}')"
echo "exported symbols:"
printf '%s\n' "$EXPORTED" | sed 's/^/    /'
# Allow-list: any ngx_* symbol whose name contains "module" — covers
# ngx_<name>_module (the module descriptor), ngx_modules, ngx_module_* tables.
# Empty lines (no exports at all) are fine.
LEAKED="$(printf '%s\n' "$EXPORTED" | grep -vE '^$' | grep -vE '^ngx_[A-Za-z0-9_]*module' || true)"
if [[ -n "$LEAKED" ]]; then
  echo "::error::SYMBOL LEAK — module exports symbols other than ngx_module*:" >&2
  echo "$LEAKED" | sed 's/^/    /' >&2
  echo "         An nginx dynamic module must export ONLY its ngx_module*" >&2
  echo "         descriptor; any other exported global can interpose on" >&2
  echo "         nginx's own symbols and corrupt the worker process." >&2
  fail=1
else
  echo "OK: only ngx_module* symbols are exported"
fi

if [[ "$fail" -ne 0 ]]; then
  die "nginx module symbol-leak gate FAILED for $SO" 2
fi

echo "==> symbol-leak gate PASSED for $SO"
