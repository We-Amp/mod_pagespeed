#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Stand up an ASan-instrumented Apache running the canonical system-test config,
# for the shutdown/graceful-restart memory-bug stress rig. Companion to the IIS and nginx rigs.
#
# Reuses the repo's own test/system/setup_apache_test.sh (the exact config the CI
# Apache System Tests use — CoreFilters, IPRO, small prefork MPM) for the module +
# vhost + cache, so this is literally "the system-test rig under ASan".
# ASan is delivered by LD_PRELOADing the clang runtime via /etc/apache2/envvars;
# because setup_apache_test.sh starts Apache through systemd (which ignores
# envvars), we mask that unit and start Apache DIRECTLY so the preload takes.
# Grounded in the proven shutdown-UAF recipe (tools/stress/cell_run.sh + the multi-site matrix).
set -uo pipefail

REPO_DIR="${REPO_DIR:-$(pwd)}"
MODULE_SO="${MODULE_SO:?set MODULE_SO to the freshly built ASan libmod_pagespeed.so}"
DOCROOT="${DOCROOT:-/var/www/html}"
PORT="${PORT:-18085}"
RIG_DIR="${RIG_DIR:-${TMPDIR:-/tmp}/apache-asan-rig}"
ERRLOG="${ERRLOG:-/var/log/apache2/error.log}"
ASAN_RUNTIME="${ASAN_RUNTIME:-}"
ENVV=/etc/apache2/envvars

if [[ -z "$ASAN_RUNTIME" ]]; then
  for c in \
    /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.asan-x86_64.so \
    /usr/lib/llvm-19/lib/clang/19/lib/linux/libclang_rt.asan-x86_64.so; do
    [[ -f "$c" ]] && ASAN_RUNTIME="$c" && break
  done
  if [[ -z "$ASAN_RUNTIME" ]] && command -v clang >/dev/null 2>&1; then
    cand="$(clang -print-file-name=libclang_rt.asan-x86_64.so 2>/dev/null || true)"
    [[ -f "$cand" ]] && ASAN_RUNTIME="$cand"
  fi
fi
[[ -f "$ASAN_RUNTIME" ]] || { echo "ERROR: clang ASan runtime not found (set ASAN_RUNTIME)"; exit 1; }
[[ -f "$MODULE_SO" ]]    || { echo "ERROR: ASan module not found at $MODULE_SO"; exit 1; }
SYMBOLIZER="$(dirname "$(dirname "$(dirname "$(dirname "$(dirname "$ASAN_RUNTIME")")")")")/bin/llvm-symbolizer"

mkdir -p "$RIG_DIR"

# 0. Mask the systemd apache2 unit for the duration of the rig. setup_apache_test.sh
#    starts Apache via `systemctl start apache2`, which (a) does NOT apply the
#    envvars LD_PRELOAD and (b) races/respawns against our direct ASan start on the
#    same port. Masking makes that systemctl start a harmless no-op (config still
#    completes) and guarantees the port is ours. cleanup unmasks.
sudo systemctl stop apache2 2>/dev/null || true
sudo systemctl mask apache2 2>/dev/null || true
sudo pkill -9 -x apache2 2>/dev/null || true

# 1. Inject ASan env into /etc/apache2/envvars (markered + backed up so cleanup
#    restores it verbatim). apache2ctl sources this on every start, so the module's
#    ASan instrumentation gets its runtime and options. detect_leaks=0: the server
#    has intentional at-exit leaks (log_message_handler, mod_instaweb pool) —
#    UAF/overflow detection stays ON; exitcode=66 makes a hit unmistakable.
echo "=== injecting ASan envvars into $ENVV ==="
sudo cp "$ENVV" "$RIG_DIR/envvars.bak"
sudo sed -i '/# ASAN-STRESS-BEGIN/,/# ASAN-STRESS-END/d' "$ENVV"
sudo tee -a "$ENVV" >/dev/null <<ENVEOF
# ASAN-STRESS-BEGIN
export LD_PRELOAD="$ASAN_RUNTIME"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:handle_abort=1:handle_segv=1:symbolize=1:exitcode=66"
$( [[ -x "$SYMBOLIZER" ]] && echo "export ASAN_SYMBOLIZER_PATH=\"$SYMBOLIZER\"" )
# ASAN-STRESS-END
ENVEOF

# 2a. Install the ASan module + canonical vhost + cache via setup_apache_test.sh.
#     Its final step starts Apache via systemctl, which does NOT apply the envvars
#     LD_PRELOAD (and an ASan-preloaded apache fails under systemd) — that failure
#     is EXPECTED and tolerated; the config (module/vhost/cache) is complete
#     by then.
echo "=== setup_apache_test.sh (config; systemd start fails under ASan — expected) ==="
( cd "$REPO_DIR" && MODULE_PATH="$MODULE_SO" \
    bash test/system/setup_apache_test.sh start ) 2>&1 | tail -20 || true

