#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Fetches and verifies the sidecar inputs of a mod_pagespeed module release,
# stages them for packing WeAmp.PageSpeed.Sidecar and
# WeAmp.PageSpeed.Sidecar.NativeAssets.Linux, and checks the packed packages
# (see docs/ci/release.md, "The sidecar inputs"). Never "latest" in a real
# run: the release is named by the release tag (v<X> takes module-v<X>), its
# authenticity is the detached signature over SHA256SUMS by the release key
# committed in this repository, and every managed and source input comes from
# that release's signed source tarball, never from a branch checkout.
#
# Usage:
#   tools/ci/sidecar_fetch.sh resolve <v<X> release tag> <real|dry> [<VERSION.txt of main>]
#   tools/ci/sidecar_fetch.sh fetch  <module tag> <out-dir> <public key file>
#   tools/ci/sidecar_fetch.sh verify <module tag> <dir>     <public key file>
#   tools/ci/sidecar_fetch.sh stage  <module tag> <dir>     <public key file> <work-dir>
#   tools/ci/sidecar_fetch.sh check-packages <dir> <nupkg-dir> <version>
#
# resolve: prints the module tag to use: module-v<X> for the release tag v<X>.
#         A rehearsal tag v<X>-pre.<N> (dry run only; a real run refuses it)
#         has no module release of its own: it takes module-v<X> when that exists,
#         else the newest published one.
#         A real run always gets that tag (fetch then fails if the module
#         release is not published), and only when the tag is the current
#         version: <VERSION.txt of main> is then required and must hold <X>,
#         so an older tag cannot be released again. A dry run, which happens before the module
#         release of the version is cut, gets module-v<X> when it exists and
#         otherwise the newest published module release (a rehearsal of the
#         pack path only; the workflow labels it as such).
# fetch:  downloads SHA256SUMS, SHA256SUMS.asc, sidecar-pair.json, the two
#         pair tarballs and the source tarball mod_pagespeed-<version>.tar.gz
#         of the tag from the public module release (We-Amp/mod_pagespeed),
#         into an empty <out-dir>, then runs verify. It also refuses when the
#         release is a draft or a pre-release, when any of those assets was
#         modified after the release was published, or when the tag no longer
#         points at the commit the manifest says was built. GH_READ_TOKEN, when
#         set, is sent to the API endpoint only, against the anonymous rate
#         limit.
# verify: refuses unless all of the following hold:
#   - the tag is module-v<version>;
#   - SHA256SUMS.asc is one good signature over SHA256SUMS by the release key,
#     checked with release_checksums_sig.sh verify (which pins the key
#     fingerprint) against <public key file>;
#   - SHA256SUMS lists sidecar-pair.json with its sha256;
#   - sidecar-pair.json names the tag and version and exactly the RIDs
#     linux-x64 and linux-arm64, and each tarball has the name, size and
#     sha256 it gives, which SHA256SUMS lists too;
#   - each pair tarball holds exactly its top directory and the files the
#     manifest names, regular files only, each with the manifest's sha256, and
#     the two executables are executable;
#   - the source tarball has the sha256 SHA256SUMS lists for it;
#   - <dir> holds exactly those six files.
#   It prints the sha256 of SHA256SUMS and of sidecar-pair.json: the lane
#   records them in the run's step summary as release evidence.
# stage:  runs verify, then writes <work-dir>/aspnetcore: the aspnetcore/ tree
#         of the source tarball (regular files and directories only), with the
#         pair of each RID (nginx, ngx_pagespeed_module.so,
#         pagespeed-nginx-launch, build-info.json) placed in
#         WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/runtimes/<rid>/native.
# check-packages: refuses unless <nupkg-dir> holds exactly the two sidecar
#         packages of <version>: the native package with exactly the four files
#         per RID at the digests of sidecar-pair.json in <dir>, its
#         buildTransitive targets and the license files; the managed package
#         with lib/net8.0 and lib/net10.0, a dependency on the native package
#         at the same version, and the readme and license files.

set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIG="$HERE/release_checksums_sig.sh"
REPO=We-Amp/mod_pagespeed
FETCH_API=""

