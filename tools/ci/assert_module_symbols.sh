#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# assert_module_symbols.sh -- prove a built server module binds its crypto to
# the copy it ships.
#
# The Apache and nginx modules link a private, static BoringSSL and hide it
# with a linker version script. They are loaded into server processes that
# usually have the system OpenSSL loaded as well (mod_ssl, nginx's own TLS).
# A shared object may be linked with unresolved references, so a build that
# lost the static crypto archive -- or that started calling a function the
# private copy does not have -- still links. The dynamic linker then binds the
# reference to whatever the host process offers: a different library, with
# different behaviour and its own process-exit handling. Nothing fails until
# the module runs.
#
# Three assertions on one ELF shared object; any violation exits non-zero and
# prints the offending names:
#
#   1. NEEDED names no crypto provider. The module carries its own; a
#      libssl/libcrypto dependency means the static link broke.
#   2. Every UNDEFINED dynamic symbol is accounted for. This is the decisive
#      one, and it is a positive rule, not a list of bad names:
#        - a versioned reference (name@GLIBC_2.14) was bound at link time to
#          one of the NEEDED libraries; its version must belong to the C/C++
#          runtime (ALLOWED_VERSION_RE);
#        - an unversioned reference is satisfied by whatever the host process
#          has loaded, so it must be on the short allow-list in
#          allowed_undefined(): the server's own API, and a handful of weak
#          runtime hooks.
#      Anything else fails, whichever library it would have come from. A name
#      that also belongs to a crypto-library family is marked as such in the
#      output (CRYPTO_FAMILIES) -- that is a diagnostic, not the gate.
#   3. The exported dynamic symbols are only the ones the profile allows, i.e.
#      the version script took effect.
#
# Profiles (--profile):
#   apache      //:libmod_pagespeed.so   exports: pagespeed_module, and the
#               apr_* / apu_* / ap_* names that pagespeed/apache/
#               mod_pagespeed.lds keeps global on purpose
#   nginx       ngx_pagespeed_module.so  exports: ngx_module*
#   client-lib  the optimizer client library the module loads at run time.
#               It is built without a crypto library and exports its own C
#               API, so assertions 1 and 2 apply and 3 is skipped.
#
# Usage:
#   assert_module_symbols.sh --profile <apache|nginx|client-lib> <file>
#   assert_module_symbols.sh --self-test [--require-fixtures]
#
# <file> is the shared object, or a .deb / .rpm that contains it (the object is
# then extracted first; a package without the object is an error, not a pass).
#
# Needs readelf (binutils); dpkg-deb for a .deb; rpm2cpio+cpio, rpm2archive,
# bsdtar or docker for an .rpm. A missing tool is an error. readelf is used for
# the symbol table as well as for NEEDED because it prints symbol versions in
# every binutils release in use; nm -D only does from 2.35 on. READELF may be
# set to a substitute command; the self-test uses that to feed recorded tool
# output through the real assertions.
#
# Exit status: 0 clean, 1 usage/tool/extraction error, 2 an assertion failed.
#
# Out of scope: the Envoy filter library (built without a version script and
# loaded into a server that itself carries BoringSSL -- it does not have the
# private-copy property by design) and the IIS module (a Windows DLL; this is
# an ELF check).

set -euo pipefail

# ---------------------------------------------------------------------------
# Crypto provider libraries (assertion 1)
# ---------------------------------------------------------------------------
# The only crypto the modules link is BoringSSL's libcrypto and libssl (see
# @boringssl in bazel/repositories.bzl; curl and the cache library use the
# same copy). Those are the two sonames the system OpenSSL, LibreSSL and
# BoringSSL packages all install, so those are the two to refuse. "libcrypt"
# (the password-hashing library that libc-based servers link) is unrelated
# and is not matched.
CRYPTO_NEEDED_RE='^lib(ssl|crypto)([^a-z]|$)'

# ---------------------------------------------------------------------------
# Undefined symbols (assertion 2)
# ---------------------------------------------------------------------------
# Version namespaces a versioned undefined symbol may carry: glibc, libstdc++
# (GLIBCXX, CXXABI) and libgcc. These are every namespace the real modules
# use. A reference versioned by anything else (OPENSSL_3.0.0, ...) fails.
ALLOWED_VERSION_RE='^(GLIBC|GLIBCXX|CXXABI|GCC)_'

