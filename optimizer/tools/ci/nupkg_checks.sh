#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The NuGet package checks of the release lane, in one place so the real run
# and the publish rehearsal run the same code (docs/ci/release.md, "NuGet
# publish rehearsal").
#
# Usage: tools/ci/nupkg_checks.sh <check> <dir> [args]
#
# Checks the release lane's `package` job runs (real run and dry run):
#   expect-seven <dir> <version>   seven .nupkg, both sidecar packages present
#   metadata <dir>                 no SourceLink; repository url is the public repo
#   console-spa <dir>              the AspNetCore package embeds the /console/ SPA
# Environment: EXPECT_SHA (the commit the packages must name), PIN_NOTE_FATAL=1
# (a README-SIDECAR-PIN.txt fails instead of warning), NUGET_ORG_EXPECT,
# NUGET_ORG_FIRST_PACKAGES, NUGET_ORG_BASE (read-only endpoint, for the self-test).
# Checks only the rehearsal adds (nothing here changes what a release does):
#   identity <dir> <version>       the exact set of seven ids, nuspec id/version
#                                  match the file name and <version>, internal
#                                  dependencies pinned to <version>, required
#                                  metadata and files, nuget.org size limit
#   signing <dir> <signed|unsigned|report>
#                                  NuGet package signature state (verified with
#                                  `dotnet nuget verify` when present) and the
#                                  Authenticode presence of the Windows binaries
#   local-feed <dir> <scratch>     push all seven to a loopback feed that answers
#                                  201/409/400 like nuget.org, twice, with
#                                  --skip-duplicate, then restore a
#                                  consumer project from it (needs dotnet and
#                                  network access to nuget.org for third-party
#                                  dependencies)
#   nuget-org <dir>                read-only look at nuget.org: which of the
#                                  id/version pairs already exist; set
#                                  NUGET_ORG_EXPECT=new to fail when one does,
#                                  NUGET_ORG_FIRST_PACKAGES="<id> ..." to accept
#                                  a 404 for ids a release publishes first (any
#                                  other 404, empty or malformed answer fails)
#   offline <dir> <version> <signed|unsigned|report>
#                                  every check above that needs no network
#
# Exit status 0 only when every requested check passes. Errors are written as
# GitHub `::error::` lines.
set -euo pipefail

IDS="WeAmp.PageSpeed WeAmp.PageSpeed.AspNetCore WeAmp.PageSpeed.NativeAssets.Linux WeAmp.PageSpeed.NativeAssets.Windows WeAmp.PageSpeed.NativeAssets.macOS WeAmp.PageSpeed.Sidecar WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"

check_expect_seven() { # <dir> <version>
  cd "$1"
  local VERSION="$2" id
  ls -lh
  [ "$(find . -maxdepth 1 -name '*.nupkg' | wc -l)" -eq 7 ] || { echo "::error::expected 7 packages"; exit 1; }
  for id in WeAmp.PageSpeed.Sidecar WeAmp.PageSpeed.Sidecar.NativeAssets.Linux; do
    [ -s "${id}.${VERSION}.nupkg" ] || { echo "::error::missing ${id}.${VERSION}.nupkg"; exit 1; }
  done
}

# The ASP.NET Core packages carry the public repository URL and commit
# (Source Link); the sidecar packages carry none. Any repository url
# that is not the public repository, and any SourceLink element in a
# nuspec, fails the release.
check_metadata() { # <dir>
  cd "$1"
  local expected='https://github.com/We-Amp/pagespeed-optimizer'
  local bad=0 nupkg nuspec url
  for nupkg in *.nupkg; do
    nuspec=$(unzip -p "$nupkg" "*.nuspec" 2>/dev/null)
    if echo "$nuspec" | grep -qi '<sourceLink'; then
      echo "::error::unexpected SourceLink element in $nupkg"
      bad=1
    fi
    url=$(echo "$nuspec" | grep -io '<repository[^>]* url="[^"]*"' | sed -E 's/.* url="([^"]*)"/\1/' || true)
    if [ -n "$url" ] && [ "$url" != "$expected" ]; then
      echo "::error::repository url in $nupkg is '$url', expected '$expected'"
      bad=1
    fi
    case "$nupkg" in
      WeAmp.PageSpeed.Sidecar*) ;;
      *) [ "$url" = "$expected" ] || { echo "::error::$nupkg has no repository url"; bad=1; } ;;
    esac
  done
  [ "$bad" = "0" ] || exit 1
  echo "OK: source-control metadata in the packages is as expected."
}