# Checks the module tag; sets PIN_TAG and PIN_VERSION.
read_tag() {
  PIN_TAG="$1"
  [[ "$PIN_TAG" =~ ^module-v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] \
    || die "'$PIN_TAG' is not a module-v<version> tag"
  PIN_VERSION="${PIN_TAG#module-v}"
}

cmd_verify() {
  [ $# -eq 3 ] || die "usage: $0 verify <module tag> <dir> <public key file>"
  local dir="$2" key="$3"
  read_tag "$1"
  [ -d "$dir" ] || die "$dir is not a directory"
  for f in SHA256SUMS SHA256SUMS.asc sidecar-pair.json; do
    [ -s "$dir/$f" ] || die "$dir/$f is missing or empty"
  done
  bash "$SIG" verify "$dir/SHA256SUMS" "$dir/SHA256SUMS.asc" "$key"
  python3 -I - "$dir" "$PIN_TAG" "$PIN_VERSION" <<'PY'
import hashlib, json, os, re, sys, tarfile

directory, tag, version = sys.argv[1:4]
RIDS = ("linux-x64", "linux-arm64")
EXECUTABLES = ("nginx", "pagespeed-nginx-launch")
SOURCE = "mod_pagespeed-%s.tar.gz" % version


def refuse(msg):
    print("::error::sidecar_fetch: %s" % msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# sha256sum output: "<digest>  <name>" (text mode) or "<digest> *<name>"
# (binary mode); both are accepted.
SUMS_LINE = re.compile(r"^([0-9a-f]{64}) [ *](\S.*)$")
sums = {}
with open(os.path.join(directory, "SHA256SUMS")) as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        m = SUMS_LINE.match(line)
        if not m or m.group(2) in sums:
            refuse("SHA256SUMS has a malformed or repeated line: %r" % line)
        sums[m.group(2)] = m.group(1)

pair_path = os.path.join(directory, "sidecar-pair.json")
pair_sha = sha256_file(pair_path)
if sums.get("sidecar-pair.json") != pair_sha:
    refuse("SHA256SUMS does not list sidecar-pair.json with sha256 %s" % pair_sha)

with open(pair_path) as fh:
    manifest = json.load(fh)
if manifest.get("schema") != 1:
    refuse("sidecar-pair.json has schema %r, expected 1" % manifest.get("schema"))
if manifest.get("tag") != tag or manifest.get("version") != version:
    refuse("sidecar-pair.json names %r / %r, the tag %s / %s"
           % (manifest.get("tag"), manifest.get("version"), tag, version))
rids = manifest.get("rids") or {}
if set(rids) != set(RIDS):
    refuse("sidecar-pair.json names RIDs %s, expected exactly %s" % (sorted(rids), ", ".join(RIDS)))

expected = {"SHA256SUMS", "SHA256SUMS.asc", "sidecar-pair.json", SOURCE}
for rid in RIDS:
    entry = rids[rid]
    name = "pagespeed-sidecar-native-%s-%s.tar.gz" % (version, rid)
    if entry.get("asset") != name:
        refuse("%s: the manifest names asset %r, expected %s" % (rid, entry.get("asset"), name))
    path = os.path.join(directory, name)
    if not os.path.isfile(path):
        refuse("missing %s" % name)
    digest = sha256_file(path)
    if digest != entry.get("sha256") or sums.get(name) != digest:
        refuse("%s: sha256 %s, manifest %s, SHA256SUMS %s"
               % (name, digest, entry.get("sha256"), sums.get(name)))
    if os.path.getsize(path) != entry.get("size"):
        refuse("%s: size %d, the manifest says %r" % (name, os.path.getsize(path), entry.get("size")))
    want_files = entry.get("files") or {}
    if not want_files:
        refuse("%s: the manifest lists no files" % rid)
    top = name[: -len(".tar.gz")]
    seen = {}
    with tarfile.open(path, "r:gz") as tar:
        for m in tar.getmembers():
            mname = m.name.rstrip("/")
            if mname == top and m.isdir():
                continue
            base = mname[len(top) + 1:] if mname.startswith(top + "/") else ""
            if not base or "/" in base or base not in want_files or base in seen:
                refuse("%s: unexpected member %s" % (name, m.name))
            if not m.isfile():
                refuse("%s: %s is not a regular file" % (name, base))
            if base in EXECUTABLES and not (m.mode & 0o111):
                refuse("%s: %s is not executable" % (name, base))
            seen[base] = hashlib.sha256(tar.extractfile(m).read()).hexdigest()
    if seen != want_files:
        bad = sorted(k for k in set(seen) | set(want_files) if seen.get(k) != want_files.get(k))
        refuse("%s: files differ from the manifest: %s" % (name, ", ".join(bad)))
    expected.add(name)
    print("ok  %s  %s (%d files)" % (digest, name, len(seen)))

source_path = os.path.join(directory, SOURCE)
if not os.path.isfile(source_path):
    refuse("missing %s" % SOURCE)
source_sha = sha256_file(source_path)
if sums.get(SOURCE) != source_sha:
    refuse("%s: sha256 %s, SHA256SUMS %s" % (SOURCE, source_sha, sums.get(SOURCE)))
print("ok  %s  %s" % (source_sha, SOURCE))

present = set(os.listdir(directory))
if present != expected:
    refuse("%s holds %s; expected exactly %s"
           % (directory, sorted(present), sorted(expected)))
print("sidecar_fetch: SHA256SUMS sha256 %s" % sha256_file(os.path.join(directory, "SHA256SUMS")))
print("sidecar_fetch: sidecar-pair.json sha256 %s" % pair_sha)
print("sidecar_fetch: %s verified (nginx %s, glibc floor %s, source %s)"
      % (tag, manifest.get("nginx_version"), manifest.get("glibc_floor"), manifest.get("source_sha")))
PY
}

cmd_fetch() {
  [ $# -eq 3 ] || die "usage: $0 fetch <module tag> <out-dir> <public key file>"
  local out="$2" key="$3" auth=() name url
  read_tag "$1"
  mkdir -p "$out"
  [ -z "$(ls -A "$out")" ] || die "$out is not empty"
  FETCH_API="$(mktemp)"
  trap 'rm -f "$FETCH_API"' EXIT
  if [ -n "${GH_READ_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer ${GH_READ_TOKEN}")
  fi
  curl -fsSL --retry 5 --retry-all-errors \
    -H "Accept: application/vnd.github+json" ${auth[@]+"${auth[@]}"} \
    "https://api.github.com/repos/${REPO}/releases/tags/${PIN_TAG}" -o "$FETCH_API" \
    || die "no release $PIN_TAG in $REPO"
  for name in SHA256SUMS SHA256SUMS.asc sidecar-pair.json \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-x64.tar.gz" \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-arm64.tar.gz" \
      "mod_pagespeed-${PIN_VERSION}.tar.gz"; do
    url="$(python3 -I - "$FETCH_API" "$name" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
if doc.get("draft") or doc.get("prerelease"):
    sys.exit("the module release is a draft or a pre-release")
urls = [a["browser_download_url"] for a in doc.get("assets", []) if a.get("name") == sys.argv[2]]
if len(urls) != 1:
    sys.exit("the module release has %d assets named %s" % (len(urls), sys.argv[2]))
print(urls[0])
PY
)" || die "cannot resolve $name in $REPO $PIN_TAG"
    case "$url" in
      "https://github.com/${REPO}/releases/download/${PIN_TAG}/"*) ;;
      *) die "unexpected download URL for $name: $url" ;;
    esac
    # Anonymous: the release is public, and the token goes to the API only.
    curl -fsSL --retry 5 --retry-all-errors -o "$out/$name" "$url"
  done
  # No fetched asset may have been touched after the release was published,
  # and the tag must still point at the commit the manifest says was built.
  python3 -I - "$FETCH_API" SHA256SUMS SHA256SUMS.asc sidecar-pair.json \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-x64.tar.gz" \
      "pagespeed-sidecar-native-${PIN_VERSION}-linux-arm64.tar.gz" \
      "mod_pagespeed-${PIN_VERSION}.tar.gz" <<'PY' || die "the module release $PIN_TAG changed after it was published"
import json, re, sys
doc = json.load(open(sys.argv[1]))
STAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
pub = doc.get("published_at")
if not isinstance(pub, str) or not STAMP.match(pub):
    sys.exit("the module release has no usable published_at")
for name in sys.argv[2:]:
    found = [a for a in doc.get("assets", []) if a.get("name") == name]
    if len(found) != 1:
        sys.exit("the module release has %d assets named %s" % (len(found), name))
    upd = found[0].get("updated_at")
    if not isinstance(upd, str) or not STAMP.match(upd):
        sys.exit("%s has no usable updated_at" % name)
    if upd > pub:
        sys.exit("%s was modified after publication (%s > %s)" % (name, upd, pub))
PY
  rm -f "$FETCH_API"
  cmd_verify "$PIN_TAG" "$out" "$key"
  want_sha="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("source_sha", ""))' "$out/sidecar-pair.json")"
  [[ "$want_sha" =~ ^[0-9a-f]{40}$ ]] || die "sidecar-pair.json has no source_sha"
  got_sha="$(peel_tag "$PIN_TAG" "${auth[@]+"${auth[@]}"}")" || die "cannot resolve the commit of $PIN_TAG in $REPO"
  [ "$got_sha" = "$want_sha" ] \
    || die "$PIN_TAG points at $got_sha now, the signed manifest was built from $want_sha"
  echo "sidecar_fetch: $PIN_TAG still points at $want_sha"
}

# peel_tag <module tag> [curl args]: prints the commit a tag of the module repo points at.
peel_tag() {
  local tag="$1" ref obj type sha
  shift
  ref="$(curl -fsSL --retry 5 --retry-all-errors -H "Accept: application/vnd.github+json" "$@" \
    "https://api.github.com/repos/${REPO}/git/ref/tags/${tag}")" || return 1
  read -r type sha < <(printf '%s' "$ref" | python3 -I -c 'import json,sys; o=json.load(sys.stdin)["object"]; print(o["type"], o["sha"])')
  if [ "$type" = tag ]; then
    obj="$(curl -fsSL --retry 5 --retry-all-errors -H "Accept: application/vnd.github+json" "$@" \
      "https://api.github.com/repos/${REPO}/git/tags/${sha}")" || return 1
    read -r type sha < <(printf '%s' "$obj" | python3 -I -c 'import json,sys; o=json.load(sys.stdin)["object"]; print(o["type"], o["sha"])')
  fi
  [ "$type" = commit ] || return 1
  printf '%s\n' "$sha"
}

cmd_resolve() {
  [ $# -ge 2 ] && [ $# -le 3 ] || die "usage: $0 resolve <v<X> release tag> <real|dry> [<VERSION.txt of main>]"
  local rel="$1" mode="$2" vfile="${3:-}" want auth=() code latest cur
  [[ "$rel" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-pre\.[0-9]+)?$ ]] || die "'$rel' is not a v<version> or v<version>-pre.<N> release tag"
  case "$mode" in real|dry) ;; *) die "mode is real or dry, not '$mode'" ;; esac
  want="module-$rel"
  if [[ "$rel" == *-pre.* ]]; then
    # A rehearsal tag has no module release of its own: a dry run looks up
    # the module release of its base version and otherwise takes the newest
    # published one (never module-v<X>-pre.<N>); a real run is refused.
    [ "$mode" = dry ] || die "a real run cannot release the rehearsal tag $rel"
    want="module-${rel%%-pre.*}"
  fi
  if [ "$mode" = real ]; then
    [ -s "$vfile" ] || die "a real run needs the VERSION.txt of main to check the tag against"
    cur="$(tr -d '[:space:]' < "$vfile")"
    [ "$cur" = "${rel#v}" ] || die "a real run releases the current version ($cur on main), not $rel"
    printf '%s\n' "$want"
    return 0
  fi
  if [ -n "${GH_READ_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer ${GH_READ_TOKEN}")
  fi
  code="$(curl -sSL --retry 5 --retry-all-errors -o /dev/null -w '%{http_code}' ${auth[@]+"${auth[@]}"} \
    "https://api.github.com/repos/${REPO}/releases/tags/${want}")" || die "cannot reach the module release API"
  case "$code" in
    200) printf '%s\n' "$want" ;;
    404)
      latest="$(curl -fsSL --retry 5 --retry-all-errors -H "Accept: application/vnd.github+json" ${auth[@]+"${auth[@]}"} \
        "https://api.github.com/repos/${REPO}/releases/latest" \
        | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("tag_name", ""))')" \
        || die "cannot find a published module release to rehearse with"
      [[ "$latest" =~ ^module-v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the latest module release '$latest' is not a module-v<version> tag"
      printf '%s\n' "$latest"
      ;;
    *) die "module release API answered HTTP $code for $want" ;;
  esac
}