# The allow-list for UNVERSIONED undefined symbols, in one place. One line per
# entry: <profile|all> <weak|any> <exact|prefix> <name>.
#   weak  the reference must be weak (the module works when it is absent);
#   any   a strong reference is fine.
# Derived from the dynamic symbol tables of the real modules: the x64 and
# arm64 CI builds of both modules, the released Apache packages and bare
# objects, the released nginx packages for every supported distribution, and
# the optimizer client library. Nothing else occurs in any of them. Add an
# entry only for a name the HOST is meant to provide.
allowed_undefined() {
  # The Apache httpd API: the server the module is loaded into provides it.
  echo "apache any prefix ap_"
  # APR and APR-util, the server's runtime library. The version script keeps
  # these names global so that calls bind to the server's copies.
  echo "apache any prefix apr_"
  echo "apache any prefix apu_"
  # The nginx API: the nginx binary the module is loaded into provides it.
  echo "nginx any prefix ngx_"
  # C math functions. The nginx module built by nginx's own link has no libm
  # in NEEDED on some distributions and reaches libm through the C++ runtime
  # it does depend on, so these stay unversioned there. Exact names only.
  local f
  for f in ceil ceilf cos exp exp2 exp2f expf floor floorf fmod llround log \
           log10 log10f log1p log1pf log2 log2f logf lrint lrintf lround nan \
           nanf pow powf rint round roundf sqrt sqrtf tanh; do
    echo "nginx any exact ${f}"
  done
  # Profiling start hook, referenced weakly by the C start files.
  echo "all weak exact __gmon_start__"
  # Transactional-memory registration and helpers, referenced weakly by the
  # C start files and by the C++ runtime's transactional clones.
  echo "all weak prefix _ITM_"
  # Transactional clones of operator new/delete, referenced weakly by the
  # statically linked C++ runtime.
  echo "all weak prefix _ZGTt"
  # Coverage-data flush hooks, referenced weakly by the statically linked
  # compiler runtime's fork/exec wrappers.
  echo "all weak prefix __gcov_"
  # Unwind-table registration, referenced weakly by the C start files of
  # toolchains that still emit it (seen in the client library).
  echo "all weak exact __register_frame_info"
  echo "all weak exact __deregister_frame_info"
}

# Crypto symbol families: a DIAGNOSTIC label on names assertion 2 rejects, so
# the log says "this is a crypto symbol" and not only "this is not allowed".
#
# Derived from the public headers of the pinned BoringSSL (BORINGSSL_VERSION in
# bazel/repositories.bzl, 0.20260508.0 when this list was made):
#
#   cat include/openssl/*.h | tr '\n' ' ' \
#     | grep -oE 'OPENSSL_EXPORT[^;{]*\(' \
#     | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\($' | tr -d ' (' \
#     | sort -u | sed -E 's/^([A-Za-z0-9]+)_.*/\1_/' | sort | uniq -c
#
# 2874 function names in about 120 families, matched as case-sensitive
# prefixes. Families whose first word is an ordinary English word are narrowed
# to their full type name; OSSL_/ossl_ (OpenSSL 3) are added. The list is not
# complete for OpenSSL or LibreSSL (they have families BoringSSL lacks) and
# does not need to be: the allow-list above is what rejects a name.
CRYPTO_FAMILIES='
SSL_ SSLv23_ SSLeay TLS_ TLSv1_ DTLS_ DTLSv1_
EVP_ CRYPTO_ OPENSSL_ OpenSSL_ BORINGSSL_ OSSL_ ossl_ FIPS_ ERR_ ENGINE_
SHA1 SHA224 SHA256 SHA384 SHA512 MD4 MD5 RIPEMD160 BLAKE2B256 SIPHASH_
HMAC HKDF CMAC_ AES_ DES_ RC4 BF_ CAST_ CTR_DRBG_ RAND_
BN_ EC_ ECDSA_ ECDH_ ED25519_ X25519 RSA_ RSAPublicKey_ RSAPrivateKey_
DSA_ DSAparams_ DH_ DHparams_ HRSS_ SPAKE2_ XWING_
MLDSA44_ MLDSA65_ MLDSA87_ MLKEM768_ MLKEM1024_ SLHDSA_ TRUST_TOKEN_
ASN1_ BIO_ BUF_ CBS_ CBB_ OBJ_ PEM_ CONF_ NCONF_ CMS_
PKCS5_ PKCS7_ PKCS8_ PKCS12_ NETSCAPE_SPK
X509_ X509V3_ GENERAL_NAME GENERAL_SUBTREE_ NAME_CONSTRAINTS_
POLICY_CONSTRAINTS_ POLICY_MAPPING_ POLICYINFO_ POLICYQUALINFO_
CERTIFICATEPOLICIES_ USERNOTICE_ NOTICEREF_ DISPLAYTEXT_ DIRECTORYSTRING_
OTHERNAME_ EDIPARTYNAME_ ACCESS_DESCRIPTION_ AUTHORITY_INFO_ACCESS_
AUTHORITY_KEYID_ BASIC_CONSTRAINTS_ EXTENDED_KEY_USAGE_ CRL_DIST_POINTS_
DIST_POINT_ ISSUING_DIST_POINT_
d2i_ i2d_ i2a_ a2i_ i2s_ s2i_ i2c_ c2i_ i2o_ o2i_ i2t_ sk_ lh_
'
CRYPTO_LABEL='[crypto-library family]'

READELF="${READELF:-readelf}"

die() { echo "ERROR: $*" >&2; exit 1; }

usage() {
  echo "usage: $0 --profile <apache|nginx|client-lib> <file.so|file.deb|file.rpm>" >&2
  echo "       $0 --self-test [--require-fixtures]" >&2
  exit 1
}

# run_tool <description> <command...> -- stdout of the command. Its stderr is
# kept out of the parsed text; a failing tool is an error, with its stderr.
run_tool() {
  local what="$1" errf rc=0
  shift
  errf="$(mktemp)"
  "$@" 2>"${errf}" || rc=$?
  if [ "${rc}" -ne 0 ]; then
    echo "ERROR: ${what} failed (exit ${rc}):" >&2
    sed 's/^/   /' "${errf}" >&2
    rm -f "${errf}"
    exit 1
  fi
  if [ -s "${errf}" ]; then
    echo "note: ${what} wrote to stderr (not parsed):" >&2
    sed 's/^/   /' "${errf}" >&2
  fi
  rm -f "${errf}"
}

