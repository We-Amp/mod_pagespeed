#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/fetch_retry.sh. No external network: a local python
# http.server (started and stopped by its own pid) serves the files, and some
# cases use a file:// URL.
#
# Run: tools/ci/test_fetch_retry.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="$here/fetch_retry.sh"
work="$(mktemp -d)"
srv_pid=""
cleanup() {
  if [ -n "$srv_pid" ]; then kill "$srv_pid" 2>/dev/null || true; wait "$srv_pid" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | awk '{print $1}'; }

mkdir -p "$work/www"
printf 'payload-one\n' > "$work/www/a.txt"
good="$(sha "$work/www/a.txt")"
export FETCH_RETRY_COUNT=2 FETCH_RETRY_MAX_TIME=20 FETCH_CONNECT_TIMEOUT=2

# 1. file:// URL with the right checksum.
bash "$script" "file://$work/www/a.txt" "$work/out/f1.txt" "$good" || fail "file:// with good sha"
cmp -s "$work/www/a.txt" "$work/out/f1.txt" || fail "file:// content differs"

# 2. wrong checksum is a hard failure (exit 3) and leaves nothing behind.
rc=0; out="$(bash "$script" "file://$work/www/a.txt" "$work/out/f2.txt" "$(printf 0%.0s $(seq 64))" 2>&1)" || rc=$?
[ "$rc" -eq 3 ] || fail "mismatch must exit 3, got $rc: $out"
echo "$out" | grep -q "sha256 mismatch" || fail "missing mismatch message: $out"
[ ! -e "$work/out/f2.txt" ] || fail "file left behind after mismatch"
[ "$(ls "$work/out")" = "f1.txt" ] || fail "temp file left behind: $(ls "$work/out")"

# 3. local HTTP server: success without a checksum.
port="$(python3 -I -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$work/www" >/dev/null 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do
  curl -fsS -o /dev/null "http://127.0.0.1:$port/a.txt" 2>/dev/null && break
  sleep 0.1
done
bash "$script" "http://127.0.0.1:$port/a.txt" "$work/out/f3.txt" || fail "http fetch"
cmp -s "$work/www/a.txt" "$work/out/f3.txt" || fail "http content differs"

# 4. a 404 fails (not swallowed) and leaves nothing.
rc=0; bash "$script" "http://127.0.0.1:$port/missing.txt" "$work/out/f4.txt" >/dev/null 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "404 must fail"
[ ! -e "$work/out/f4.txt" ] || fail "file left behind after 404"

# 5. the server comes up late: retries bridge the outage.
kill "$srv_pid"; wait "$srv_pid" 2>/dev/null || true; srv_pid=""
( sleep 3; exec python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$work/www" >/dev/null 2>&1 ) &
srv_pid=$!
FETCH_RETRY_COUNT=8 FETCH_RETRY_MAX_TIME=30 bash "$script" "http://127.0.0.1:$port/a.txt" "$work/out/f5.txt" "$good" \
  || fail "retry across a short outage"
cmp -s "$work/www/a.txt" "$work/out/f5.txt" || fail "late-server content differs"

# 6. a stalled transfer is cut off and the whole call stays inside
#    FETCH_RETRY_MAX_TIME + FETCH_ATTEMPT_MAX_TIME (here 4 + 3 s).
kill "$srv_pid" 2>/dev/null || true; wait "$srv_pid" 2>/dev/null || true; srv_pid=""
cat > "$work/stall.py" <<'EOF'
import socket, sys, threading, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8)
def serve(c):
    c.recv(4096)
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\nx")
    time.sleep(120)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
EOF
python3 -I "$work/stall.py" "$port" >/dev/null 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do
  python3 -I -c 'import socket,sys;socket.create_connection(("127.0.0.1",int(sys.argv[1])),1)' "$port" 2>/dev/null && break
  sleep 0.1
done
start=$SECONDS rc=0
FETCH_RETRY_COUNT=8 FETCH_RETRY_MAX_TIME=4 FETCH_ATTEMPT_MAX_TIME=3 FETCH_STALL_TIME=2 \
  bash "$script" "http://127.0.0.1:$port/a.txt" "$work/out/f6.txt" >/dev/null 2>&1 || rc=$?
took=$((SECONDS - start))
[ "$rc" -ne 0 ] || fail "stalled transfer must fail"
[ "$took" -le 9 ] || fail "stalled transfer took ${took}s, bound is 7s"
[ ! -e "$work/out/f6.txt" ] || fail "file left behind after stall"

# 7. the default curl flags carry the stall limit and the per-attempt cap.
mkdir -p "$work/shim"
printf '#!/bin/sh\necho "$@" > "%s"\nexit 22\n' "$work/args" > "$work/shim/curl"
chmod +x "$work/shim/curl"
env -u FETCH_RETRY_COUNT -u FETCH_RETRY_MAX_TIME -u FETCH_ATTEMPT_MAX_TIME \
    -u FETCH_STALL_TIME -u FETCH_CONNECT_TIMEOUT PATH="$work/shim:$PATH" \
  bash "$script" "http://127.0.0.1:1/x" "$work/out/f7.txt" >/dev/null 2>&1 || true
args="$(cat "$work/args")"
for want in "--retry 8" "--retry-max-time 90" "--max-time 60" "--speed-limit 1024" \
            "--speed-time 60" "--connect-timeout 20"; do
  case " $args " in *" $want "*) ;; *) fail "default curl flags lack '$want': $args" ;; esac
done

echo "PASS: fetch_retry"