cmd_stage() {
  [ $# -eq 4 ] || die "usage: $0 stage <module tag> <dir> <public key file> <work-dir>"
  local dir="$2" key="$3" work="$4"
  cmd_verify "$1" "$dir" "$key"
  mkdir -p "$work"
  [ ! -e "$work/aspnetcore" ] || die "$work/aspnetcore already exists"
  python3 -I - "$dir" "$PIN_VERSION" "$work" <<'PY'
import os, shutil, sys, tarfile

directory, version, work = sys.argv[1:4]
RIDS = ("linux-x64", "linux-arm64")
NATIVE = {"nginx": 0o755, "ngx_pagespeed_module.so": 0o644,
          "pagespeed-nginx-launch": 0o755, "build-info.json": 0o644}
NATIVE_PROJECT = "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"


def refuse(msg):
    print("::error::sidecar_fetch stage: %s" % msg, file=sys.stderr)
    sys.exit(1)


def copy_member(tar, m, dest, mode):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with tar.extractfile(m) as src, open(dest, "wb") as out:
        shutil.copyfileobj(src, out)
    os.chmod(dest, mode)


# The aspnetcore/ tree of the verified source tarball: regular files and
# directories only, no absolute path, no "..", nothing outside aspnetcore/.
prefix = "mod_pagespeed-%s/aspnetcore/" % version
root = os.path.join(work, "aspnetcore")
count = 0
with tarfile.open(os.path.join(directory, "mod_pagespeed-%s.tar.gz" % version), "r:gz") as tar:
    for m in tar:
        name = m.name
        if not name.startswith(prefix):
            continue
        rel = name[len(prefix):].rstrip("/")
        if not rel:
            continue
        parts = rel.split("/")
        if name.startswith("/") or any(p in ("", ".", "..") for p in parts):
            refuse("unsafe path %s" % name)
        dest = os.path.join(root, *parts)
        if m.isdir():
            os.makedirs(dest, exist_ok=True)
        elif m.isfile():
            copy_member(tar, m, dest, 0o755 if m.mode & 0o111 else 0o644)
            count += 1
        else:
            refuse("%s is not a regular file or directory" % name)
for p in ("Directory.Build.props", NATIVE_PROJECT + "/" + NATIVE_PROJECT + ".csproj",
          "WeAmp.PageSpeed.AspNetCore/WeAmp.PageSpeed.AspNetCore.csproj",
          "WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj"):
    if not os.path.isfile(os.path.join(root, p)):
        refuse("the source tarball has no aspnetcore/%s" % p)
print("staged %d files of aspnetcore/ from the source tarball" % count)

# The verified pair of each RID, where the native project packs it from.
for rid in RIDS:
    name = "pagespeed-sidecar-native-%s-%s" % (version, rid)
    native = os.path.join(root, NATIVE_PROJECT, "runtimes", rid, "native")
    os.makedirs(native, exist_ok=True)
    for f in os.listdir(native):
        if f != ".gitkeep":
            refuse("%s already holds %s" % (native, f))
    placed = set()
    with tarfile.open(os.path.join(directory, name + ".tar.gz"), "r:gz") as tar:
        for m in tar.getmembers():
            base = m.name[len(name) + 1:] if m.name.startswith(name + "/") else ""
            if base in NATIVE and m.isfile():
                copy_member(tar, m, os.path.join(native, base), NATIVE[base])
                placed.add(base)
    if placed != set(NATIVE):
        refuse("%s: missing %s" % (name, ", ".join(sorted(set(NATIVE) - placed))))
    print("placed %s in %s" % (", ".join(sorted(placed)), os.path.relpath(native, work)))
PY
}

cmd_check_packages() {
  [ $# -eq 3 ] || die "usage: $0 check-packages <dir> <nupkg-dir> <version>"
  python3 -I - "$@" <<'PY'
import hashlib, json, os, re, sys, zipfile
import xml.etree.ElementTree as ET

directory, nupkgs, version = sys.argv[1:4]
RIDS = ("linux-x64", "linux-arm64")
NATIVE_FILES = ("nginx", "ngx_pagespeed_module.so", "pagespeed-nginx-launch", "build-info.json")
MANAGED = "WeAmp.PageSpeed.Sidecar"
NATIVE = "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"
LICENSES = ("LICENSE", "NOTICE", "THIRD-PARTY-NOTICES")


def refuse(msg):
    print("::error::sidecar_fetch check-packages: %s" % msg, file=sys.stderr)
    sys.exit(1)


def nuspec(z, pkg_id):
    names = [n for n in z.namelist() if n.endswith(".nuspec") and "/" not in n]
    if names != [pkg_id + ".nuspec"]:
        refuse("%s: nuspec entries %s" % (pkg_id, names))
    root = ET.fromstring(z.read(names[0]))
    ns = {"n": root.tag[1:].split("}")[0]} if root.tag.startswith("{") else {"n": ""}
    meta = root.find("n:metadata", ns)
    get = lambda k: (meta.findtext("n:" + k, default="", namespaces=ns) or "").strip()
    if get("id") != pkg_id or get("version") != version:
        refuse("%s: nuspec says %s %s, expected %s %s" % (pkg_id, get("id"), get("version"), pkg_id, version))
    repo = meta.find("n:repository", ns)
    if repo is not None and any(repo.get(a) for a in ("url", "commit", "branch")):
        refuse("%s: source-control metadata in the nuspec" % pkg_id)
    return meta, ns


with open(os.path.join(directory, "sidecar-pair.json")) as fh:
    manifest = json.load(fh)

want = {"%s.%s.nupkg" % (MANAGED, version), "%s.%s.nupkg" % (NATIVE, version)}
present = {f for f in os.listdir(nupkgs) if f.endswith(".nupkg")}
if present != want:
    refuse("%s holds %s, expected exactly %s" % (nupkgs, sorted(present), sorted(want)))

with zipfile.ZipFile(os.path.join(nupkgs, "%s.%s.nupkg" % (NATIVE, version))) as z:
    nuspec(z, NATIVE)
    names = set(z.namelist())
    for f in LICENSES + ("buildTransitive/%s.targets" % NATIVE,):
        if f not in names:
            refuse("%s: no %s" % (NATIVE, f))
    if any(n.startswith("lib/") for n in names):
        refuse("%s: carries lib/ (a content-only package)" % NATIVE)
    natives = sorted(n for n in names if n.startswith("runtimes/") and not n.endswith("/"))
    expect = sorted("runtimes/%s/native/%s" % (r, f) for r in RIDS for f in NATIVE_FILES)
    if natives != expect:
        refuse("%s: runtimes entries %s, expected %s" % (NATIVE, natives, expect))
    for rid in RIDS:
        files = manifest["rids"][rid]["files"]
        for f in NATIVE_FILES:
            got = hashlib.sha256(z.read("runtimes/%s/native/%s" % (rid, f))).hexdigest()
            if got != files[f]:
                refuse("%s: runtimes/%s/native/%s has sha256 %s, sidecar-pair.json %s"
                       % (NATIVE, rid, f, got, files[f]))
    print("ok  %s %s: %d native files at the digests of sidecar-pair.json" % (NATIVE, version, len(expect)))

with zipfile.ZipFile(os.path.join(nupkgs, "%s.%s.nupkg" % (MANAGED, version))) as z:
    meta, ns = nuspec(z, MANAGED)
    names = set(z.namelist())
    for f in LICENSES + ("README.md",) + tuple("lib/%s/%s.dll" % (t, MANAGED) for t in ("net8.0", "net10.0")):
        if f not in names:
            refuse("%s: no %s" % (MANAGED, f))
    if any(n.startswith("runtimes/") for n in names):
        refuse("%s: carries runtimes/ (the pair belongs in %s)" % (MANAGED, NATIVE))
    deps = meta.findall(".//n:dependency", ns)
    native_deps = [d for d in deps if d.get("id") == NATIVE]
    groups = meta.findall(".//n:dependencies/n:group", ns)
    if len(groups) < 2 or len(native_deps) != len(groups):
        refuse("%s: every framework group must depend on %s (groups %d, deps %d)"
               % (MANAGED, NATIVE, len(groups), len(native_deps)))
    for d in native_deps:
        if re.sub(r"^\[|[\],].*$", "", d.get("version", "")) != version:
            refuse("%s: depends on %s %s, expected %s" % (MANAGED, NATIVE, d.get("version"), version))
    print("ok  %s %s: net8.0 + net10.0, depends on %s %s" % (MANAGED, version, NATIVE, version))
PY
}

case "${1:-}" in
  resolve) shift; cmd_resolve "$@" ;;
  fetch) shift; cmd_fetch "$@" ;;
  verify) shift; cmd_verify "$@" ;;
  stage) shift; cmd_stage "$@" ;;
  check-packages) shift; cmd_check_packages "$@" ;;
  *) die "usage: $0 resolve|fetch|verify|stage|check-packages ..." ;;
esac
