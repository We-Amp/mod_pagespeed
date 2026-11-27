#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for tools/ci/sidecar_fetch.sh. Builds a throwaway signed module
# release (own key, own fingerprint through the signature script's self-test
# override) and checks that verify and stage accept it and refuse each way it
# can be wrong. Offline; never touches the real release key.
#
# SIDECAR_LIVE_TAG=module-vX.Y.Z additionally runs fetch (and with it verify,
# stage and the post-publication checks) against that published release with
# the repository's own release public key.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FETCH="$HERE/sidecar_fetch.sh"

command -v gpg >/dev/null || { echo "gpg not found"; exit 1; }
W="$(mktemp -d /tmp/scf.XXXXXX)"
trap 'GNUPGHOME="$W/k1" gpgconf --kill gpg-agent >/dev/null 2>&1; GNUPGHOME="$W/k2" gpgconf --kill gpg-agent >/dev/null 2>&1; rm -rf "$W"' EXIT
export RUNNER_TEMP="$W"

fail=0
expect() { # expect ok|no <desc> <cmd...>
  local want="$1" desc="$2"; shift 2
  if "$@" >"$W/out" 2>&1; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc :: $(tail -n 1 "$W/out" | cut -c1-110)"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}

mkkey() { # mkkey <dir> -> prints the fingerprint; writes <dir>.pub
  mkdir -m 700 "$1"
  GNUPGHOME="$1" gpg --batch --quiet --pinentry-mode loopback --passphrase pw \
    --quick-gen-key "Self-test <self-test@example.invalid>" ed25519 sign 1d 2>/dev/null
  local f
  f="$(GNUPGHOME="$1" gpg --batch --with-colons --list-secret-keys 2>/dev/null | awk -F: '$1=="fpr"{print $10; exit}')"
  GNUPGHOME="$1" gpg --batch --armor --export "$f" 2>/dev/null > "$1.pub"
  echo "$f"
}
F1="$(mkkey "$W/k1")"
mkkey "$W/k2" >/dev/null
export RELEASE_SIG_SELF_TEST=1 RELEASE_KEY_FPR="$F1"

sha() { python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }
sign() { GNUPGHOME="$1" gpg --batch --quiet --pinentry-mode loopback --passphrase pw --armor --detach-sign --output "$3" "$2" 2>/dev/null; }

# build <dir> <version>: a well-formed release directory signed by k1.
build() {
  local d="$1" v="$2" tag="module-v$2"
  mkdir -p "$d" "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux" \
    "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.AspNetCore" \
    "$W/src/mod_pagespeed-$v/aspnetcore/WeAmp.PageSpeed.AspNetCore.Tests"
  local a="$W/src/mod_pagespeed-$v/aspnetcore"
  echo '<Project/>' > "$a/Directory.Build.props"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux.csproj"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.AspNetCore/WeAmp.PageSpeed.AspNetCore.csproj"
  echo '<Project/>' > "$a/WeAmp.PageSpeed.AspNetCore.Tests/WeAmp.PageSpeed.AspNetCore.Tests.csproj"
  tar -C "$W/src" -czf "$d/mod_pagespeed-$v.tar.gz" "mod_pagespeed-$v"
  python3 -I - "$d" "$v" "$tag" <<'PY'
import hashlib, json, os, sys, tarfile, io
d, v, tag = sys.argv[1:4]
rids = {}
for rid in ("linux-x64", "linux-arm64"):
    top = "pagespeed-sidecar-native-%s-%s" % (v, rid)
    files = {"nginx": b"#!nginx " + rid.encode(), "ngx_pagespeed_module.so": b"so " + rid.encode(),
             "pagespeed-nginx-launch": b"#!launch " + rid.encode(), "build-info.json": b"{}"}
    path = os.path.join(d, top + ".tar.gz")
    with tarfile.open(path, "w:gz") as t:
        di = tarfile.TarInfo(top); di.type = tarfile.DIRTYPE; di.mode = 0o755; t.addfile(di)
        for n, b in files.items():
            ti = tarfile.TarInfo(top + "/" + n); ti.size = len(b)
            ti.mode = 0o755 if n in ("nginx", "pagespeed-nginx-launch") else 0o644
            t.addfile(ti, io.BytesIO(b))
    raw = open(path, "rb").read()
    rids[rid] = {"asset": top + ".tar.gz", "sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw),
                 "files": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()}}
json.dump({"schema": 1, "tag": tag, "version": v, "nginx_version": "0", "glibc_floor": "0",
           "source_sha": "a" * 40, "rids": rids}, open(os.path.join(d, "sidecar-pair.json"), "w"))
PY
  sums "$d"
}
# sums <dir> [signer]: (re)writes SHA256SUMS over every asset but SHA256SUMS(.asc) and signs it.
sums() {
  local d="$1" f
  rm -f "$d/SHA256SUMS" "$d/SHA256SUMS.asc"
  ( cd "$d" && for f in *; do printf '%s  %s\n' "$(sha "$f")" "$f"; done > "$W/sums.tmp" )
  mv "$W/sums.tmp" "$d/SHA256SUMS"
  sign "${2:-$W/k1}" "$d/SHA256SUMS" "$d/SHA256SUMS.asc"
}
copy() { rm -rf "$2"; cp -R "$1" "$2"; }