# Dynamic symbols from `readelf -W --dyn-syms` on stdin, as
# "<U|D> <WEAK|GLOBAL> <name[@version]>" -- U undefined, D defined.
dyn_symbols() {
  awk '$1 ~ /^[0-9]+:$/ && NF >= 8 && ($5 == "GLOBAL" || $5 == "WEAK") {
         print ($7 == "UND" ? "U" : "D"), $5, $8
       }'
}

# Rejected undefined symbols, one per line, from "<bind> <name[@version]>"
# lines on stdin. Plain string comparison throughout: no pattern can lose an
# anchor.
reject_undefined() {
  local profile="$1"
  # The two multi-line tables travel in the environment: not every awk
  # accepts a newline in a -v assignment.
  AMS_TABLE="$(allowed_undefined)" AMS_FAMILIES="${CRYPTO_FAMILIES}" \
  awk -v profile="${profile}" -v version_re="${ALLOWED_VERSION_RE}" \
      -v label="${CRYPTO_LABEL}" '
    BEGIN {
      table = ENVIRON["AMS_TABLE"]; families = ENVIRON["AMS_FAMILIES"]
      n = split(table, rows, "\n")
      for (i = 1; i <= n; i++) {
        if (split(rows[i], f, " ") != 4) continue
        if (f[1] != "all" && f[1] != profile) continue
        cnt++; a_bind[cnt] = f[2]; a_kind[cnt] = f[3]; a_name[cnt] = f[4]
      }
      nfam = split(families, fam, /[ \n]+/)
    }
    function allowed(bind, name,   i) {
      for (i = 1; i <= cnt; i++) {
        if (a_bind[i] == "weak" && bind != "WEAK") continue
        if (a_kind[i] == "exact" && name == a_name[i]) return 1
        if (a_kind[i] == "prefix" && index(name, a_name[i]) == 1) return 1
      }
      return 0
    }
    function crypto(name,   i) {
      for (i = 1; i <= nfam; i++)
        if (fam[i] != "" && index(name, fam[i]) == 1) return 1
      return 0
    }
    {
      bind = $1; sym = $2; why = ""
      at = index(sym, "@")
      if (at > 0) {
        name = substr(sym, 1, at - 1)
        ver = substr(sym, at + 1); sub(/^@/, "", ver)
        if (ver !~ version_re) why = "version " ver " is not a C/C++ runtime version"
      } else {
        name = sym
        if (!allowed(bind, name))
          why = (bind == "WEAK" ? "weak, " : "") "unversioned and not on the allow-list"
      }
      if (why != "") print sym "  (" why ")" (crypto(name) ? "  " label : "")
    }' | sort -u
}

# check_object <profile> <file> -- the three assertions. Returns 0 or 2.
check_object() {
  local profile="$1" so="$2" rc=0
  local export_re="" export_desc="" entry_re=""
  case "${profile}" in
    apache)
      export_re='^(pagespeed_module$|apr_|apu_|ap_)'
      export_desc="pagespeed_module and apr_*/apu_*/ap_*"
      entry_re='^pagespeed_module$' ;;
    nginx)
      export_re='^ngx_module'; export_desc="ngx_module*"
      entry_re='^ngx_modules$' ;;
    client-lib) ;;
    *) die "unknown profile: ${profile}" ;;
  esac

  echo "=== module symbol check (${profile}): ${so} ==="

  # Capture each tool's output once and match in memory: a pipe into an
  # early-exiting grep under pipefail reports the writer's SIGPIPE as failure.
  local dyn syms
  dyn="$(run_tool "${READELF} -d ${so}" "${READELF}" -W -d "${so}")" || exit 1
  syms="$(run_tool "${READELF} --dyn-syms ${so}" "${READELF}" -W --dyn-syms "${so}")" || exit 1
  syms="$(dyn_symbols <<<"${syms}")"

  local needed undefined exported
  needed="$(awk '/\(NEEDED\)/ { n = $NF; gsub(/[][]/, "", n); print n }' <<<"${dyn}")"
  undefined="$(awk '$1 == "U" { print $2, $3 }' <<<"${syms}")"
  exported="$(awk '$1 == "D" { n = $3; sub(/@.*/, "", n); print n }' <<<"${syms}" | sort -u)"

  # A shared object with no NEEDED entries and no undefined symbols at all is
  # not a module that was read correctly; refuse to call that clean.
  if [ -z "${needed}" ] || [ -z "${undefined}" ]; then
    die "no NEEDED entries or no undefined dynamic symbols read from ${so} -- not a dynamically linked ELF object, or the tools could not read it"
  fi

  # --- 1. NEEDED ------------------------------------------------------------
  echo "-- NEEDED --"; sed 's/^/   /' <<<"${needed}"
  local bad_needed
  bad_needed="$(grep -E "${CRYPTO_NEEDED_RE}" <<<"${needed}" || true)"
  if [ -n "${bad_needed}" ]; then
    echo "FAIL: crypto library in NEEDED -- the module must carry its own copy:" >&2
    sed 's/^/   /' <<<"${bad_needed}" >&2
    rc=2
  else
    echo "OK: no libssl/libcrypto in NEEDED."
  fi

  # --- 2. undefined symbols -------------------------------------------------
  local n_undef n_unver bad_undef
  n_undef="$(grep -c . <<<"${undefined}" || true)"
  n_unver="$(grep -vc '@' <<<"${undefined}" || true)"
  bad_undef="$(reject_undefined "${profile}" <<<"${undefined}")"
  if [ -n "${bad_undef}" ]; then
    echo "FAIL: undefined symbol(s) the host process would resolve, not the module or its declared libraries:" >&2
    sed 's/^/   /' <<<"${bad_undef}" >&2
    rc=2
  else
    echo "OK: ${n_undef} undefined dynamic symbols, ${n_unver} unversioned; every one is a runtime-library version or on the allow-list."
  fi

  # --- 3. exported symbols --------------------------------------------------
  local n_export
  n_export="$(grep -c . <<<"${exported}" || true)"
  if [ -n "${export_re}" ]; then
    echo "-- exported dynamic symbols (${n_export}) --"; sed 's/^/   /' <<<"${exported}"
    local bad_export
    bad_export="$(grep -v '^$' <<<"${exported}" | grep -vE "${export_re}" || true)"
    if [ -n "${bad_export}" ]; then
      echo "FAIL: unexpected exported symbol(s) -- the version script did not hide the module's internals:" >&2
      sed 's/^/   /' <<<"${bad_export}" >&2
      rc=2
    elif ! grep -qE "${entry_re}" <<<"${exported}"; then
      echo "FAIL: the module entry point is not exported (expected a symbol matching ${entry_re})." >&2
      rc=2
    else
      echo "OK: ${n_export} exported symbol(s), only ${export_desc}."
    fi
  else
    echo "-- exported dynamic symbols: not asserted for ${profile} (${n_export} exported) --"
  fi

  if [ "${rc}" -ne 0 ]; then
    echo "=== module symbol check FAILED (${profile}): ${so} ===" >&2
    return 2
  fi
  echo "=== module symbol check PASSED (${profile}) ==="
}

