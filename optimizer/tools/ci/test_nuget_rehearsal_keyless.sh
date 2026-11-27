#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Asserts the NuGet publish rehearsal is structurally without a NuGet key.
# Negative assertions: its workflow files name no environment, read no
# secret, ask for no OIDC token or write permission, never log in to
# nuget.org, and the only `nuget push` in the script targets the loopback
# feed. Positive assertions: the exact permissions blocks, the allowed
# triggers, one hosted runner label, a checkout of main without persisted
# credentials, `feed_url` assigned once to a loopback address, a mock feed
# that advertises loopback only. Then it proves each assertion bites by
# running the checks against deliberately broken copies.
#
# Run: tools/ci/test_nuget_rehearsal_keyless.sh   (no network, no dotnet)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$(mktemp -d "${TMPDIR:-/tmp}/keyless.XXXXXX")"
trap 'rm -rf "$W"' EXIT

C=.github/workflows/nuget-rehearsal.yml
P=.github/workflows/pipeline-nuget-rehearsal.yml
SCRIPT=tools/ci/nupkg_checks.sh
MOCK=tools/ci/nuget_mock_feed.py
FILES="$C $P"

# check <root>: one line per violation; non-zero if any.
check() {
  python3 -I - "$1" "$C" "$P" "$SCRIPT" "$MOCK" <<'PY'
import os, re, sys
root, C, P, SCRIPT, MOCK = sys.argv[1:6]
errs = []
def err(m): errs.append(m)
def rd(f):
    p = os.path.join(root, f)
    if not os.path.isfile(p) or os.path.getsize(p) == 0:
        err("missing " + f); return ""
    return open(p).read()
def code(t):  # drop comment-only lines and trailing comments
    out = []
    for l in t.split("\n"):
        if re.match(r"^\s*#", l): continue
        out.append(re.sub(r"\s#\s.*$", "", l))
    return "\n".join(out)
def block(t, key, indent):  # lines nested under `key:` at the given indent
    lines, res, on = t.split("\n"), [], False
    for l in lines:
        if on:
            if l.strip() == "": continue
            if len(l) - len(l.lstrip()) > indent: res.append(l.strip()); continue
            on = False
        if re.match(r"^ {%d}%s:\s*$" % (indent, re.escape(key)), l): on = True; res = []; found = True
    return res
ct, pt, st, mt = rd(C), rd(P), rd(SCRIPT), rd(MOCK)
if errs: print("\n".join(errs)); sys.exit(1)
cc, pc, sc = code(ct), code(pt), code(st)

# ---- negative assertions on both workflows
neg = [
    (r"^\s*environment\s*:", "names an environment"),
    (r"secrets[.\[]", "reads a secret"), (r"^\s*secrets\s*:", "passes secrets"), (r"inherit", "inherits secrets"),
    (r"id-token", "asks for an OIDC token"), (r"[Ww]rite", "mentions write (permission)"),
    (r"NuGet/login", "logs in to nuget.org"), (r"NUGET_[A-Z_]*(KEY|SOURCE)|--api-key|-ApiKey", "handles a NuGet key or source variable"),
    (r"nuget\s+(push|delete|add|setapikey)", "runs a nuget push/delete/add/setapikey"),
    (r"AZURE_|RELEASE_GPG|CORP_DISPATCH_TOKEN", "names a release secret"),
    (r"uses:\s*\S*(release\.yml|pipeline-release)", "calls the release lane"),
    (r"curl[^\n]*(-X|--request)\s*(PUT|POST|PATCH|DELETE)|curl[^\n]*(-T\b|--upload-file|--data|-d\s)", "sends data with curl"),
    (r"--method|-X\s*(PUT|POST|PATCH|DELETE)", "makes a non-GET API call"),
    (r"gh\s+release\s+(create|upload|edit|delete)", "writes a release"),
]
for name, txt in (("caller", cc), ("rehearsal", pc)):
    for rx, msg in neg:
        if re.search(rx, txt, re.M): err("%s: %s" % (name, msg))

# ---- positive assertions
# triggers
if block(ct, "on", 0) and sorted(l.split(":")[0] for l in block(ct, "on", 0) if re.match(r"^[a-z_]+:", l) and not l.startswith(("description", "type", "default", "required", "options"))):
    keys = [m.group(1) for m in re.finditer(r"^  ([a-z_]+):\s*$", ct.split("\njobs:")[0], re.M)]
    top = [k for k in keys if k in ("push", "pull_request", "pull_request_target", "schedule", "workflow_run", "release", "workflow_dispatch", "workflow_call", "repository_dispatch", "issue_comment", "create", "delete")]
    if top != ["workflow_dispatch"]: err("caller: triggers are %s, expected only workflow_dispatch" % top)
else:
    err("caller: no on: block")
pkeys = [m.group(1) for m in re.finditer(r"^  ([a-z_]+):\s*$", pt.split("\njobs:")[0], re.M)]
ptop = [k for k in pkeys if k in ("push", "pull_request", "pull_request_target", "schedule", "workflow_run", "release", "workflow_dispatch", "workflow_call", "repository_dispatch", "issue_comment", "create", "delete")]
if ptop != ["workflow_call"]: err("rehearsal: triggers are %s, expected only workflow_call" % ptop)
# permissions
want_p = ["actions: read", "contents: read"]
if sorted(block(ct, "permissions", 0)) != ["contents: read"]: err("caller: top-level permissions are not exactly contents: read")
if sorted(block(ct.split("\njobs:")[1] if "\njobs:" in ct else "", "permissions", 4)) != want_p: err("caller: the job's permissions are not exactly actions: read, contents: read")
if sorted(block(pt, "permissions", 0)) != want_p: err("rehearsal: top-level permissions are not exactly actions: read, contents: read")
if re.search(r"^    permissions:", pt, re.M) and sorted(block(pt, "permissions", 4)) != want_p: err("rehearsal: a job's permissions differ")
# one hosted runner label
labels = re.findall(r"^\s*runs-on:\s*(.*?)\s*$", pc, re.M)
if not labels or any(l != "ubuntu-24.04" for l in labels): err("rehearsal: runs-on is %s, expected only ubuntu-24.04" % labels)
if re.search(r"runs-on", cc): err("caller: has a runs-on")
# checkout of main without credentials
uses = re.findall(r"^\s*-?\s*uses:\s*(\S+)", pc, re.M)
for u in uses:
    if not re.match(r"actions/(checkout|setup-dotnet)@[0-9a-f]{40}$", u): err("rehearsal: unexpected or unpinned action %s" % u)
if not re.search(r"uses: actions/checkout@[0-9a-f]{40}[^\n]*\n\s+with:\n\s+ref: main\n\s+persist-credentials: false\n", pc): err("rehearsal: the checkout is not exactly ref: main with persist-credentials: false")
# caller: one job, one uses
cu = re.findall(r"^\s*uses:\s*(\S+)", cc, re.M)
if cu != ["We-Amp/pagespeed-optimizer/.github/workflows/pipeline-nuget-rehearsal.yml@main"]: err("caller: uses %s" % cu)
if len(re.findall(r"^  [a-z][a-z-]*:\s*$", ct.split("\njobs:")[1] if "\njobs:" in ct else "", re.M)) != 1: err("caller: does not have exactly one job")
# script: the only push targets feed_url; feed_url assigned once, loopback
pushes = [l for l in sc.split("\n") if re.search(r"nuget\s+push", l)]
if not pushes: err(SCRIPT + ": no push found (test needs updating)")
for l in pushes:
    if '--source "$feed_url"' not in l: err(SCRIPT + ": a nuget push that does not target $feed_url")
    if re.search(r"--api-key|-k\s", l): err(SCRIPT + ": a push with a key")
assign = re.findall(r"^\s*feed_url=(.*)$", sc, re.M)
if len(assign) != 1 or not re.match(r'"http://127\.0\.0\.1:\$\{?port\}?/', assign[0]): err(SCRIPT + ": feed_url is not assigned exactly once to a loopback address (%s)" % assign)
if re.search(r"NUGET_SOURCE|--api-key|NUGET_API_KEY|-ApiKey|dotnet\s+nuget\s+(delete|add|setapikey)", sc): err(SCRIPT + ": handles a NuGet key or source variable")
if re.search(r"(curl|wget)[^\n]*(-X\s*\w+|--request|-T\b|--upload-file|--data|--post|-d\s)", sc): err(SCRIPT + ": sends data with curl or wget")
# mock: loopback only
hosts = set(re.findall(r"https?://([^/\"%:\s]+)", mt))
if hosts - {"127.0.0.1"}: err(MOCK + ": names a host other than loopback: %s" % sorted(hosts))
if not re.search(r'ThreadingHTTPServer\(\("127\.0\.0\.1", 0\)', mt): err(MOCK + ": does not bind 127.0.0.1")
if not re.search(r'"@id":"http://127\.0\.0\.1:%d/', mt) and "http://127.0.0.1:%d/" not in mt: err(MOCK + ": the advertised publish URL is not loopback")
print("\n".join(errs))
sys.exit(1 if errs else 0)
PY
}

