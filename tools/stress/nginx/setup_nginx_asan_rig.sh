#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Stand up a self-contained nginx rig serving the ASan-instrumented
# ngx_pagespeed_module.so, for the shutdown/reload memory-bug stress rig
# (the nginx leg of the shutdown-race stress campaign).
#
# Mirrors the hand-built ~/nginx-stress rig the campaign started from. Unlike the IIS rig this is
# fully self-contained in RIG_DIR (its own prefix) — no shared-infra mutation, so
# no snapshot/restore dance. The nginx BINARY is stock (`--with-compat`, no
# sanitizer); ALL ASan coverage comes from the module + the LD_PRELOADed runtime
# (see run-nginx.sh below). That is exactly the model the run-side validation
# passed.
set -euo pipefail

RIG_DIR="${RIG_DIR:-$HOME/nginx-asan-rig}"
NGINX_BINARY="${NGINX_BINARY:-$HOME/nginx-src/objs/nginx}"
MODULE_SO="${MODULE_SO:?set MODULE_SO to the freshly built ASan ngx_pagespeed_module.so}"
PORT="${PORT:-18081}"
REPO_DIR="${REPO_DIR:-$(pwd)}"
# clang ASan runtime to LD_PRELOAD; auto-detect if unset.
ASAN_RUNTIME="${ASAN_RUNTIME:-}"

if [[ -z "$ASAN_RUNTIME" ]]; then
  for c in \
    /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.asan-x86_64.so \
    /usr/lib/llvm-19/lib/clang/19/lib/linux/libclang_rt.asan-x86_64.so; do
    [[ -f "$c" ]] && ASAN_RUNTIME="$c" && break
  done
  # Fallback: ask clang where its ASan runtime lives.
  if [[ -z "$ASAN_RUNTIME" ]] && command -v clang >/dev/null 2>&1; then
    cand="$(clang -print-file-name=libclang_rt.asan-x86_64.so 2>/dev/null || true)"
    [[ -f "$cand" ]] && ASAN_RUNTIME="$cand"
  fi
fi
[[ -f "$ASAN_RUNTIME" ]] || { echo "ERROR: clang ASan runtime not found (set ASAN_RUNTIME)"; exit 1; }
[[ -x "$NGINX_BINARY" ]] || { echo "ERROR: nginx binary not found/executable at $NGINX_BINARY"; exit 1; }
[[ -f "$MODULE_SO" ]]    || { echo "ERROR: ASan module not found at $MODULE_SO"; exit 1; }

echo "=== setting up nginx ASan rig at $RIG_DIR (port $PORT) ==="
rm -rf "$RIG_DIR"
mkdir -p "$RIG_DIR"/{sbin,modules,conf,html,cache,logs,cores,client_body_temp,proxy_temp,fastcgi_temp,uwsgi_temp,scgi_temp}
cp "$NGINX_BINARY" "$RIG_DIR/sbin/nginx"
cp "$MODULE_SO"    "$RIG_DIR/modules/ngx_pagespeed_module.so"

# mime.types drives Content-Type, which drives IPRO / rewriting. Copy it next to
# the conf and include it by absolute path (relative include is prefix-relative
# and brittle once -p points at an arbitrary RIG_DIR).
MIME_SRC=""
for c in "$(dirname "$NGINX_BINARY")/../conf/mime.types" "$HOME/nginx-src/conf/mime.types" /etc/nginx/mime.types; do
  [[ -f "$c" ]] && MIME_SRC="$c" && break
done
[[ -n "$MIME_SRC" ]] || { echo "ERROR: mime.types not found (looked next to nginx + /etc/nginx)"; exit 1; }
cp "$MIME_SRC" "$RIG_DIR/conf/mime.types"

# self-contained rewrite corpus into <docroot>/stress (8 pages / 4 css / 4 js / 6 png)
python3 "$REPO_DIR/tools/stress/stress_shutdown.py" gen-corpus --out "$RIG_DIR/html/stress"

# ASan runtime wrapper: LD_PRELOAD so the module's instrumentation resolves the
# runtime; detect_leaks=0 (server has intentional at-exit leaks — LSan stays on
# for unit tests); exitcode=66 so a hit is an unmistakable non-zero.
cat > "$RIG_DIR/run-nginx.sh" <<EOF
#!/bin/bash
export LD_PRELOAD="$ASAN_RUNTIME"
# detect_stack_use_after_return=1 (fake stack) so stack-lifetime bugs are
# caught too — a dead-buffer read this rig hunted stayed invisible without it.
export ASAN_OPTIONS="detect_leaks=0:detect_stack_use_after_return=1:abort_on_error=1:handle_abort=1:handle_segv=1:exitcode=66:log_path=$RIG_DIR/logs/asan"
ulimit -c unlimited 2>/dev/null || true
exec "$RIG_DIR/sbin/nginx" -p "$RIG_DIR" -c "$RIG_DIR/conf/nginx.conf" "\$@"
EOF
chmod +x "$RIG_DIR/run-nginx.sh"

cat > "$RIG_DIR/nginx-reload.sh" <<EOF
#!/bin/bash
exec "$RIG_DIR/run-nginx.sh" -s reload
EOF
chmod +x "$RIG_DIR/nginx-reload.sh"

# hard restart: graceful quit, wait for the port to release (force stop if it
# does not), then start and wait for it to come back up.
cat > "$RIG_DIR/nginx-restart.sh" <<EOF
#!/bin/bash
P="$RIG_DIR"
\$P/run-nginx.sh -s quit 2>/dev/null || true
for i in \$(seq 1 100); do ss -lnt 2>/dev/null | grep -q ':$PORT ' || break; sleep 0.2; done
if ss -lnt 2>/dev/null | grep -q ':$PORT '; then
  \$P/run-nginx.sh -s stop 2>/dev/null || true
  for i in \$(seq 1 50); do ss -lnt 2>/dev/null | grep -q ':$PORT ' || break; sleep 0.2; done
fi
sleep 1
\$P/run-nginx.sh
for i in \$(seq 1 100); do ss -lnt 2>/dev/null | grep -q ':$PORT ' && break; sleep 0.2; done
EOF
chmod +x "$RIG_DIR/nginx-restart.sh"

cat > "$RIG_DIR/conf/nginx.conf" <<EOF
load_module modules/ngx_pagespeed_module.so;
daemon on;
worker_processes 2;
error_log $RIG_DIR/logs/error.log info;
pid $RIG_DIR/logs/nginx.pid;

events { worker_connections 1024; }

http {
    include       $RIG_DIR/conf/mime.types;
    default_type  application/octet-stream;
    access_log    off;

    server {
        listen 127.0.0.1:$PORT;
        server_name localhost;
        root $RIG_DIR/html;

        pagespeed on;
        pagespeed FileCachePath $RIG_DIR/cache;
        pagespeed RewriteLevel CoreFilters;
        pagespeed InPlaceResourceOptimization on;

        location / { }
    }
}
EOF

echo "SETUP DONE (rig at $RIG_DIR, ASan runtime $ASAN_RUNTIME)"