# Resolve <input> to the object to check, extracting a package into $3.
resolve_object() {
  local profile="$1" input="$2" work="$3" pattern
  case "${profile}" in
    apache)     pattern='*mod_pagespeed*.so' ;;
    nginx)      pattern='ngx_pagespeed*.so' ;;
    client-lib) pattern='libpagespeed.so*' ;;
    *) die "unknown profile: ${profile}" ;;
  esac
  case "${input}" in
    *.deb)
      command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not found (needed to unpack ${input})"
      dpkg-deb -x "${input}" "${work}"
      ;;
    *.rpm)
      local abs
      abs="$(cd "$(dirname "${input}")" && pwd)/$(basename "${input}")"
      if command -v rpm2cpio >/dev/null 2>&1 && command -v cpio >/dev/null 2>&1; then
        ( cd "${work}" && rpm2cpio "${abs}" | cpio -idm --quiet )
      elif command -v rpm2archive >/dev/null 2>&1; then
        ( cd "${work}" && rpm2archive - < "${abs}" | tar -xz )
      elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -x -f "${abs}" -C "${work}"
      elif command -v docker >/dev/null 2>&1; then
        # Last resort for a host with none of the above: unpack in a stock
        # EL container, which ships rpm2archive.
        docker run --rm --user "$(id -u):$(id -g)" \
          -v "${abs}":/pkg.rpm:ro -v "${work}":/out almalinux:9 \
          bash -c "cd /out && rpm2archive - < /pkg.rpm | tar -xz"
      else
        die "no rpm extractor found (need rpm2cpio+cpio, rpm2archive, bsdtar or docker) for ${input}"
      fi
      ;;
    *)
      printf '%s\n' "${input}"
      return 0
      ;;
  esac
  local found
  found="$(find "${work}" -type f -name "${pattern}" | sort | head -n 1)"
  [ -n "${found}" ] || die "no ${pattern} inside ${input}"
  printf '%s\n' "${found}"
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
# Two layers. The first feeds recorded readelf output through the real
# assertions via the READELF override, so it runs on any host, including one
# without binutils. The second builds real fixture objects -- with and without
# an undefined EVP_ reference, with and without a version script -- and runs
# when a C compiler and binutils are present. It says so when it does not run;
# --require-fixtures turns that into a failure.
self_test() {
  local require_fixtures="${1:-0}" work fails=0
  work="$(mktemp -d)"
  # shellcheck disable=SC2064
  trap "rm -rf '${work}'" EXIT

  # Mock readelf: print the recorded file that matches the invocation, and
  # whatever the case wants on stderr / as exit status.
  cat > "${work}/readelf" <<'MOCK'
#!/bin/bash
[ -f "${MOCK_DIR}/stderr" ] && cat "${MOCK_DIR}/stderr" >&2
case "$*" in
  *--dyn-syms*) cat "${MOCK_DIR}/symbols" ;;
  *" -d "*)     cat "${MOCK_DIR}/dynamic" ;;
  *) exit 64 ;;