build "$W/good" 9.9.9
PUB="$W/k1.pub"
expect ok "verify: a well-formed signed release"           bash "$FETCH" verify module-v9.9.9 "$W/good" "$PUB"
expect ok "verify: prints the SHA256SUMS digest"           bash -c "bash '$FETCH' verify module-v9.9.9 '$W/good' '$PUB' | grep -q \"SHA256SUMS sha256 $(sha "$W/good/SHA256SUMS")\""
expect ok "stage: writes the tree and both pairs"          bash "$FETCH" stage module-v9.9.9 "$W/good" "$PUB" "$W/work"
for rid in linux-x64 linux-arm64; do
  if [ -x "$W/work/aspnetcore/WeAmp.PageSpeed.Sidecar.NativeAssets.Linux/runtimes/$rid/native/nginx" ]; then
    echo "ok   stage: $rid nginx is executable"
  else
    echo "FAIL stage: $rid nginx missing or not executable"; fail=1
  fi
done
expect no "stage: refuses an existing work tree"           bash "$FETCH" stage module-v9.9.9 "$W/good" "$PUB" "$W/work"

expect no "verify: tag is not module-v<version>"           bash "$FETCH" verify v9.9.9 "$W/good" "$PUB"
expect no "verify: tag names another version"              bash "$FETCH" verify module-v9.9.8 "$W/good" "$PUB"
expect no "verify: signed by another key"                  bash -c "cd '$W' && cp -R good other && cd other && rm SHA256SUMS.asc && GNUPGHOME='$W/k2' gpg --batch --quiet --pinentry-mode loopback --passphrase pw --armor --detach-sign --output SHA256SUMS.asc SHA256SUMS && bash '$FETCH' verify module-v9.9.9 '$W/other' '$PUB'"