# 2a'. Defensive: own the vhost on $PORT serving $DOCROOT, regardless of any
#      non-default Listen / enabled-site state the runner carries (cell_run.sh
#      pattern). setup_apache_test.sh installs the pagespeed config globally
#      (mods-enabled), so it still applies to this vhost.
echo "=== installing dedicated rig vhost on :$PORT ==="
# Own ports.conf outright (backed up, restored by cleanup): the runner's copy may
# carry stray/duplicate Listen directives from prior manual work that make
# `apache2 -k start` fail to bind. A minimal single-Listen file is deterministic.
sudo cp /etc/apache2/ports.conf "$RIG_DIR/ports.conf.bak"
echo "Listen $PORT" | sudo tee /etc/apache2/ports.conf >/dev/null
for s in /etc/apache2/sites-enabled/*.conf; do [ -e "$s" ] && sudo a2dissite "$(basename "$s" .conf)" >/dev/null 2>&1; done
sudo tee /etc/apache2/sites-available/zz-apache-asan-rig.conf >/dev/null <<VH
<VirtualHost *:$PORT>
    ServerName localhost
    DocumentRoot $DOCROOT
    <Directory "$DOCROOT">
        Require all granted
        Options FollowSymLinks
    </Directory>
</VirtualHost>
VH
sudo a2ensite zz-apache-asan-rig >/dev/null 2>&1

# 2b. Start Apache DIRECTLY, bypassing systemd, so the envvars LD_PRELOAD actually
#     loads the ASan runtime into the process (the proven shutdown-UAF recipe).
#     setup_apache_test.sh may have started a systemd apache holding $PORT — tear it
#     down COMPLETELY (systemd + processes) and confirm the port is free (sudo ss —
#     an unprivileged ss misses sockets and races the bind) before starting.
echo "=== stop any (systemd) apache, free :$PORT, then direct ASan start ==="
sudo systemctl stop apache2 2>/dev/null || true
sudo apache2ctl stop 2>/dev/null || true
sudo pkill -TERM -x apache2 2>/dev/null || true
sleep 2
sudo pkill -9 -x apache2 2>/dev/null || true
for i in $(seq 1 60); do sudo ss -lntH "sport = :$PORT" 2>/dev/null | grep -q . || break; sleep 0.3; done
if sudo ss -lntH "sport = :$PORT" 2>/dev/null | grep -q .; then
  echo "ERROR: :$PORT still bound after teardown:"; sudo ss -lntp "sport = :$PORT"; exit 1
fi
sudo bash -c "set -a; source $ENVV; apache2 -k start"
sleep 2

# 3. Confirm the ASan runtime is actually mapped into the running Apache.
sleep 1
PID="$(pgrep -x apache2 | head -1 || true)"
if [[ -n "$PID" ]] && sudo grep -q libclang_rt.asan "/proc/$PID/maps" 2>/dev/null; then
  echo "ASAN_RUNTIME_LOADED=YES (pid $PID)"
else
  echo "ASAN_RUNTIME_LOADED=NO"; echo "--- error.log tail ---"; sudo tail -30 "$ERRLOG" 2>/dev/null || true
  exit 1
fi

# 4. Corpus into <docroot>/stress (gen as the current user, then place with sudo).
python3 "$REPO_DIR/tools/stress/stress_shutdown.py" gen-corpus --out "$RIG_DIR/corpus"
sudo mkdir -p "$DOCROOT/stress"
sudo cp "$RIG_DIR"/corpus/* "$DOCROOT/stress/"
sudo chown -R www-data:www-data "$DOCROOT/stress" 2>/dev/null || true

# 5. reload / restart helpers (source envvars so LD_PRELOAD re-applies on a cold start)
cat > "$RIG_DIR/apache-reload.sh" <<EOF
#!/bin/bash
sudo bash -c 'set -a; source $ENVV; apache2 -k graceful'
EOF
cat > "$RIG_DIR/apache-restart.sh" <<EOF
#!/bin/bash
sudo bash -c 'set -a; source $ENVV; apache2 -k graceful-stop' 2>/dev/null || true
for i in \$(seq 1 50); do sudo ss -lntH "sport = :$PORT" 2>/dev/null | grep -q . || break; sleep 0.2; done
sudo pkill -9 -x apache2 2>/dev/null || true
sleep 1
sudo bash -c 'set -a; source $ENVV; apache2 -k start'
for i in \$(seq 1 50); do sudo ss -lntH "sport = :$PORT" 2>/dev/null | grep -q . && break; sleep 0.2; done
EOF
chmod +x "$RIG_DIR/apache-reload.sh" "$RIG_DIR/apache-restart.sh"

CODE="$(curl -s -o /dev/null -w '%{http_code}' "http://localhost:$PORT/stress/page1.html" || true)"
echo "health /stress/page1.html: $CODE"
echo "SETUP DONE (rig helpers in $RIG_DIR, ASan runtime $ASAN_RUNTIME)"