esac
[ -f "${MOCK_DIR}/status" ] && exit "$(cat "${MOCK_DIR}/status")"
exit 0
MOCK
  chmod +x "${work}/readelf"

  # und/def <BIND> <name> -- one line of `readelf -W --dyn-syms`, as GNU
  # readelf prints it (a versioned undefined name is followed by " (N)").
  und() {
    case "$2" in
      *@*) printf '    12: 0000000000000000     0 FUNC    %-6s DEFAULT  UND %s (3)\n' "$1" "$2" ;;
      *)   printf '    12: 0000000000000000     0 NOTYPE  %-6s DEFAULT  UND %s\n' "$1" "$2" ;;
    esac
  }
  def() { printf '   640: 00000000019d2a10    24 OBJECT  %-6s DEFAULT   26 %s\n' "$1" "$2"; }
  header='
Symbol table '"'"'.dynsym'"'"' contains 700 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND
     1: 0000000000001000     0 SECTION LOCAL  DEFAULT   12 .text'

  local needed_ok needed_ssl
  needed_ok=' 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libcrypt.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000000e (SONAME)             Library soname: [libmod_pagespeed.so]'
  needed_ssl="${needed_ok}
 0x0000000000000001 (NEEDED)             Shared library: [libcrypto.so.3]"

  # Imports of the kind the real modules have. Everything here must pass:
  # runtime-library versions, the weak runtime hooks, and each profile's
  # server API -- including API names that CONTAIN a crypto family
  # (ngx_conf_set_bitmask_slot has "sk_", apr__SHA256_Init has "SHA256").
  local undef_common undef_apache undef_nginx undef_client
  undef_common="$(
    und GLOBAL 'getentropy@GLIBC_2.25'; und GLOBAL 'getrandom@GLIBC_2.25'
    und GLOBAL 'memcpy@GLIBC_2.14';     und GLOBAL 'dlopen@GLIBC_2.34'
    und GLOBAL '__stack_chk_fail@GLIBC_2.4'
    und GLOBAL '_Unwind_Resume@GCC_3.0'
    und GLOBAL '_ZNSt13random_device7_M_initERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE@GLIBCXX_3.4.21'
    und GLOBAL '__cxa_begin_catch@CXXABI_1.3'
    und WEAK '__gmon_start__';  und WEAK '_ITM_registerTMCloneTable'
    und WEAK '_ITM_deregisterTMCloneTable'; und WEAK '_ITM_RU1'
    und WEAK '_ZGTtnam';        und WEAK '_ZGTtdlPv'
    und WEAK '__gcov_dump';     und WEAK '__gcov_flush')"
  undef_apache="${undef_common}
$(und GLOBAL ap_hook_post_config; und GLOBAL ap_ssl_var_lookup
  und GLOBAL ap_random_insecure_bytes; und GLOBAL apr__SHA256_Init
  und GLOBAL apr_md5_init; und GLOBAL apu_version)"
  undef_nginx="${undef_common}
$(und GLOBAL ngx_conf_set_bitmask_slot; und GLOBAL ngx_ssl_get_protocol
  und GLOBAL ngx_crypt; und GLOBAL ngx_md5_update; und GLOBAL ngx_sha1_init
  und GLOBAL ceil; und GLOBAL log2f; und GLOBAL tanh)"
  undef_client="${undef_common}
$(und WEAK __register_frame_info; und WEAK __deregister_frame_info)"

  local def_apache def_nginx
  def_apache="$(def GLOBAL pagespeed_module; def GLOBAL apr_allocator_create
                def GLOBAL apr__SHA256_Init)"
  def_nginx="$(def GLOBAL ngx_module_names; def GLOBAL ngx_module_order
               def GLOBAL ngx_modules)"

  # set_mock <dynamic> <undefined-lines> <defined-lines>
  set_mock() {
    rm -rf "${work}/mock"; mkdir -p "${work}/mock"
    printf '%s\n' "$1" > "${work}/mock/dynamic"
    printf '%s\n%s\n%s\n' "${header}" "$2" "$3" > "${work}/mock/symbols"
  }
  # run_case <label> <profile> <want-rc> [<text>...] -- every <text> must be
  # in the output; a <text> starting with "!" must NOT be; "=" + text must be
  # a whole output line.
  LAST_OUT=""
  run_case() {
    local label="$1" profile="$2" want="$3" out rc=0 t
    shift 3
    out="$(MOCK_DIR="${work}/mock" READELF="${work}/readelf" \
           bash "$0" --profile "${profile}" "${work}/mock/dynamic" 2>&1)" || rc=$?
    LAST_OUT="${out}"
    if [ "${rc}" -ne "${want}" ]; then
      echo "  FAIL ${label}: exit ${rc}, expected ${want}"; sed 's/^/      /' <<<"${out}"
      fails=$((fails + 1)); return
    fi
    for t in "$@"; do
      case "${t}" in
        '!'*) if grep -qF -- "${t#!}" <<<"${out}"; then
                echo "  FAIL ${label}: output must not contain '${t#!}'"; sed 's/^/      /' <<<"${out}"
                fails=$((fails + 1)); return
              fi ;;
        '='*) if ! grep -qxF -- "${t#=}" <<<"${out}"; then
                echo "  FAIL ${label}: output lacks the line '${t#=}'"; sed 's/^/      /' <<<"${out}"
                fails=$((fails + 1)); return
              fi ;;
        *)    if ! grep -qF -- "${t}" <<<"${out}"; then
                echo "  FAIL ${label}: output lacks '${t}'"; sed 's/^/      /' <<<"${out}"
                fails=$((fails + 1)); return
              fi ;;
      esac
    done
    echo "  ok   ${label}"
  }
  local NOT_ALLOWED="(unversioned and not on the allow-list)"
  local WEAK_NOT_ALLOWED="(weak, unversioned and not on the allow-list)"

  echo "-- recorded tool output --"
  set_mock "${needed_ok}" "${undef_apache}" "${def_apache}"
  run_case "clean apache module passes" apache 0 "PASSED" "3 exported symbol(s)"
  set_mock "${needed_ok}" "${undef_nginx}" "${def_nginx}"
  run_case "clean nginx module passes" nginx 0 "PASSED"
  set_mock "${needed_ok}" "${undef_client}" "${def_nginx}"
  run_case "clean client library passes, exports not asserted" client-lib 0 "not asserted"

  # NEEDED.
  set_mock "${needed_ssl}" "${undef_apache}" "${def_apache}"
  run_case "libcrypto in NEEDED fails" apache 2 "libcrypto.so.3"
  set_mock "${needed_ok}
 0x0000000000000001 (NEEDED)             Shared library: [libssl.so.1.1]" "${undef_nginx}" "${def_nginx}"
  run_case "libssl in NEEDED fails" nginx 2 "libssl.so.1.1"

  # The allow-list is the gate: the real finding, and names from no known
  # family at all.
  set_mock "${needed_ok}" "${undef_apache}