# A failed SPA build or a drifted EmbedWebShell glob would ship an
# empty console and every install would answer 503 for /console/.
check_console_spa() { # <dir>
  cd "$1"
  local aspnet_nupkg STRINGS
  aspnet_nupkg=$(find . -maxdepth 1 -name 'WeAmp.PageSpeed.AspNetCore.*.nupkg' | head -1)
  STRINGS="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/aspnet-dll.strings.XXXXXX")"
  unzip -p "$aspnet_nupkg" "lib/net10.0/WeAmp.PageSpeed.AspNetCore.dll" | strings -a > "$STRINGS"
  if ! grep -aFq 'WeAmp.PageSpeed.AspNetCore.Console/index.html' "$STRINGS"; then
    echo "::error::Embedded /console/ SPA missing from $aspnet_nupkg"
    rm -f "$STRINGS"
    exit 1
  fi
  echo "OK: embedded /console/ SPA present ($(grep -acF 'WeAmp.PageSpeed.AspNetCore.Console/' "$STRINGS") manifest entries)."
  rm -f "$STRINGS"
}

# ---- rehearsal-only checks ------------------------------------------------

check_identity() { # <dir> <version>
  IDS="$IDS" python3 -I - "$1" "$2" <<'PY'
import os, re, sys, zipfile
import xml.etree.ElementTree as ET

d, version = sys.argv[1], sys.argv[2]
ids = os.environ["IDS"].split()
expect_sha = os.environ.get("EXPECT_SHA", "")
pin_note_fatal = os.environ.get("PIN_NOTE_FATAL", "") == "1"
errs = []
def err(m): errs.append(m); print("::error::" + m)

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$")
if not SEMVER.match(version):
    err("version %r is not SemVer 2" % version)
# README-SIDECAR-PIN.txt travels with the packages when the sidecar did not come
# from module-v<X> (X: the version without any -pre.<N>). For a candidate
# (PIN_NOTE_FATAL=1) that fails; otherwise it only warns.
note = os.path.join(d, "README-SIDECAR-PIN.txt")
if os.path.exists(note):
    base = version.split("-pre.")[0]
    if pin_note_fatal:
        err("README-SIDECAR-PIN.txt is present: the sidecar packages are not built from module-v%s" % base)
    else:
        print("::warning::README-SIDECAR-PIN.txt is present: the sidecar packages are not from module-v%s" % base)
files = sorted(f for f in os.listdir(d) if not f.startswith(".") and f != "README-SIDECAR-PIN.txt")
want = sorted("%s.%s.nupkg" % (i, version) for i in ids)
extra = [f for f in files if f not in want]
missing = [f for f in want if f not in files]
if extra: err("unexpected files next to the packages: " + ", ".join(extra))
if missing: err("missing packages: " + ", ".join(missing))

# What each native-assets package must carry (a package with a missing or empty
# binary installs fine and fails at run time).
RT = "runtimes/%s/native/"
NATIVE = {
    "WeAmp.PageSpeed.NativeAssets.Linux": [RT % r + f for r in ("linux-x64", "linux-arm64") for f in ("libpagespeed.so", "factory_worker", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.NativeAssets.Windows": [RT % "win-x64" + f for f in ("pagespeed.dll", "factory_worker.exe", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.NativeAssets.macOS": [RT % "osx-arm64" + f for f in ("libpagespeed.dylib", "factory_worker", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux": [RT % r + f for r in ("linux-x64", "linux-arm64") for f in ("nginx", "ngx_pagespeed_module.so", "pagespeed-nginx-launch", "build-info.json")],
}
commits = {}
MAX = 250 * 1024 * 1024  # nuget.org rejects a package over 250 MB
for f in want:
    p = os.path.join(d, f)
    if not os.path.isfile(p): continue
    ident = f[:-len(".%s.nupkg" % version)]
    before = len(errs)
    if os.path.getsize(p) > MAX: err("%s is over the nuget.org size limit" % f)
    try:
        z = zipfile.ZipFile(p)
    except Exception as e:
        err("%s is not a zip: %s" % (f, e)); continue
    bad = z.testzip()
    if bad: err("%s: corrupt entry %s" % (f, bad))
    names = z.namelist()
    if len(names) != len(set(names)): err("%s: duplicate zip entries" % f)
    for n in names:
        if n.startswith("/") or ".." in n.split("/"): err("%s: unsafe entry name %r" % (f, n))
    for need in ("[Content_Types].xml", "_rels/.rels"):
        if need not in names: err("%s: missing %s" % (f, need))
    specs = [n for n in names if n.endswith(".nuspec") and "/" not in n]
    if len(specs) != 1:
        err("%s: expected exactly one root nuspec, found %d" % (f, len(specs))); continue
    # The nuspec namespace differs between package kinds (2011/08, 2013/05).
    md = ET.fromstring(z.read(specs[0]).decode("utf-8-sig")).find("{*}metadata")
    if md is None: err("%s: no metadata element" % f); continue
    g = lambda t: (md.findtext("{*}" + t, default="") or "").strip()
    if g("id") != ident: err("%s: nuspec id %r differs from the file name" % (f, g("id")))
    if g("version") != version: err("%s: nuspec version %r, expected %r" % (f, g("version"), version))
    if specs[0] != ident + ".nuspec": err("%s: nuspec is named %s" % (f, specs[0]))
    if not re.match(r"^[A-Za-z0-9_.-]{1,100}$", g("id")): err("%s: id outside nuget.org's accepted shape" % f)
    for req in ("authors", "description", "copyright"):
        if not g(req): err("%s: empty <%s>" % (f, req))
    lic = md.find("{*}license")
    if lic is None or not (lic.text or "").strip(): err("%s: no <license> expression" % f)
    for tag in ("readme", "icon"):  # when declared, the file must be in the package
        v = g(tag)
        if v and v.replace("\\", "/") not in names: err("%s: <%s> %s is not in the package" % (f, tag, v))
    if len(g("description")) > 4000: err("%s: description over nuget.org's 4000 character limit" % f)
    if len(g("id")) + len(version) > 200: err("%s: id/version too long" % f)
    rep = md.find("{*}repository")
    if rep is not None and rep.get("commit"):
        commits[ident] = rep.get("commit")
        if expect_sha and rep.get("commit") != expect_sha:
            err("%s: repository commit %s is not the expected %s" % (f, rep.get("commit"), expect_sha))
    for nf in NATIVE.get(ident, []):
        if nf not in names: err("%s: missing %s" % (f, nf))
        elif z.getinfo(nf).file_size == 0: err("%s: %s is empty" % (f, nf))
        elif nf.endswith("/BUILD_INFO.json"):
            try:
                import json
                bi = json.loads(z.read(nf).decode("utf-8-sig"))
                if expect_sha and bi.get("git_sha") != expect_sha: err("%s: %s git_sha %s is not the expected %s" % (f, nf, bi.get("git_sha"), expect_sha))
            except Exception as e:
                err("%s: %s is not JSON: %s" % (f, nf, e))
    seen = set()
    for dep in md.iterfind(".//{*}dependency"):
        did, dv = dep.get("id", ""), dep.get("version", "")
        seen.add(did)
        if did in ids and dv != version:
            err("%s: dependency %s %s is not pinned to %s" % (f, did, dv, version))
    if "AspNetCore" in ident and "WeAmp.PageSpeed" not in seen: err("%s: no dependency on WeAmp.PageSpeed" % f)
    if ident == "WeAmp.PageSpeed.Sidecar" and "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux" not in seen:
        err("%s: no dependency on the sidecar native assets" % f)
    if len(errs) == before:
        print("ok   %-48s id/version/metadata, %d entries, %.1f MB" % (f, len(names), os.path.getsize(p) / 1e6))
if len(set(commits.values())) > 1:
    err("the managed packages name different repository commits: %s" % sorted(set(commits.values())))
if errs:
    sys.exit(1)
print("OK: seven packages, ids and versions consistent with %s." % version)
PY
}

check_signing() { # <dir> <signed|unsigned|report>
  local dir="$1" want="$2" nupkg bad=0
  case "$want" in signed|unsigned|report) ;; *) echo "::error::signing mode must be signed, unsigned or report"; exit 1;; esac
  for nupkg in "$dir"/*.nupkg; do
    if unzip -l "$nupkg" | awk '{print $4}' | grep -qx '.signature.p7s'; then
      # A package that carries a NuGet signature is verified by the client.
      command -v dotnet >/dev/null || { echo "::error::$nupkg is signed but dotnet is not available to verify it"; bad=1; continue; }
      if dotnet nuget verify --all "$nupkg" >/dev/null 2>&1; then echo "ok   $(basename "$nupkg"): NuGet signature verifies"; else echo "::error::NuGet signature of $nupkg does not verify"; bad=1; fi
    else
      echo "note $(basename "$nupkg"): no NuGet package signature (nuget.org signs on ingest; it accepts unsigned packages)"
    fi
  done
  # Authenticode: is a certificate table present in each Windows PE binary?
  # Presence only; the chain and the timestamp are not checked here.
  python3 -I - "$dir" "$want" <<'PY' || bad=1
import glob, struct, sys, zipfile, io
d, want = sys.argv[1], sys.argv[2]
pkg = glob.glob(d + "/WeAmp.PageSpeed.NativeAssets.Windows.*.nupkg")
if len(pkg) != 1:
    print("::error::expected one Windows native-assets package"); sys.exit(1)
z = zipfile.ZipFile(pkg[0]); rc = 0; seen = 0
for n in z.namelist():
    if not n.lower().endswith((".dll", ".exe")): continue
    seen += 1
    b = z.read(n)
    if b[:2] != b"MZ": print("::error::%s is not a PE file" % n); rc = 1; continue
    pe = struct.unpack_from("<I", b, 0x3c)[0]
    if b[pe:pe+4] != b"PE\0\0": print("::error::%s has no PE header" % n); rc = 1; continue
    magic = struct.unpack_from("<H", b, pe + 24)[0]
    dd = pe + 24 + (96 if magic == 0x10b else 112)  # data directories
    off, size = struct.unpack_from("<II", b, dd + 4 * 8)  # entry 4: certificate table
    signed = size > 0 and off > 0 and off + size <= len(b)
    state = "Authenticode certificate table present (%d bytes)" % size if signed else "no Authenticode signature"
    ok = want == "report" or (want == "signed") == signed
    print(("ok   " if ok else "::error::") + "%s: %s" % (n, state))
    rc |= 0 if ok else 1
if seen == 0:
    print("::error::the Windows native-assets package carries no .dll or .exe"); rc = 1
sys.exit(rc)
PY
  [ "$bad" = 0 ] || exit 1
  echo "OK: signature state is as expected ($want)."
}

sha256_of() { if command -v sha256sum >/dev/null; then sha256sum < "$1" | cut -d' ' -f1; else shasum -a 256 < "$1" | cut -d' ' -f1; fi; }

check_local_feed() { # <dir> <scratch>
  local dir scratch feed pkg out port mockpid feed_url first id lid got
  dir="$(cd "$1" && pwd)"; scratch="$2"
  command -v dotnet >/dev/null || { echo "::error::dotnet is required for the local feed check"; exit 1; }
  mkdir -p "$scratch"; scratch="$(cd "$scratch" && pwd)"
  feed="$scratch/feed"; rm -rf "$feed" "$scratch/consumer" "$scratch/pkgs" "$scratch/port"
  # A loopback endpoint that answers 201 / 409 / 400 as nuget.org does. A
  # plain folder feed would not do: the client ignores --skip-duplicate there.
  python3 -I "$(dirname "$0")/nuget_mock_feed.py" "$feed" "$scratch/port" &
  mockpid=$!
  trap 'kill "$mockpid" 2>/dev/null || true' EXIT
  for _ in $(seq 1 50); do [ -s "$scratch/port" ] && break; sleep 0.1; done
  port="$(cat "$scratch/port")"
  feed_url="http://127.0.0.1:${port}/v3/index.json"
  # The client refuses plain HTTP sources unless the config allows this one.
  cat > "$scratch/NuGet.Config" <<CFG
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="rehearsal" value="$feed_url" allowInsecureConnections="true" />
  </packageSources>
</configuration>
CFG
  # Round 1 pushes every package; round 2 is the re-run of a release, where
  # every package answers 409 and --skip-duplicate must turn that into exit 0.
  for round in 1 2; do
    for pkg in "$dir"/*.nupkg; do
      out="$(dotnet nuget push "$pkg" --source "$feed_url" --configfile "$scratch/NuGet.Config" --skip-duplicate 2>&1)" || { echo "$out"; echo "::error::push round $round of $(basename "$pkg") failed"; exit 1; }
      if [ "$round" = 1 ]; then
        printf '%s' "$out" | grep -q 'Your package was pushed' || { echo "$out"; echo "::error::round 1 did not push $(basename "$pkg")"; exit 1; }
      else
        printf '%s' "$out" | grep -q 'Conflict' && printf '%s' "$out" | grep -qi 'already exists' || { echo "$out"; echo "::error::round 2 did not meet a 409 for $(basename "$pkg")"; exit 1; }
      fi
    done
    echo "ok   push round $round: seven packages, exit 0"
  done
  # Without --skip-duplicate the same duplicate must fail with the 409.
  first="$(find "$dir" -name '*.nupkg' | head -1)"
  if out="$(dotnet nuget push "$first" --source "$feed_url" --configfile "$scratch/NuGet.Config" 2>&1)"; then
    echo "::error::a duplicate push without --skip-duplicate succeeded; the rehearsal feed is not answering 409"; exit 1
  fi
  printf '%s' "$out" | grep -q '409 (Conflict)' || { echo "$out"; echo "::error::the duplicate push failed, but not with a 409"; exit 1; }
  echo "ok   a duplicate push without --skip-duplicate fails with 409; with it, exit 0"
  # A file that is not a package must be refused with a 400.
  head -c 1000 "$first" > "$scratch/truncated.nupkg"
  if out="$(dotnet nuget push "$scratch/truncated.nupkg" --source "$feed_url" --configfile "$scratch/NuGet.Config" --skip-duplicate 2>&1)"; then
    echo "::error::a truncated package was accepted"; exit 1
  fi
  printf '%s' "$out" | grep -q '400 (Bad Request)' || { echo "$out"; echo "::error::the truncated package failed, but not with a 400"; exit 1; }
  echo "ok   a truncated package is refused with 400"
  [ "$(find "$feed" -name '*.nupkg' | wc -l)" -eq 7 ] || { echo "::error::the local feed does not hold exactly seven packages"; exit 1; }
  kill "$mockpid" 2>/dev/null || true; trap - EXIT
  # Consumer restore: the dependency closure resolves from the feed plus nuget.org.
  mkdir -p "$scratch/consumer"
  cat > "$scratch/consumer/Consumer.csproj" <<CSPROJ
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFrameworks>net10.0</TargetFrameworks>
    <RestoreAdditionalProjectSources>$feed</RestoreAdditionalProjectSources>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="WeAmp.PageSpeed.AspNetCore" Version="VER" />
    <PackageReference Include="WeAmp.PageSpeed.Sidecar" Version="VER" />
  </ItemGroup>
</Project>
CSPROJ
  local ver
  ver="$(basename "$(find "$dir" -name 'WeAmp.PageSpeed.2*.nupkg' -o -name 'WeAmp.PageSpeed.[0-9]*.nupkg' | head -1)" .nupkg)"; ver="${ver#WeAmp.PageSpeed.}"
  sed -i.bak "s/VER/$ver/" "$scratch/consumer/Consumer.csproj" && rm -f "$scratch/consumer/Consumer.csproj.bak"
  # Fresh package cache, so nothing comes from a machine-wide cache; the feed
  # is listed first-class, nuget.org supplies the third-party dependencies.
  NUGET_PACKAGES="$scratch/pkgs" dotnet restore "$scratch/consumer/Consumer.csproj" --source "$feed" --source https://api.nuget.org/v3/index.json >"$scratch/restore.log" 2>&1 \
    || { tail -n 30 "$scratch/restore.log"; echo "::error::a consumer restore from the local feed failed"; exit 1; }
  # What the consumer restored is byte-identical to what was pushed.
  for pkg in "$dir"/*.nupkg; do
    id="$(unzip -p "$pkg" '*.nuspec' | sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' | head -1 | tr -d '\r')"
    lid="$(printf '%s' "$id" | tr 'A-Z' 'a-z')"
    got="$scratch/pkgs/$lid/$ver/$lid.$ver.nupkg"
    [ -f "$got" ] || { echo "::error::$id $ver was not restored"; exit 1; }
    [ "$(sha256_of "$got")" = "$(sha256_of "$pkg")" ] \
      || { echo "::error::the restored $id differs from the pushed file"; exit 1; }
  done
  echo "ok   all seven restored packages are byte-identical to the pushed files"
  echo "OK: a loopback feed accepts the seven packages, a re-push with --skip-duplicate exits 0, and a consumer restores them."
}

# What nuget.org says about each id/version. Anything but a well-formed
# answer is an error: an empty body, HTML or truncated JSON must never read as
# "the version is new". A 404 is accepted only for an id named in
# NUGET_ORG_FIRST_PACKAGES (a package published for the very first time).
check_nuget_org() { # <dir>
  local dir="$1" nupkg id ver lid lver code body found=0 bad=0 expect="${NUGET_ORG_EXPECT:-report}" has
  local first=" ${NUGET_ORG_FIRST_PACKAGES:-} "
  command -v jq >/dev/null || { echo "::error::jq is required for the nuget.org check"; exit 1; }
  body="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/nugetorg.XXXXXX")"
  for nupkg in "$dir"/*.nupkg; do
    ver="$(unzip -p "$nupkg" '*.nuspec' | sed -n 's/.*<version>\(.*\)<\/version>.*/\1/p' | head -1 | tr -d '\r')"
    id="$(unzip -p "$nupkg" '*.nuspec' | sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' | head -1 | tr -d '\r')"
    [ -n "$id" ] && [ -n "$ver" ] || { echo "::error::cannot read id/version of $nupkg"; exit 1; }
    lid="$(printf '%s' "$id" | tr 'A-Z' 'a-z')"; lver="$(printf '%s' "$ver" | tr 'A-Z' 'a-z')"
    code="$(curl -sS --max-time 30 --retry 2 -o "$body" -w '%{http_code}' "${NUGET_ORG_BASE:-https://api.nuget.org}/v3-flatcontainer/$lid/index.json")" \
      || { echo "::error::nuget.org is unreachable"; rm -f "$body"; exit 1; }
    case "$code" in
      404)
        case "$first" in
          *" $id "*) echo "new  $id: not on nuget.org yet, as announced (a first push creates the id; prefix ownership is not checkable here)";;
          *) echo "::error::nuget.org answers 404 for $id, which is expected to exist; name it in first_packages only if this release creates it"; bad=1;;
        esac;;
      200)
        # -s -e: an empty or non-JSON body gives a non-zero status (jq 1.6 exits 0 on empty input with -e alone)
        if ! jq -es 'length == 1 and (.[0] | type == "object") and (.[0].versions | type == "array") and (.[0].versions | all(type == "string"))' "$body" >/dev/null 2>&1; then
          echo "::error::nuget.org answered 200 for $id without a versions array of strings"; bad=1; continue
        fi
        has="$(jq -r --arg v "$lver" '.versions | map(ascii_downcase) | index($v) | if . == null then "no" else "yes" end' "$body")"
        if [ "$has" = yes ]; then
          echo "dupl $id $ver: already on nuget.org; --skip-duplicate would skip it and nothing from this run would be published"; found=1
        elif [ "$has" = no ]; then
          echo "ok   $id: id exists, $ver is a new version"
        else
          echo "::error::cannot read the version list of $id"; bad=1
        fi;;
      *) echo "::error::unexpected HTTP $code for $id"; bad=1;;
    esac
  done
  rm -f "$body"
  [ "$bad" = 0 ] || exit 1
  if [ "$found" = 1 ] && [ "$expect" = new ]; then echo "::error::a package version already exists on nuget.org"; exit 1; fi
  echo "OK: nuget.org read-only look done."
}

cmd="${1:-}"; [ $# -ge 1 ] && shift || true
case "$cmd" in
  expect-seven) check_expect_seven "$@" ;;
  metadata)     check_metadata "$@" ;;
  console-spa)  check_console_spa "$@" ;;
  identity)     check_identity "$@" ;;
  signing)      check_signing "$@" ;;
  local-feed)   check_local_feed "$@" ;;
  nuget-org)    check_nuget_org "$@" ;;
  offline)      [ $# -eq 3 ] || { echo "usage: offline <dir> <version> <signed|unsigned|report>"; exit 2; }
                S="$0"
                "$S" expect-seven "$1" "$2"; "$S" identity "$1" "$2"; "$S" metadata "$1"
                "$S" console-spa "$1"; "$S" signing "$1" "$3" ;;
  *) sed -n '8,38p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