fail=0
if out="$(check "$ROOT")"; then echo "ok   the rehearsal is keyless (no environment, no secret, no OIDC, no write, no push beyond loopback)"; else echo "FAIL the rehearsal is not keyless"; echo "$out" | sed 's/^/     /'; fail=1; fi

# mutate <desc> <file> <python expression over s>; the check must fail.
mutate() {
  local d="$W/m"; rm -rf "$d"; mkdir -p "$d/.github/workflows" "$d/tools/ci"
  for f in $FILES $SCRIPT $MOCK; do cp "$ROOT/$f" "$d/$f"; done
  python3 -I - "$d/$2" "$3" <<'PY'
import sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read()
s2 = eval(expr, {"s": s})
open(p, "w").write(s2)
sys.exit(0 if s2 != s else 3)
PY
  [ $? = 0 ] || { echo "FAIL mutation did not change anything: $1"; fail=1; return; }
  if check "$d" >/dev/null; then echo "FAIL not caught: $1"; fail=1; else echo "ok   caught: $1"; fi
}
mutate 'environment: release'                     $P "s.replace('    runs-on: ubuntu-24.04', '    environment: release\n    runs-on: ubuntu-24.04')"
mutate 'a secrets reference'                      $P "s.replace('\${{ github.token }}', '\${{ secrets.GITHUB_TOKEN }}')"
mutate 'secrets: inherit in the caller'           $C "s.replace('    uses: ', '    secrets: inherit\n    uses: ')"
mutate 'id-token: write'                          $P "s.replace('  actions: read\n\njobs', '  actions: read\n  id-token: write\n\njobs')"
mutate 'contents: write'                          $P "s.replace('permissions:\n  contents: read', 'permissions:\n  contents: write', 1)"
mutate 'contents: \"write\" (quoted)'             $P "s.replace('permissions:\n  contents: read', 'permissions:\n  contents: \"write\"', 1)"
mutate 'the callee top-level permissions removed' $P "s.replace('permissions:\n  contents: read\n  actions: read\n\njobs', 'jobs')"
mutate 'the caller top-level permissions removed' $C "s.replace('permissions:\n  contents: read\n\nconcurrency', 'concurrency')"
mutate 'the caller job permissions removed'       $C "s.replace('    permissions:\n      contents: read\n      actions: read\n', '')"
mutate 'extra caller job permission (pull-requests: read)' $C "s.replace('      actions: read\n', '      actions: read\n      pull-requests: read\n')"
mutate 'ref: main removed from the checkout'      $P "s.replace('          ref: main\n', '')"
mutate 'persist-credentials removed'              $P "s.replace('          persist-credentials: false\n', '')"
mutate 'persist-credentials: true'                $P "s.replace('persist-credentials: false', 'persist-credentials: true')"
mutate 'a pull_request_target trigger'            $C "s.replace('on:\n  workflow_dispatch:', 'on:\n  pull_request_target:\n  workflow_dispatch:')"
mutate 'a tag-push trigger'                       $C "s.replace('on:\n  workflow_dispatch:', 'on:\n  push:\n  workflow_dispatch:')"
mutate 'a schedule trigger in the callee'         $P "s.replace('on:\n  workflow_call:', 'on:\n  schedule:\n  workflow_call:')"
mutate 'a job calling release.yml'                $C "s.replace('pipeline-nuget-rehearsal.yml@main', 'release.yml@main')"
mutate 'a call to the real lane'                  $C "s.replace('pipeline-nuget-rehearsal.yml@main', 'pipeline-release-nuget.yml@main')"
mutate 'a second job in the caller'               $C "s + '\n  second:\n    uses: We-Amp/pagespeed-optimizer/.github/workflows/pipeline-nuget-rehearsal.yml@main\n'"
mutate 'a persistent runner label'                $P "s.replace('runs-on: ubuntu-24.04', 'runs-on: fleet-heavy')"
mutate 'an unpinned action'                       $P "__import__('re').sub(r'(actions/setup-dotnet@)[0-9a-f]{40}', r'\1v6', s)"
mutate 'NuGet/login step'                         $P "s.replace('      - name: Ensure .NET 10 SDK', '      - uses: NuGet/login@v1\n      - name: Ensure .NET 10 SDK')"
mutate 'a nuget push in the workflow'             $P "s.replace('run: tools/ci/nupkg_checks.sh nuget-org nupkg', 'run: dotnet nuget push nupkg/*.nupkg')"
mutate 'feed_url pointing at nuget.org'           $SCRIPT "s.replace('feed_url=\"http://127.0.0.1:\${port}/v3/index.json\"', 'feed_url=\"https://api.nuget.org/v3/index.json\"')"
mutate 'feed_url assigned a second time'          $SCRIPT "s.replace('  # The client refuses plain HTTP', '  feed_url=\"http://127.0.0.1:1/x\"\n  # The client refuses plain HTTP')"
mutate 'feed_url from NUGET_SOURCE'               $SCRIPT "s.replace('feed_url=\"http://127.0.0.1:\${port}/v3/index.json\"', 'feed_url=\"\${NUGET_SOURCE:-http://127.0.0.1:\${port}/v3/index.json}\"')"
mutate 'a push with extra spacing to another source' $SCRIPT "s.replace('dotnet nuget push \"\$pkg\" --source \"\$feed_url\"', 'dotnet nuget   push \"\$pkg\" --source https://api.nuget.org/v3/index.json')"
mutate 'an api key in the script'                 $SCRIPT "s.replace('--skip-duplicate 2>&1', '--skip-duplicate --api-key x 2>&1')"
mutate 'curl -X PUT to nuget.org in the script'   $SCRIPT "s.replace('check_nuget_org() { # <dir>', 'check_nuget_org() { # <dir>\n  curl -X PUT -T \"\$1\" https://www.nuget.org/api/v2/package')"
mutate 'curl --data to a host in the workflow'    $P "s.replace('run: tools/ci/nupkg_checks.sh nuget-org nupkg', 'run: curl --data @nupkg/x https://example.invalid/')"
mutate 'the mock advertising nuget.org'           $MOCK "s.replace('url = \"http://127.0.0.1:%d/api/v2/package\" % self.server.server_port', 'url = \"https://www.nuget.org/api/v2/package\"')"
mutate 'the mock bound beyond loopback'           $MOCK "s.replace('(\"127.0.0.1\", 0)', '(\"0.0.0.0\", 0)')"

[ "$fail" = 0 ] && echo PASS || { echo FAILED; exit 1; }