$(und WEAK OPENSSL_memory_alloc; und WEAK OPENSSL_memory_free; und WEAK OPENSSL_memory_get_size)" "${def_apache}"
  run_case "the three weak allocator hooks fail" apache 2 \
    "=   OPENSSL_memory_alloc  ${WEAK_NOT_ALLOWED}  ${CRYPTO_LABEL}" \
    "OPENSSL_memory_free" "OPENSSL_memory_get_size"
  local sym
  for sym in TS_RESP_new OCSP_response_status UI_new SRP_Calc_A Camellia_encrypt \
             WHIRLPOOL b2i_PrivateKey totally_unknown_function inflate; do
    set_mock "${needed_ok}" "${undef_nginx}
$(und GLOBAL "${sym}")" "${def_nginx}"
    run_case "undefined ${sym} (in no listed family) fails" nginx 2 \
      "=   ${sym}  ${NOT_ALLOWED}"
  done

  # Every crypto family, one name each, in ONE module: each must be rejected
  # and each must carry the diagnostic label. Driven by the list itself, so a
  # family cannot be dropped or mistyped without a case failing.
  local fam famlines="" n_fam=0 missing=""
  for fam in ${CRYPTO_FAMILIES}; do
    famlines="${famlines}$(und GLOBAL "${fam}selftest")
"
    n_fam=$((n_fam + 1))
  done
  set_mock "${needed_ok}" "${undef_apache}
${famlines}" "${def_apache}"
  run_case "all ${n_fam} crypto families fail" apache 2
  for fam in ${CRYPTO_FAMILIES}; do
    grep -qxF -- "   ${fam}selftest  ${NOT_ALLOWED}  ${CRYPTO_LABEL}" <<<"${LAST_OUT}" \
      || missing="${missing} ${fam}"
  done
  # The loop above is driven by the list, so it cannot see an entry that was
  # deleted. Pin the size; change this number together with the list.
  local want_fam=112
  if [ "${n_fam}" -ne "${want_fam}" ]; then
    echo "  FAIL CRYPTO_FAMILIES has ${n_fam} entries, the self-test expects ${want_fam}"; fails=$((fails + 1))
  fi
  if [ -n "${missing}" ]; then
    echo "  FAIL crypto families not rejected with the label:${missing}"; fails=$((fails + 1))
  else
    echo "  ok   each of the ${n_fam} families is rejected and labelled"
  fi

  # Anchoring, in both directions. A name that merely CONTAINS an allowed
  # prefix is not allowed; a name that merely contains a family is not
  # labelled as crypto.
  set_mock "${needed_ok}" "${undef_nginx}
$(und GLOBAL my_ngx_helper; und GLOBAL xap_thing; und GLOBAL ceil_extra; und GLOBAL myceil)" "${def_nginx}"
  run_case "names that only contain an allowed prefix fail" nginx 2 \
    "=   my_ngx_helper  ${NOT_ALLOWED}" "=   xap_thing  ${NOT_ALLOWED}" \
    "=   ceil_extra  ${NOT_ALLOWED}" "=   myceil  ${NOT_ALLOWED}"
  set_mock "${needed_ok}" "${undef_apache}
$(und GLOBAL task_num_EVP_x)" "${def_apache}"
  run_case "a name that only contains a family is rejected without the label" apache 2 \
    "=   task_num_EVP_x  ${NOT_ALLOWED}"

  # Profiles do not share server APIs; math names are nginx-only.
  set_mock "${needed_ok}" "${undef_common}
