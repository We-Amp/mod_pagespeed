#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for tools/ci/nupkg_checks.sh. Builds small synthetic packages (no
# network, no dotnet) and checks that the offline checks accept a good set
# and reject each defect they claim to catch.
#
# Run: tools/ci/test_nupkg_checks.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHK="$HERE/nupkg_checks.sh"
W="$(mktemp -d "${TMPDIR:-/tmp}/nupkgchk.XXXXXX")"
trap 'rm -rf "$W"' EXIT
V=9.9.9

# mkset <dir> [mutation]: seven synthetic packages.
mkset() {
  python3 -I - "$1" "${2:-}" "${VV:-$V}" <<'PY'
import json, os, struct, sys, zipfile
d, mut, v = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(d, exist_ok=True)
ids = ["WeAmp.PageSpeed", "WeAmp.PageSpeed.AspNetCore", "WeAmp.PageSpeed.NativeAssets.Linux",
       "WeAmp.PageSpeed.NativeAssets.Windows", "WeAmp.PageSpeed.NativeAssets.macOS",
       "WeAmp.PageSpeed.Sidecar", "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux"]
REPO = "https://github.com/We-Amp/pagespeed-optimizer"
SHA = "a" * 40
RT = "runtimes/%s/native/"
NATIVE = {
    "WeAmp.PageSpeed.NativeAssets.Linux": [RT % r + f for r in ("linux-x64", "linux-arm64") for f in ("libpagespeed.so", "factory_worker", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.NativeAssets.Windows": [RT % "win-x64" + f for f in ("pagespeed.dll", "factory_worker.exe", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.NativeAssets.macOS": [RT % "osx-arm64" + f for f in ("libpagespeed.dylib", "factory_worker", "BUILD_INFO.json")],
    "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux": [RT % r + f for r in ("linux-x64", "linux-arm64") for f in ("nginx", "ngx_pagespeed_module.so", "pagespeed-nginx-launch", "build-info.json")],
}
def pe(signed):
    b = bytearray(512); b[0:2] = b"MZ"; struct.pack_into("<I", b, 0x3c, 0x80)
    b[0x80:0x84] = b"PE\0\0"; struct.pack_into("<H", b, 0x80 + 24, 0x20b)
    if signed:
        struct.pack_into("<II", b, 0x80 + 24 + 112 + 4 * 8, 400, 100)
    return bytes(b)
for i in ids:
    ver = v
    name = "%s.%s.nupkg" % (i, v)
    deps = ""
    if i.endswith("AspNetCore"):
        deps = '<dependencies><group targetFramework="net10.0"><dependency id="WeAmp.PageSpeed" version="%s" /></group></dependencies>' % (("1.0.0" if mut == "unpinned" else v))
    if i == "WeAmp.PageSpeed.Sidecar":
        deps = '<dependencies><group targetFramework="net10.0"><dependency id="WeAmp.PageSpeed.Sidecar.NativeAssets.Linux" version="%s" /></group></dependencies>' % v
    repo = "" if i.startswith("WeAmp.PageSpeed.Sidecar") else '<repository type="git" url="%s" commit="%s" />' % (REPO if not (mut == "badrepo" and i == "WeAmp.PageSpeed") else "https://example.invalid/x", SHA)
    if mut == "sourcelink" and i == "WeAmp.PageSpeed": repo += "<sourceLink />"
    if mut == "nuspecver" and i == "WeAmp.PageSpeed": ver = "1.2.3"
    if mut == "norepo" and i == "WeAmp.PageSpeed": repo = ""
    nuspec = ('<?xml version="1.0"?><package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd"><metadata>'
              '<id>%s</id><version>%s</version><authors>We-Amp</authors><description>d</description>'
              '<license type="expression">Apache-2.0</license><copyright>c</copyright><icon>icon.png</icon>%s%s</metadata></package>') % (i, ver, repo, deps)
    if mut == "nolicense" and i == "WeAmp.PageSpeed": nuspec = nuspec.replace('<license type="expression">Apache-2.0</license>', "")
    if mut == "noicon" and i == "WeAmp.PageSpeed": nuspec = nuspec  # icon declared, not shipped (below)
    if mut == "missing" and i == "WeAmp.PageSpeed.Sidecar": continue
    if mut == "missingnative" and i == "WeAmp.PageSpeed.Sidecar.NativeAssets.Linux": continue
    z = zipfile.ZipFile(os.path.join(d, name), "w")
    z.writestr("[Content_Types].xml", "<Types/>"); z.writestr("_rels/.rels", "<Relationships/>")
    z.writestr(i + ".nuspec", nuspec)
    if not (mut == "noicon" and i == "WeAmp.PageSpeed"): z.writestr("icon.png", b"png")
    if i.endswith("AspNetCore"):
        z.writestr("lib/net10.0/WeAmp.PageSpeed.AspNetCore.dll", b"MZ\0\0" + (b"" if mut == "nospa" else b"\0WeAmp.PageSpeed.AspNetCore.Console/index.html\0 xx"))
    for nf in NATIVE.get(i, []):
        if mut == "nonative" and nf.endswith("factory_worker") and i.endswith("Linux") and "Sidecar" not in i: continue
        if nf.endswith("/BUILD_INFO.json"): z.writestr(nf, json.dumps({"git_sha": SHA}))
        elif nf.endswith(("pagespeed.dll", "factory_worker.exe")):
            if mut == "nope": continue
            z.writestr(nf, pe(mut != "unsigned-win"))
        elif mut == "emptynative" and nf.endswith("libpagespeed.so") and "x64" in nf: z.writestr(nf, b"")
        else: z.writestr(nf, b"bin")
    z.close()
if mut == "extra": open(os.path.join(d, "WeAmp.Other.9.9.9.nupkg"), "wb").write(b"x")
if mut in ("pinbase", "pinother"):
    named = "module-v" + v.split("-pre.")[0] if mut == "pinbase" else "module-v0.0.1"
    open(os.path.join(d, "README-SIDECAR-PIN.txt"), "w").write("DRY RUN ONLY: contain the native pair and the managed code of %s, not of module-v%s." % (named, v))
if mut == "corrupt":
    p = os.path.join(d, "WeAmp.PageSpeed.%s.nupkg" % v); open(p, "wb").write(b"not a zip")
PY
}

fail=0
t() { # t ok|no <desc> <mutation> <check...>
  local want="$1" desc="$2" mut="$3"; shift 3
  rm -rf "$W/set"; mkset "$W/set" "$mut"
  if "$CHK" "$@" >"$W/out" 2>&1; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}
S="$W/set"
t ok 'a good set passes every offline check'           ''          offline "$S" $V signed
t ok 'a note only warns for a non-candidate'          pinother    offline "$S" $V signed
PIN_NOTE_FATAL=1 t no 'a note on a release candidate fails' pinother    identity "$S" $V
VV=9.9.9-pre.1 PIN_NOTE_FATAL=1 t no 'a note on a prerelease candidate fails' pinother identity "$S" 9.9.9-pre.1
VV=9.9.9-pre.1 t ok 'a note on a prerelease warns when not a candidate' pinother identity "$S" 9.9.9-pre.1
VV=9.9.9-pre.1 PIN_NOTE_FATAL=1 t ok 'a prerelease dry run without a note passes' '' identity "$S" 9.9.9-pre.1
EXPECT_SHA=$(printf 'a%.0s' $(seq 40)) t ok 'the expected commit matches' '' identity "$S" $V
EXPECT_SHA=$(printf 'b%.0s' $(seq 40)) t no 'a different expected commit' '' identity "$S" $V
t no 'a native binary missing from a package'          nonative    identity "$S" $V
t no 'an empty native binary'                          emptynative identity "$S" $V
t no 'signing: no PE file in the Windows package'      nope        signing "$S" report
t no 'six packages'                                    missing     expect-seven "$S" $V
t no 'sidecar native package missing'                  missingnative expect-seven "$S" $V
t no 'an eighth package'                               extra       expect-seven "$S" $V
t no 'an unexpected package id (identity)'             extra       identity "$S" $V
t no 'a wrong version on the command line'             ''          identity "$S" 9.9.8
t no 'a nuspec version that differs from the file'     nuspecver   identity "$S" $V
t no 'an internal dependency not pinned to the version' unpinned   identity "$S" $V
t no 'no license expression'                           nolicense   identity "$S" $V
t no 'a declared icon that is not in the package'      noicon      identity "$S" $V
t no 'a package that is not a zip'                     corrupt     identity "$S" $V
t no 'a SourceLink element'                            sourcelink  metadata "$S"
t no 'a repository url that is not the public repo'    badrepo     metadata "$S"
t no 'no repository url on a non-sidecar package'      norepo      metadata "$S"
t no 'the console SPA missing from the dll'            nospa       console-spa "$S"
t ok 'signed Windows binaries when signed is expected' ''          signing "$S" signed
t no 'unsigned Windows binaries when signed is expected' unsigned-win signing "$S" signed
t ok 'unsigned Windows binaries when unsigned is expected' unsigned-win signing "$S" unsigned
t no 'signed Windows binaries when unsigned is expected' ''        signing "$S" unsigned
t no 'a bad signing mode'                              ''          signing "$S" maybe

# ---- nuget-org: only a well-formed answer reads as "new" (read-only, loopback server)
cat > "$W/readsrv.py" <<'PY'
import http.server, sys
mode, portfile, ver = sys.argv[1:4]
BODY = {
    "empty": (200, b""), "html": (200, b"<html>maintenance</html>"), "trunc": (200, b'{"versions":["1.0.0"'),
    "nonarray": (200, b'{"versions":"1.0.0"}'), "numbers": (200, b'{"versions":[1,2]}'), "noversions": (200, b"{}"),
    "array": (200, b'[]'), "404": (404, b"{}"),
    "new": (200, b'{"versions":["1.0.0"]}'), "dup": (200, ('{"versions":["1.0.0","%s"]}' % ver).encode()),
}[mode]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        self.send_response(BODY[0]); self.send_header("Content-Length", str(len(BODY[1]))); self.end_headers(); self.wfile.write(BODY[1])
srv = http.server.HTTPServer(("127.0.0.1", 0), H)
open(portfile, "w").write(str(srv.server_port)); srv.serve_forever()
PY
rm -rf "$W/set"; mkset "$W/set" ''
org() { # org ok|no <desc> <mode> [env...]
  local want="$1" desc="$2" mode="$3"; shift 3
  rm -f "$W/port"; python3 -I "$W/readsrv.py" "$mode" "$W/port" $V & local pid=$!
  for _ in $(seq 1 50); do [ -s "$W/port" ] && break; sleep 0.1; done
  if env NUGET_ORG_BASE="http://127.0.0.1:$(cat "$W/port")" "$@" "$CHK" nuget-org "$W/set" >"$W/out" 2>&1; then got=ok; else got=no; fi
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; sed 's/^/     /' "$W/out"; fail=1; fi
}
org no 'nuget.org: an empty 200 body is not "new"'          empty
org no 'nuget.org: an HTML 200 body is not "new"'           html
org no 'nuget.org: truncated JSON is not "new"'             trunc
org no 'nuget.org: versions that is not an array'           nonarray
org no 'nuget.org: versions that are not strings'           numbers
org no 'nuget.org: a body without versions'                 noversions
org no 'nuget.org: a JSON array instead of an object'       array
org no 'nuget.org: a 404 for an id not announced as new'    404
org ok 'nuget.org: a 404 for ids announced as first packages' 404 NUGET_ORG_FIRST_PACKAGES="$(sed -n 's/^IDS="\(.*\)"$/\1/p' "$CHK")"
org ok 'nuget.org: a well-formed list without the version'  new
org no 'nuget.org: the version listed, expect new'          dup NUGET_ORG_EXPECT=new
org ok 'nuget.org: the version listed, report only'         dup

# ---- the mock feed refuses ids that would escape its directory
rm -f "$W/port"; python3 -I "$HERE/nuget_mock_feed.py" "$W/mockfeed" "$W/port" & mpid=$!
for _ in $(seq 1 50); do [ -s "$W/port" ] && break; sleep 0.1; done
M="http://127.0.0.1:$(cat "$W/port")/api/v2/package"
python3 -I - "$W" <<'PY'
import sys, zipfile
w = sys.argv[1]
for name, pid in (("escape", "x/../../escaped"), ("dots", "a..b"), ("good", "Good.Pkg")):
    z = zipfile.ZipFile("%s/%s.nupkg" % (w, name), "w")
    z.writestr("p.nuspec", "<package><metadata><id>%s</id><version>1.0.0</version></metadata></package>" % pid); z.close()
PY
for n in escape dots; do
  c="$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @"$W/$n.nupkg" "$M")"
  if [ "$c" = 400 ]; then echo "ok   the mock refuses id of $n.nupkg (400)"; else echo "FAIL the mock answered $c for $n.nupkg"; fail=1; fi
done
c="$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @"$W/good.nupkg" "$M")"
if [ "$c" = 201 ]; then echo "ok   the mock accepts a plain id (201)"; else echo "FAIL the mock answered $c for a plain id"; fail=1; fi
c="$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @"$W/good.nupkg" "$M")"
if [ "$c" = 409 ]; then echo "ok   the mock answers 409 for the duplicate"; else echo "FAIL the mock answered $c for a duplicate"; fail=1; fi
[ -z "$(find "$W" -name 'escaped*' -not -path "$W/set/*")" ] && echo "ok   nothing was written outside the feed directory" || { echo "FAIL a file escaped the feed directory"; fail=1; }
kill "$mpid" 2>/dev/null; wait "$mpid" 2>/dev/null
if [ "$fail" = 0 ]; then echo PASS; else echo FAILED; exit 1; fi