copy "$W/good" "$W/t1"; echo x >> "$W/t1/SHA256SUMS"
expect no "verify: SHA256SUMS changed after signing"       bash "$FETCH" verify module-v9.9.9 "$W/t1" "$PUB"
copy "$W/good" "$W/t2"; rm "$W/t2/SHA256SUMS.asc"
expect no "verify: signature missing"                      bash "$FETCH" verify module-v9.9.9 "$W/t2" "$PUB"
copy "$W/good" "$W/t3"; echo x >> "$W/t3/pagespeed-sidecar-native-9.9.9-linux-x64.tar.gz"
expect no "verify: pair tarball changed, still signed over the old bytes" bash "$FETCH" verify module-v9.9.9 "$W/t3" "$PUB"
copy "$W/good" "$W/t4"; echo x >> "$W/t4/pagespeed-sidecar-native-9.9.9-linux-x64.tar.gz"; sums "$W/t4"
expect no "verify: pair tarball changed and re-signed (manifest digest)" bash "$FETCH" verify module-v9.9.9 "$W/t4" "$PUB"
copy "$W/good" "$W/t5"; echo x >> "$W/t5/mod_pagespeed-9.9.9.tar.gz"
expect no "verify: source tarball changed after signing"   bash "$FETCH" verify module-v9.9.9 "$W/t5" "$PUB"
copy "$W/good" "$W/t6"; echo junk > "$W/t6/extra.txt"; sums "$W/t6"
expect no "verify: extra file in the directory"            bash "$FETCH" verify module-v9.9.9 "$W/t6" "$PUB"
copy "$W/good" "$W/t7"; python3 -I -c 'import json,sys; p=sys.argv[1]; m=json.load(open(p)); m["tag"]="module-v9.9.8"; json.dump(m,open(p,"w"))' "$W/t7/sidecar-pair.json"; sums "$W/t7"
expect no "verify: signed manifest names another tag"      bash "$FETCH" verify module-v9.9.9 "$W/t7" "$PUB"
copy "$W/good" "$W/t8"; grep -v ' sidecar-pair.json$' "$W/t8/SHA256SUMS" > "$W/t8.sums"; mv "$W/t8.sums" "$W/t8/SHA256SUMS"; sign "$W/k1" "$W/t8/SHA256SUMS" "$W/t8/SHA256SUMS.asc.new"; mv "$W/t8/SHA256SUMS.asc.new" "$W/t8/SHA256SUMS.asc"
expect no "verify: SHA256SUMS does not list the manifest"  bash "$FETCH" verify module-v9.9.9 "$W/t8" "$PUB"

# The tag is derived, never read from a file; a real run needs the current version.
printf '2.8.0\n' > "$W/VERSION.txt"
printf '2.7.0\n' > "$W/VERSION.old"
: > "$W/VERSION.empty"
expect ok "resolve: real run, v<X> takes module-v<X>"      bash -c "[ \"\$(bash '$FETCH' resolve v2.8.0 real '$W/VERSION.txt')\" = module-v2.8.0 ]"
expect no "resolve: real run of an older tag"              bash "$FETCH" resolve v2.7.0 real "$W/VERSION.txt"
expect no "resolve: real run, main is at an older version" bash "$FETCH" resolve v2.8.0 real "$W/VERSION.old"
expect no "resolve: real run without VERSION.txt"          bash "$FETCH" resolve v2.8.0 real
expect no "resolve: real run, empty VERSION.txt"           bash "$FETCH" resolve v2.8.0 real "$W/VERSION.empty"
expect no "resolve: real run, VERSION.txt missing"         bash "$FETCH" resolve v2.8.0 real "$W/none"
# VERSION.txt names the rehearsal tag itself, so only the explicit refusal can stop this.
printf '2.8.0-pre.1\n' > "$W/VERSION.pre"
expect no "resolve: real run of a rehearsal tag"           bash "$FETCH" resolve v2.8.0-pre.1 real "$W/VERSION.pre"
expect no "resolve: real run of a rehearsal tag (VERSION.txt at the base)" bash "$FETCH" resolve v2.8.0-pre.1 real "$W/VERSION.txt"
expect no "resolve: refuses a malformed pre-release tag"   bash "$FETCH" resolve v2.8.0-rc.1 dry
expect no "resolve: refuses a pre-release tag without N"   bash "$FETCH" resolve v2.8.0-pre dry
expect no "resolve: refuses a module tag"                  bash "$FETCH" resolve module-v2.8.0 real "$W/VERSION.txt"
expect no "resolve: refuses an unknown mode"               bash "$FETCH" resolve v2.8.0 latest
if [ -e "$ROOT/tools/ci/sidecar-pin.json" ]; then echo "FAIL tools/ci/sidecar-pin.json still exists"; fail=1; else echo "ok   no pin file"; fi

# fetch and resolve against a stubbed GitHub: the fixture release above, with
# the API answers each case needs.
mkdir "$W/bin"
cat > "$W/bin/curl" <<'STUB'
#!/bin/bash
# Stand-in for curl: serves $STUB_DIR answers and the fixture release at $STUB_FILES.
o=""; w=""; url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) o="$2"; shift 2 ;;
    -w) w="$2"; shift 2 ;;
    -H|--retry) shift 2 ;;
    http*) url="$1"; shift ;;
    *) shift ;;
  esac
