#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Filter order of the nginx module built from pagespeed/nginx/config.
#
# The packaged module is built by nginx's own configure from
# pagespeed/nginx/config, not by Bazel. This test runs that configure for a
# static (--add-module) and a dynamic (--add-dynamic-module) build and checks
# that both place the module's two filters exactly where the Bazel-built
# module's ngx_module_order (pagespeed/nginx/ngx_pagespeed_module.cc) places
# them:
#   - the dynamic build carries the same module names and the same order
#     array as ngx_pagespeed_module.cc;
#   - the static module list equals the list nginx's loader produces when it
#     loads that dynamic module into the same nginx;
#   - in that list the rewriting filter sits between compression and the
#     postpone filter (so it runs after SSI, sub, addition and the headers
#     filter), and the etag filter between range_header and compression.
#
# Configure only: nothing is compiled, the PSOL archive is a placeholder.
#
# Usage: tools/ci/test_nginx_config_filter_order.sh [nginx-source-dir]
#   Without an argument it downloads nginx $NGINX_VERSION (default 1.24.0) and
#   verifies it against the pinned sha256 (or $NGINX_SHA256).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODULE_DIR="$REPO_ROOT/pagespeed/nginx"
BAZEL_ORDER_SRC="$MODULE_DIR/ngx_pagespeed_module.cc"

NGINX_VERSION="${NGINX_VERSION:-1.24.0}"
if [ -z "${NGINX_SHA256:-}" ]; then
  case "$NGINX_VERSION" in
    1.24.0) NGINX_SHA256=77a2541637b92a621e3ee76776c8b7b40cf6d707e69ba53a940283e30ff2f55d ;;
    *) echo "FAIL: set NGINX_SHA256 for nginx $NGINX_VERSION" >&2; exit 1 ;;
  esac
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ $# -ge 1 ]; then
  SRC="$(cd "$1" && pwd)"
else
  tarball="$WORK/nginx-$NGINX_VERSION.tar.gz"
  curl -fsSL --retry 3 -o "$tarball" "https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz"
  echo "$NGINX_SHA256  $tarball" | sha256sum -c --quiet
  tar xzf "$tarball" -C "$WORK"
  SRC="$WORK/nginx-$NGINX_VERSION"
fi
[ -x "$SRC/configure" ] || { echo "FAIL: no nginx source at $SRC" >&2; exit 1; }

# The config only checks that the archive exists. A failing bazel shim keeps
# it from querying a workspace.
touch "$WORK/placeholder.a"
mkdir -p "$WORK/bin"
printf '#!/bin/sh\nexit 1\n' > "$WORK/bin/bazel"
chmod +x "$WORK/bin/bazel"

# Filters that the order names and that need no extra library. The rewrite
# module is left out only because it needs PCRE.
CONFIGURE_ARGS=(
  --with-compat
  --without-http_rewrite_module
  --with-http_sub_module
  --with-http_addition_module
  --with-http_gunzip_module
)

run_configure() {  # <name> <module option>
  local dir="$WORK/$1"
  cp -a "$SRC" "$dir"
  if ! (cd "$dir" && PATH="$WORK/bin:$PATH" \
        MOD_PAGESPEED_DIR="$REPO_ROOT" \
        PAGESPEED_ARCHIVE="$WORK/placeholder.a" \
        PAGESPEED_PSOL_ARCHIVE="$WORK/placeholder.a" \
        ./configure "${CONFIGURE_ARGS[@]}" "$2=$MODULE_DIR") \
        > "$WORK/$1.log" 2>&1; then
    tail -30 "$WORK/$1.log" >&2
    echo "FAIL: nginx configure ($1)" >&2
    exit 1
  fi
}

run_configure static --add-module
run_configure dynamic --add-dynamic-module

python3 - "$BAZEL_ORDER_SRC" "$WORK/static/objs/ngx_modules.c" \
  "$WORK/dynamic/objs/ngx_modules.c" \
  "$WORK/dynamic/objs/ngx_pagespeed_modules.c" <<'PY'
import re
import sys

bazel_src, static_c, base_c, dynamic_c = sys.argv[1:]


def array(path, name):
    text = open(path).read()
    m = re.search(r"\b" + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit(f"FAIL: no {name}[] in {path}")
    return re.findall(r'"([^"]+)"', m.group(1))


def load(base, modules, order):
    """nginx's ngx_add_module, applied to each module of one shared object."""
    cycle = list(base)
    for module in modules:
        later = order[order.index(module) + 1:] if module in order else []
        before = len(cycle)
        for name in later:
            for m in range(before):
                if cycle[m] == name:
                    before = m
                    break
        cycle.insert(before, module)
    return cycle


def filters(modules):
    """The filter modules, in list order (they run in reverse)."""
    return [m for m in modules if "filter" in m or m == "ngx_pagespeed"]


failures = []
bazel_names = array(bazel_src, "ngx_module_names")
bazel_order = array(bazel_src, "ngx_module_order")

dyn_names = array(dynamic_c, "ngx_module_names")
dyn_order = array(dynamic_c, "ngx_module_order")
if dyn_names != bazel_names:
    failures.append(f"dynamic module names {dyn_names} != Bazel {bazel_names}")
if dyn_order != bazel_order:
    failures.append(f"dynamic order {dyn_order} != Bazel {bazel_order}")

base = array(base_c, "ngx_module_names")
expected = load(base, bazel_names, bazel_order)
loaded = load(base, dyn_names, dyn_order)
static = array(static_c, "ngx_module_names")

print("static build, filter module list (filters run bottom to top):")
for m in filters(static):
    print("  " + m)
print("dynamic build, filter module list after load_module:")
for m in filters(loaded):
    print("  " + m)

if static != expected:
    failures.append(
        "static module list differs from the Bazel module's order:\n"
        f"  static:   {filters(static)}\n  expected: {filters(expected)}")
if loaded != expected:
    failures.append(
        "dynamic module list differs from the Bazel module's order:\n"
        f"  dynamic:  {filters(loaded)}\n  expected: {filters(expected)}")


def check_chain(name, modules):
    idx = {m: i for i, m in enumerate(modules)}
    chain = ["ngx_http_range_header_filter_module", "ngx_pagespeed_etag_filter",
             "ngx_http_gzip_filter_module", "ngx_pagespeed",
             "ngx_http_postpone_filter_module", "ngx_http_ssi_filter_module",
             "ngx_http_charset_filter_module", "ngx_http_sub_filter_module",
             "ngx_http_addition_filter_module",
             "ngx_http_headers_filter_module"]
    missing = [m for m in chain if m not in idx]
    if missing:
        failures.append(f"{name}: modules missing from the list: {missing}")
        return
    positions = [idx[m] for m in chain]
    if positions != sorted(positions):
        failures.append(f"{name}: filters out of order: {chain} at {positions}")
    # Adjacent: nothing between the etag filter and compression, or between
    # compression and the rewriting filter.
    for a, b in (("ngx_http_range_header_filter_module", "ngx_pagespeed_etag_filter"),
                 ("ngx_pagespeed_etag_filter", "ngx_http_gzip_filter_module"),
                 ("ngx_http_gzip_filter_module", "ngx_pagespeed"),
                 ("ngx_pagespeed", "ngx_http_postpone_filter_module")):
        if idx[b] != idx[a] + 1:
            failures.append(f"{name}: {b} does not follow {a} directly")


check_chain("static", static)
check_chain("dynamic", loaded)

if failures:
    for f in failures:
        print("FAIL: " + f)
    sys.exit(1)
print("PASS: static and dynamic builds from config match the Bazel module's filter order")
PY