$(und GLOBAL ngx_palloc)" "${def_apache}"
  run_case "nginx API name fails in the apache profile" apache 2 "=   ngx_palloc  ${NOT_ALLOWED}"
  set_mock "${needed_ok}" "${undef_common}
$(und GLOBAL ap_hook_post_config)" "${def_nginx}"
  run_case "httpd API name fails in the nginx profile" nginx 2 "=   ap_hook_post_config  ${NOT_ALLOWED}"
  set_mock "${needed_ok}" "${undef_common}
$(und GLOBAL ap_hook_post_config)" "${def_nginx}"
  run_case "server API name fails in the client library" client-lib 2 "ap_hook_post_config"
  set_mock "${needed_ok}" "${undef_common}
$(und GLOBAL ceil)" "${def_apache}"
  run_case "unversioned math name fails in the apache profile" apache 2 "=   ceil  ${NOT_ALLOWED}"

  # Runtime hooks must be weak.
  set_mock "${needed_ok}" "${undef_apache}
$(und GLOBAL __gcov_other; und GLOBAL _ITM_strong)" "${def_apache}"
  run_case "a strong reference to a weak-only hook fails" apache 2 \
    "=   __gcov_other  ${NOT_ALLOWED}" "=   _ITM_strong  ${NOT_ALLOWED}"

  # Versions: the version is what is judged, and the name before it is
  # reported whole.
  set_mock "${needed_ok}" "${undef_nginx}
$(und GLOBAL 'EVP_sha256@OPENSSL_3.0.0')" "${def_nginx}"
  run_case "a reference versioned by a crypto library fails" nginx 2 \
    "=   EVP_sha256@OPENSSL_3.0.0  (version OPENSSL_3.0.0 is not a C/C++ runtime version)  ${CRYPTO_LABEL}"
  set_mock "${needed_ok}" "${undef_nginx}
$(und GLOBAL 'inflate@ZLIB_1.2.3'; und GLOBAL 'x@XGLIBC_2.2'; und GLOBAL 'y@MYGCC_1')" "${def_nginx}"
  run_case "any other version namespace fails" nginx 2 \
    "inflate@ZLIB_1.2.3" "x@XGLIBC_2.2" "y@MYGCC_1"
  set_mock "${needed_ok}" "${undef_common}
$(und GLOBAL 'EVP_sha256@GLIBC_2.2.5'; und GLOBAL 'totally_unknown_function@GLIBC_2.2.5')" "${def_nginx}"
  run_case "a runtime-versioned name passes whatever it is called" client-lib 0 "PASSED"

  # Exports.
  set_mock "${needed_ok}" "${undef_apache}" "${def_apache}
$(def GLOBAL EVP_sha256)"
  run_case "apache module exporting EVP_sha256 fails" apache 2 "=   EVP_sha256"
  set_mock "${needed_ok}" "${undef_nginx}" "${def_nginx}
$(def GLOBAL _ZN6google8protobuf7MessageD2Ev)"
  run_case "nginx module exporting an internal fails" nginx 2 "_ZN6google8protobuf7MessageD2Ev"
  set_mock "${needed_ok}" "${undef_nginx}" "${def_nginx}
$(def GLOBAL __start_pb_defaults; def GLOBAL __stop_pb_defaults)"
  run_case "exported section bounds fail" nginx 2 "=   __start_pb_defaults" "=   __stop_pb_defaults"
  set_mock "${needed_ok}" "${undef_apache}" "${def_nginx}"
  run_case "apache profile on nginx exports fails" apache 2 "ngx_modules"
  set_mock "${needed_ok}" "${undef_apache}" ""
  run_case "module with no exports fails" apache 2 "entry point"
  set_mock "${needed_ok}" "${undef_apache}" "$(def GLOBAL apr_allocator_create)"
  run_case "apache module without pagespeed_module fails" apache 2 "entry point"
  set_mock "${needed_ok}" "${undef_apache}" "${def_apache}
$(def WEAK my_ap_helper)"
  run_case "an export that only contains an allowed prefix fails" apache 2 "=   my_ap_helper"

  # Tool trouble is an error, never a pass, and never parsed as symbols.
  set_mock "" "" ""
  run_case "empty tool output is an error, not a pass" apache 1 "not a dynamically linked ELF"
  set_mock "${needed_ok}" "${undef_apache}" "${def_apache}"
  echo 2 > "${work}/mock/status"
  echo "readelf: Error: not an ELF file" > "${work}/mock/stderr"
  run_case "a failing tool is an error and shows its stderr" apache 1 "failed (exit 2)" "not an ELF file"
  set_mock "${needed_ok}" "${undef_apache}" "${def_apache}"
  echo "   99: 0000000000000000     0 NOTYPE  GLOBAL DEFAULT  UND EVP_from_stderr" > "${work}/mock/stderr"
  run_case "tool warnings on stderr are shown but not parsed" apache 0 "PASSED" "not parsed"
  rm -rf "${work}/mock"

  echo "-- real fixture objects --"
  local cc="" c
  for c in "${CC:-}" cc gcc clang; do
    if [ -n "${c}" ] && command -v "${c}" >/dev/null 2>&1; then cc="${c}"; break; fi
  done
  if [ -z "${cc}" ] || ! command -v readelf >/dev/null 2>&1 || [ "$(uname -s)" != "Linux" ]; then
    if [ "${require_fixtures}" = 1 ]; then
      echo "  FAIL real fixtures required but unavailable: needs Linux, a C compiler and readelf"
      fails=$((fails + 1))
    else
      echo "  skipped: needs Linux, a C compiler and readelf (the recorded cases above ran)"
    fi
  else
    cat > "${work}/clean.c" <<'SRC'