done
case "$url" in
  */releases/tags/*) f="$STUB_DIR/tag-${url##*/}.json" ;;
  */releases/latest) f="$STUB_DIR/latest.json" ;;
  */releases/download/*) f="$STUB_FILES/${url##*/}" ;;
  */git/ref/tags/*) f="$STUB_DIR/ref.json" ;;
  */git/tags/*) f="$STUB_DIR/tagobj.json" ;;
  *) echo "stub curl: unexpected $url" >&2; exit 22 ;;
esac
if [ ! -e "$f" ]; then
  if [ -n "$w" ]; then printf '404'; exit 0; fi
  exit 22
fi
[ -z "$w" ] || printf '200'
if [ -n "$o" ]; then cp "$f" "$o"; elif [ -z "$w" ]; then cat "$f"; fi
STUB
chmod +x "$W/bin/curl"
export STUB_FILES="$W/good"
SHA_A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
SHA_B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

# stub <n> <python statements editing doc>: a fresh API answer set in $STUB_DIR.
stub() {
  STUB_DIR="$W/s$1"; rm -rf "$STUB_DIR"; mkdir -p "$STUB_DIR"; export STUB_DIR
  python3 -I - "$W/good" "$STUB_DIR/tag-module-v9.9.9.json" "$2" <<'PY'
import json, os, sys
d, out, edit = sys.argv[1:4]
base = "https://github.com/We-Amp/mod_pagespeed/releases/download/module-v9.9.9/"
names = sorted(os.listdir(d))
doc = {"draft": False, "prerelease": False, "published_at": "2026-01-01T00:00:10Z",
       "assets": [{"name": n, "updated_at": "2026-01-01T00:00:00Z", "browser_download_url": base + n} for n in names]}
doc["assets"].append({"name": "unrelated-debug.tar.gz", "updated_at": "2026-01-01T00:00:00Z", "browser_download_url": "https://example.invalid/x"})
exec(edit)
json.dump(doc, open(out, "w"))
PY
  printf '{"object":{"type":"commit","sha":"%s"}}' "$SHA_A" > "$STUB_DIR/ref.json"
}
# shellcheck disable=SC2329 # invoked through expect
fetch() { rm -rf "$W/o"; PATH="$W/bin:$PATH" bash "$FETCH" fetch module-v9.9.9 "$W/o" "$PUB"; }

stub 1 "pass";                                                         expect ok "fetch: a good release (lightweight tag)" fetch
stub 2 "doc['draft']=True";                                            expect no "fetch: draft release" fetch
stub 3 "doc['prerelease']=True";                                       expect no "fetch: pre-release" fetch
stub 4 "doc['assets'][0]['updated_at']='2026-01-01T00:00:11Z'";        expect no "fetch: an input modified after publication" fetch
stub 5 "doc['assets'][-1]['updated_at']='2026-02-01T00:00:00Z'";       expect ok "fetch: an unrelated asset modified later is not an input" fetch
stub 6 "[a.pop('updated_at') for a in doc['assets']]";                 expect no "fetch: updated_at missing (fails closed)" fetch
stub 7 "doc['published_at']=None";                                     expect no "fetch: published_at null" fetch
stub 8 "doc.pop('published_at')";                                      expect no "fetch: published_at missing" fetch
stub 9 "doc['assets'].append(dict(doc['assets'][0]))";                 expect no "fetch: duplicate asset name" fetch
stub 10 "doc['assets'][0]['browser_download_url']='https://example.invalid/x'"; expect no "fetch: download URL outside the module release" fetch
stub 11 "pass"; printf '{"object":{"type":"commit","sha":"%s"}}' "$SHA_B" > "$STUB_DIR/ref.json"
                                                                       expect no "fetch: tag moved (lightweight)" fetch
stub 12 "pass"; printf '{"object":{"type":"tag","sha":"%s"}}' "$SHA_B" > "$STUB_DIR/ref.json"; printf '{"object":{"type":"commit","sha":"%s"}}' "$SHA_A" > "$STUB_DIR/tagobj.json"
                                                                       expect ok "fetch: annotated tag, right commit" fetch
stub 13 "pass"; printf '{"object":{"type":"tag","sha":"%s"}}' "$SHA_A" > "$STUB_DIR/ref.json"; printf '{"object":{"type":"commit","sha":"%s"}}' "$SHA_B" > "$STUB_DIR/tagobj.json"
                                                                       expect no "fetch: tag moved (annotated)" fetch
stub 14 "pass"; echo '{}' > "$STUB_DIR/ref.json";                      expect no "fetch: tag ref garbled" fetch
stub 15 "pass"; echo 'not json' > "$STUB_DIR/ref.json";                expect no "fetch: tag ref not JSON" fetch
stub 16 "pass"; rm "$STUB_DIR/ref.json";                               expect no "fetch: tag ref unknown" fetch
stub 17 "pass"; printf '{"object":{"type":"tree","sha":"%s"}}' "$SHA_A" > "$STUB_DIR/ref.json"
                                                                       expect no "fetch: tag ref is not a commit" fetch
stub 18 "pass"; rm "$STUB_DIR/tag-module-v9.9.9.json";                 expect no "fetch: no such release" fetch

# resolve, dry run: module-v<X> when it exists, else the newest published release.
stub 20 "pass"; printf '{"tag_name":"module-v9.9.8"}' > "$STUB_DIR/latest.json"
# shellcheck disable=SC2329 # invoked through expect
dry() { PATH="$W/bin:$PATH" bash "$FETCH" resolve "$1" dry; }
expect ok "resolve: dry run, module release exists"        bash -c "[ \"\$(PATH='$W/bin':\$PATH bash '$FETCH' resolve v9.9.9 dry)\" = module-v9.9.9 ]"
expect ok "resolve: dry run, not cut yet, newest published" bash -c "[ \"\$(PATH='$W/bin':\$PATH bash '$FETCH' resolve v9.9.10 dry)\" = module-v9.9.8 ]"
printf '{"tag_name":"something-else"}' > "$STUB_DIR/latest.json"
expect no "resolve: dry run, newest release is not a module tag" dry v9.9.10
printf '{"tag_name":"module-v9.9.8"}' > "$STUB_DIR/latest.json"
cp "$STUB_DIR/tag-module-v9.9.9.json" "$STUB_DIR/tag-module-v9.9.9-pre.1.json"
expect ok "resolve: dry run of a rehearsal tag takes the base module release, never module-v<X>-pre.<N>" \
  bash -c "[ \"\$(PATH='$W/bin':\$PATH bash '$FETCH' resolve v9.9.9-pre.1 dry)\" = module-v9.9.9 ]"
expect ok "resolve: dry run of a rehearsal tag, base not cut yet, newest published" \
  bash -c "[ \"\$(PATH='$W/bin':\$PATH bash '$FETCH' resolve v9.9.10-pre.2 dry)\" = module-v9.9.8 ]"
rm "$STUB_DIR/latest.json"
expect no "resolve: dry run, nothing published"            dry v9.9.10
expect no "resolve: dry run of a rehearsal tag, nothing published" dry v9.9.10-pre.1

if [ -n "${SIDECAR_LIVE_TAG:-}" ]; then
  # The repository's own release key and fingerprint, not the self-test key.
  expect ok "live: fetch, verify and post-publication checks of $SIDECAR_LIVE_TAG" \
    env -u RELEASE_SIG_SELF_TEST -u RELEASE_KEY_FPR bash "$FETCH" fetch "$SIDECAR_LIVE_TAG" "$W/live" "$ROOT/RELEASE-SIGNING-KEY.asc"
  expect ok "live: stage the fetched release" \
    env -u RELEASE_SIG_SELF_TEST -u RELEASE_KEY_FPR bash "$FETCH" stage "$SIDECAR_LIVE_TAG" "$W/live" "$ROOT/RELEASE-SIGNING-KEY.asc" "$W/live-work"
fi

[ "$fail" = 0 ] && echo "All sidecar fetch cases passed" || echo "sidecar fetch self-test FAILED"
exit "$fail"