#include <stdlib.h>
#include <string.h>
int pagespeed_module = 1;
static int EVP_private(void) { return 7; }
int internal_helper(const char *s) { return (int)strlen(s) + abs(EVP_private()); }
SRC
    cat > "${work}/leaky.c" <<'SRC'
#include <stdlib.h>
#include <string.h>
extern const void *EVP_sha256(void);
int pagespeed_module = 1;
int internal_helper(const char *s) { return (int)strlen(s) + abs(EVP_sha256() != 0); }
SRC
    cat > "${work}/hooks.c" <<'SRC'
#include <stdlib.h>
#include <string.h>
extern void *OPENSSL_memory_alloc(size_t) __attribute__((weak));
extern int ap_hook_thing(void);
int pagespeed_module = 1;
int internal_helper(const char *s) {
  return (int)strlen(s) + (OPENSSL_memory_alloc != 0) + abs(ap_hook_thing());
}
SRC
    cat > "${work}/hostapi.c" <<'SRC'
#include <stdlib.h>
#include <string.h>
extern int ap_hook_thing(void);
int pagespeed_module = 1;
int internal_helper(const char *s) { return (int)strlen(s) + abs(ap_hook_thing()); }
SRC
    printf '{ global: pagespeed_module; local: *; };\n' > "${work}/module.lds"
    # real_case <label> <source> <version-script-or-empty> <want-rc> <text>
    real_case() {
      local label="$1" src="$2" lds="$3" want="$4" text="$5" out rc=0 obj
      obj="${work}/$(basename "${src}" .c)-${lds:+lds}.so"
      if ! "${cc}" -shared -fPIC -o "${obj}" "${src}" \
             ${lds:+-Wl,--version-script,"${lds}"} 2>"${work}/cc.err"; then
        echo "  FAIL ${label}: fixture did not build"; sed 's/^/      /' "${work}/cc.err"
        fails=$((fails + 1)); return
      fi
      out="$(READELF=readelf bash "$0" --profile apache "${obj}" 2>&1)" || rc=$?
      if [ "${rc}" -ne "${want}" ] || { [ -n "${text}" ] && ! grep -qF -- "${text}" <<<"${out}"; }; then
        echo "  FAIL ${label}: exit ${rc} (expected ${want}), looking for '${text}'"
        sed 's/^/      /' <<<"${out}"
        fails=$((fails + 1)); return
      fi
      echo "  ok   ${label}"
    }
    real_case "fixture with version script, no stray reference passes" \
      "${work}/clean.c" "${work}/module.lds" 0 "PASSED"
    real_case "fixture importing only the server API passes" \
      "${work}/hostapi.c" "${work}/module.lds" 0 "PASSED"
    real_case "fixture with an undefined EVP_sha256 fails" \
      "${work}/leaky.c" "${work}/module.lds" 2 "   EVP_sha256  ${NOT_ALLOWED}  ${CRYPTO_LABEL}"
    real_case "fixture with a weak undefined allocator hook fails" \
      "${work}/hooks.c" "${work}/module.lds" 2 "   OPENSSL_memory_alloc  ${WEAK_NOT_ALLOWED}  ${CRYPTO_LABEL}"
    real_case "fixture without a version script fails on exports" \
      "${work}/clean.c" "" 2 "internal_helper"
  fi

  if [ "${fails}" -ne 0 ]; then
    echo "assert_module_symbols self-test: ${fails} case(s) FAILED" >&2
    return 1
  fi
  echo "assert_module_symbols self-test: all cases passed"
}

# ---------------------------------------------------------------------------
# Entry
# ---------------------------------------------------------------------------
PROFILE=""
INPUT=""
SELF_TEST=0
REQUIRE_FIXTURES=0
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) SELF_TEST=1; shift ;;
    --require-fixtures) REQUIRE_FIXTURES=1; shift ;;
    --profile) [ $# -ge 2 ] || usage; PROFILE="$2"; shift 2 ;;
    --profile=*) PROFILE="${1#--profile=}"; shift ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) [ -z "${INPUT}" ] || usage; INPUT="$1"; shift ;;
  esac
done
if [ "${SELF_TEST}" = 1 ]; then
  self_test "${REQUIRE_FIXTURES}"
  exit $?
fi
[ -n "${PROFILE}" ] && [ -n "${INPUT}" ] || usage
case "${PROFILE}" in apache|nginx|client-lib) ;; *) die "unknown profile: ${PROFILE}" ;; esac
[ -f "${INPUT}" ] || die "${INPUT} not found"
command -v "${READELF}" >/dev/null 2>&1 || die "required tool not found: ${READELF} (install binutils)"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
OBJECT="$(resolve_object "${PROFILE}" "${INPUT}" "${WORK}")"
check_object "${PROFILE}" "${OBJECT}"
