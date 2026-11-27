#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The management API refuses what it must refuse.
#
#   smoke-api-invariant.sh [--binary PATH]
#
# The invariant, asserted against a real daemon rather than against prose:
#
#   REMOTE IS NEVER UNAUTHENTICATED, AND UNAUTHENTICATED IS NEVER REMOTE.
#
# ...with one deliberate exception, ruled in the design (§13 Q3): on the unix
# socket the FILESYSTEM is the credential.  Reaching a 0660 `pagespeed`-group
# socket is the authentication, so no bearer token is asked for on top -- that
# removal is the whole reason the socket transport exists.
#
# Legs (design doc §7.2):
#   1  non-loopback bind, no --api-allow-remote      -> refuses, names the flag
#   2  non-loopback bind, allow-remote, NO token     -> refuses unconditionally
#   3  loopback TCP, no token, no --api-no-auth      -> refuses, names the flag
#   4  --api-socket + --api-port together            -> refuses (one transport)
#   5  a non-IPv4 --api-bind (::1, localhost)        -> refuses, names IPv4
#   6  a too-short token is treated as absent, loudly
#   7  loopback + token: no bearer 401 / wrong 403 / right 200
#   8  tokenless POST /v1/cache/purge over TCP -> 401, the cache volume is
#      still there, and the response carries NO purge report -- paired with an
#      authenticated purge that DOES report volume_reset, so the negative leg
#      has a positive control rather than asserting by absence.  (It does not
#      assert a seeded cache ENTRY survives: seeding one needs the notify
#      path, which is out of this script's scope -- the daemon-side purge
#      semantics are covered by cache_handlers_test.)
#   9  --api-read-open is no longer implied by an absent token
#  10  --api-socket: 0660, NO tcp listener at all, serves HTTP, upgrades a
#      WebSocket, and answers reads AND purge with no credential -- but still
#      demands the X-Requested-With CSRF header on mutating requests, because
#      a reverse-proxied console makes the socket browser-reachable and that
#      header is then the only control left on a credential-free path
#  11  /v1/health stays unauthenticated and leaks no credential or inventory
#
# Self-contained: a scratch directory, no systemd, no packaging, no web server.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BINARY="$REPO/bazel-bin/src/worker/factory_worker"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    *) echo "usage: smoke-api-invariant.sh [--binary PATH]" >&2; exit 2 ;;
  esac
done
[[ -x "$BINARY" ]] || { echo "error: binary not found: $BINARY" >&2; exit 1; }
[[ "$(uname -s)" == "Linux" || "$(uname -s)" == "Darwin" ]] || {
  echo "skip: POSIX-only smoke" >&2; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 1; }

fails=0
check() { # label expected actual
  if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
    echo "FAIL: $1 -- expected [$2], got [$3]$([[ "$3" == "000" ]] && echo ' (no response within 60 s)')" >&2
    fails=$((fails+1)); fi
}
mode_of() { stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"; }

# Bounded stop: SIGTERM, up to 10 s for exit, then SIGKILL and up to 5 s more.
# Never a bare `wait` -- a worker that ignores SIGTERM must not hold the job
# until its timeout.  Polls `kill -0`; reaps only once the pid is gone.
stop_pid() { # pid
  local pid="${1:-}" i
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  for ((i = 0; i < 100; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "note: pid $pid ignored SIGTERM for 10 s; sending SIGKILL" >&2
    kill -9 "$pid" 2>/dev/null || true
    for ((i = 0; i < 50; i++)); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
  fi
  kill -0 "$pid" 2>/dev/null || wait "$pid" 2>/dev/null || true
  return 0
}

TMP="$(mktemp -d)"
WORKER_PID=""
cleanup() {
  stop_pid "$WORKER_PID"
  WORKER_PID=""
  rm -rf "$TMP" 2>/dev/null
  return 0
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

CACHE_DIR="$TMP/cache/v2"
RUN_DIR="$TMP/run"
mkdir -p "$CACHE_DIR" "$RUN_DIR"

# A free TCP port for the legs that need one.
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

base_args=(--cache-dir "$CACHE_DIR" --cache-size 16777216 --log-level warning)

# ---------------------------------------------------------------------------
# Refusal matrix.  Each leg asserts BOTH a non-zero exit AND that the message
# names the one flag that would make the requested posture legal -- a refusal
# an operator cannot act on is only half a refusal.
# ---------------------------------------------------------------------------
run_refusal() { # logfile args...
  local log="$1"; shift
  "$BINARY" "${base_args[@]}" --socket "$RUN_DIR/r$RANDOM.sock" "$@" \
    >"$log" 2>&1
  echo $?
}

check "non-loopback bind without --api-allow-remote refuses" "1" \
  "$(run_refusal "$TMP/remote.log" --api-port "$PORT" --api-bind 0.0.0.0 \
      --api-token a-token-long-enough-to-pass)"
check "...and the refusal names --api-allow-remote" "1" \
  "$(grep -c -- '--api-allow-remote' "$TMP/remote.log")"

check "non-loopback bind without a token refuses even WITH --api-allow-remote" "1" \
  "$(run_refusal "$TMP/remote-notoken.log" --api-port "$PORT" --api-bind 0.0.0.0 \
      --api-allow-remote --api-no-auth)"
check "...and the refusal says the API is never remotely unauthenticated" "1" \
  "$(grep -c 'never allowed to be unauthenticated' "$TMP/remote-notoken.log")"

check "tokenless loopback without --api-no-auth refuses" "1" \
  "$(run_refusal "$TMP/noauth.log" --api-port "$PORT")"
check "...and the refusal names --api-no-auth" "1" \
  "$(grep -c -- '--api-no-auth' "$TMP/noauth.log")"

# The socket is NOT in that rule: its group scope is the credential, so a
# tokenless --api-socket start is legal and needs no opt-out flag.  Asserted
# for real further down, where it serves traffic without one.
check "--api-socket and --api-port together refuse (one transport)" "1" \
  "$(run_refusal "$TMP/two-transports.log" --api-socket "$RUN_DIR/two.sock" \
      --api-port "$PORT" --api-token some-token-that-is-long-enough)"
check "...and the refusal says the daemon serves exactly one" "1" \
  "$(grep -c 'exactly one' "$TMP/two-transports.log")"

# uv_ip4_addr takes an IPv4 literal only, so a hostname or IPv6 bind can never
# work; it is refused at parse rather than left to fail the bind later.
for bad_bind in '::1' '[::1]' localhost '::'; do
  check "--api-bind $bad_bind refuses (IPv4 literal required)" "1" \
    "$(run_refusal "$TMP/bind6.log" --api-port "$PORT" --api-bind "$bad_bind" \
        --api-token some-token-that-is-long-enough)"
  check "...and the refusal for $bad_bind names IPv4" "1" \
    "$(grep -c 'IPv4 literal' "$TMP/bind6.log")"
done

# A truncated or mis-pasted token is a typo, not a weak credential: it is
# treated as ABSENT (so the invariant refuses) and says why.
check "a too-short token is treated as absent and refuses" "1" \
  "$(run_refusal "$TMP/shorttok.log" --api-port "$PORT" --api-token short)"
check "...and names the length problem" "1" \
  "$(grep -c 'characters; at least' "$TMP/shorttok.log")"

# The whole 127.0.0.0/8 block is loopback, not just 127.0.0.1 -- assert by
# STARTING there and getting an answer, not by the daemon merely not exiting.
"$BINARY" "${base_args[@]}" --socket "$RUN_DIR/lo8.sock" \
  --api-port "$PORT" --api-bind 127.0.0.7 --api-token lo8-token-long-enough \
  >"$TMP/lo8.log" 2>&1 &
LO8_PID=$!
for _ in $(seq 1 600); do
  curl -s --max-time 60 -o /dev/null "http://127.0.0.7:$PORT/v1/health" && break
  sleep 0.2
done
check "a 127.0.0.0/8 address other than 127.0.0.1 counts as loopback" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' "http://127.0.0.7:$PORT/v1/health")"
check "...and it did not demand --api-allow-remote" "0" \
  "$(grep -c -- '--api-allow-remote' "$TMP/lo8.log" || true)"
stop_pid "$LO8_PID"

# ---------------------------------------------------------------------------
# Live daemon: loopback + token.
# ---------------------------------------------------------------------------
# Deterministically >= the daemon's 16-character minimum. "smoke-token-$RANDOM"
# was 13-17 chars depending on the draw, so a short $RANDOM made the daemon
# (correctly) treat it as absent and refuse to start -- a test that failed on
# a dice roll rather than on the behaviour under test.
TOKEN="smoke-token-0123456789-$RANDOM"
"$BINARY" "${base_args[@]}" --socket "$RUN_DIR/notify.sock" \
  --api-port "$PORT" --api-bind 127.0.0.1 --api-token "$TOKEN" \
  >"$TMP/live.log" 2>&1 &
WORKER_PID=$!
for _ in $(seq 1 600); do
  curl -s --max-time 60 -o /dev/null "http://127.0.0.1:$PORT/v1/health" && break
  sleep 0.2
done
# If the daemon is not up, every leg below reports 000 and the reader is left
# guessing which of a dozen assertions is the real failure.  Say it once, with
# the daemon's own reason attached.
if ! curl -s --max-time 60 -o /dev/null "http://127.0.0.1:$PORT/v1/health"; then
  echo "FAIL: the loopback+token daemon never came up; its log said:" >&2
  sed -n '1,10p' "$TMP/live.log" >&2
  exit 1
fi

code() { # url [headers...]
  curl -s --max-time 60 -o /dev/null -w '%{http_code}' "$@"
}

check "no bearer on a read endpoint => 401" "401" \
  "$(code "http://127.0.0.1:$PORT/v1/cache/urls")"
check "wrong bearer on a read endpoint => 403" "403" \
  "$(code -H "Authorization: Bearer wrong" "http://127.0.0.1:$PORT/v1/cache/urls")"
check "correct bearer on a read endpoint => 200" "200" \
  "$(code -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/v1/cache/urls")"

# The load-bearing leg: a tokenless purge must not be ANSWERED, and must not
# have RUN.  A full purge deletes and recreates the cache volume, so the
# volume's inode is the state that says whether it happened -- a status code
# alone would still pass if the handler ran and then failed to reply.
volume_count() {
  find "$CACHE_DIR" -maxdepth 1 -name 'cache-*' -type f | wc -l | tr -d ' '
}
body() { curl -s --max-time 60 "$@"; }
check "the cache volume exists before the purge legs" "1" "$(volume_count)"

check "tokenless POST /v1/cache/purge => 401" "401" \
  "$(code -X POST -H 'Content-Type: application/json' \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      "http://127.0.0.1:$PORT/v1/cache/purge")"
check "wrong-bearer POST /v1/cache/purge => 403" "403" \
  "$(code -X POST -H 'Content-Type: application/json' \
      -H 'Authorization: Bearer wrong' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      "http://127.0.0.1:$PORT/v1/cache/purge")"
check "...and the cache volume is still there afterwards" "1" \
  "$(volume_count)"
# The handler reports what it did; an unauthenticated attempt must produce no
# such report at all.
check "...and the refused attempt reported no purge" "0" \
  "$(body -X POST -H 'Content-Type: application/json' \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      "http://127.0.0.1:$PORT/v1/cache/purge" | grep -c 'volume_reset' || true)"

# Positive control: the same request WITH the token runs and SAYS it ran.
# Without this pair, "no purge report" is equally consistent with "this
# assertion cannot observe a purge at all".
check "an authenticated purge => 200" "200" \
  "$(code -X POST -H 'Content-Type: application/json' \
      -H "Authorization: Bearer $TOKEN" \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      "http://127.0.0.1:$PORT/v1/cache/purge")"
check "...and THAT one reports volume_reset (the check has teeth)" "1" \
  "$(body -X POST -H 'Content-Type: application/json' \
      -H "Authorization: Bearer $TOKEN" \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      "http://127.0.0.1:$PORT/v1/cache/purge" | grep -c 'volume_reset')"

# read_open is a CHOICE now, not a side effect of a missing credential.
check "a token without --api-read-open still gates GET" "401" \
  "$(code "http://127.0.0.1:$PORT/v1/stats")"

# /v1/health stays open for monitoring -- and must carry nothing that an
# unauthenticated caller has no business seeing.
HEALTH="$(curl -s --max-time 60 "http://127.0.0.1:$PORT/v1/health")"
check "/v1/health is unauthenticated" "200" \
  "$(code "http://127.0.0.1:$PORT/v1/health")"
check "/v1/health does not leak the token" "0" \
  "$(printf '%s' "$HEALTH" | grep -c "$TOKEN")"
check "/v1/health does not carry a cached-URL inventory" "0" \
  "$(printf '%s' "$HEALTH" | grep -c '"urls"')"
check "/v1/health reports the browser sandbox state" "1" \
  "$(printf '%s' "$HEALTH" | grep -c '"browser_sandbox"')"
# The read-only syscall-filter answer is part of the health
# contract too, so monitoring can see an unfiltered daemon without a unit file.
check "/v1/health reports the syscall filter state" "1" \
  "$(printf '%s' "$HEALTH" | grep -c '"syscall_filter"')"

# The token must not be reachable through /proc for any local user.
if [[ -r "/proc/$WORKER_PID/cmdline" ]]; then
  check "the token is on argv only because THIS test put it there" "1" \
    "$(tr '\0' '\n' < "/proc/$WORKER_PID/cmdline" | grep -c -- '--api-token')"
fi

stop_pid "$WORKER_PID"; WORKER_PID=""

# ---------------------------------------------------------------------------
# The unix-socket transport: the default local path.
# ---------------------------------------------------------------------------
# NOTE: no --api-no-auth and no token. That is the point: the socket's 0660
# group scope is the credential, so this start must be legal on its own.
API_SOCK="$RUN_DIR/api.sock"
PURGE_TOKEN="purge-token-0123456789-$RANDOM"
PAGESPEED_PURGE_TOKEN="$PURGE_TOKEN" \
"$BINARY" "${base_args[@]}" --socket "$RUN_DIR/notify2.sock" \
  --api-socket "$API_SOCK" \
  >"$TMP/uds.log" 2>&1 &
WORKER_PID=$!
for _ in $(seq 1 600); do
  [[ -S "$API_SOCK" ]] && break
  sleep 0.2
done
if [[ ! -S "$API_SOCK" ]]; then
  echo "FAIL: the unix-socket daemon never came up; its log said:" >&2
  sed -n '1,10p' "$TMP/uds.log" >&2
  exit 1
fi

check "the API socket exists" "1" "$([[ -S "$API_SOCK" ]] && echo 1 || echo 0)"
check "the API socket is 0660" "660" "$(mode_of "$API_SOCK")"
check "no world-writable bit under the runtime dir" "0" \
  "$(find "$RUN_DIR" -perm -o+w | wc -l | tr -d ' ')"
check "HTTP over the unix socket answers" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      http://localhost/v1/health)"
# `ss` absent would make `grep -c` return 0 and this leg pass without ever
# looking -- the exact "assert by absence" shape this suite exists to avoid.
# Require it where it exists (Linux) and skip loudly where it does not.
if [[ "$(uname -s)" == "Linux" ]]; then
  if ! command -v ss >/dev/null 2>&1; then
    echo "FAIL: ss is required to prove no TCP port is bound" >&2
    fails=$((fails+1))
  else
    check "the daemon binds NO tcp port in unix-socket mode" "0" \
      "$(ss -ltnp 2>/dev/null | grep -c "pid=$WORKER_PID," || true)"
  fi
else
  echo "skip: no-TCP-listener leg needs ss (Linux only)"
fi
# The socket needs no opt-out, so it must NOT emit the tokenless-TCP banner:
# that warning is about an unauthenticated network listener, and printing it
# here would train operators to ignore it where it matters.
check "the socket does not warn about a missing token" "0" \
  "$(grep -c 'WITHOUT a token' "$TMP/uds.log")"

# §13 Q3, asserted: reads and PURGE both answer with NO credential at all.
check "a read over the unix socket needs no bearer" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      http://localhost/v1/stats)"
check "a header-less mutating request over the socket is refused" "400" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      -X POST -H 'Content-Type: application/json' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      http://localhost/v1/cache/purge)"
check "PURGE over the unix socket needs no bearer" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      -X POST -H 'Content-Type: application/json' \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      http://localhost/v1/cache/purge)"

# The WebSocket upgrade over the unix transport (the console's path under
# WS3).  DetachConnection has to reopen the duplicated fd as a PIPE handle
# here, not a TCP one -- a mistake that a TCP-only test suite cannot see, and
# that would present as "the console connects but never updates".
ws_check() {
  python3 - "$API_SOCK" <<'PYWS'
import socket, sys, time
# One 8 s deadline for the whole exchange: the handshake AND the first frame.
deadline = time.monotonic() + 8
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
buf = b""

class Eof(Exception):
    pass

def need(n):
    # Read until buf holds at least n bytes; Eof if the peer closes first.
    global buf
    while len(buf) < n:
        left = deadline - time.monotonic()
        if left <= 0:
            raise socket.timeout()
        s.settimeout(left)
        chunk = s.recv(65536)
        if not chunk:
            raise Eof()
        buf += chunk

try:
    s.settimeout(8)
    s.connect(sys.argv[1])
    s.sendall(b"GET /v1/ws/stats HTTP/1.1\r\nHost: localhost\r\n"
              b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
              b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              b"Sec-WebSocket-Version: 13\r\n\r\n")
    # The daemon writes the 101 and the stats snapshot back to back, so one
    # recv can return the 101 alone, the 101 plus part of the snapshot frame,
    # or the 101 plus all of it.  Read to the end of the headers, keep what
    # follows as frame bytes, and parse frames off the byte stream rather
    # than assuming one recv per frame.
    try:
        while b"\r\n\r\n" not in buf:
            need(len(buf) + 1)
    except Eof:
        print("no-101"); sys.exit(0)
    head, _, buf = buf.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/1.1 101") \
            or b"sec-websocket-accept" not in head.lower():
        print("no-101"); sys.exit(0)
    # A stream that hands back a 101 but never frames is not a working
    # stream.  Skip ping/pong until the first text or binary frame has fully
    # arrived; a close frame, any other opcode, or EOF first is a failure.
    while True:
        need(2)
        opcode, masked, plen = buf[0] & 0x0F, buf[1] & 0x80, buf[1] & 0x7F
        ext = {126: 2, 127: 8}.get(plen, 0)
        need(2 + ext)
        if ext:
            plen = int.from_bytes(buf[2:2 + ext], "big")
        hdr = 2 + ext + (4 if masked else 0)
        need(hdr + plen)
        if opcode in (1, 2):
            print("ok"); break
        if opcode == 8:
            print("no-frame:close"); break
        if opcode not in (9, 10):
            print("no-frame:opcode-%d" % opcode); break
        buf = buf[hdr + plen:]
except Eof:
    print("no-frame:eof")
except Exception as exc:  # noqa: BLE001
    print("error:%s" % type(exc).__name__)
PYWS
}
check "the WebSocket stream upgrades and frames over the unix socket" "ok" \
  "$(ws_check)"

stop_pid "$WORKER_PID"; WORKER_PID=""

# A token configured ALONGSIDE the socket must not resurrect the bearer
# requirement.  Every packaged install has one in daemon.env for the TCP case,
# and the recommended local transport is the socket -- if that combination
# started demanding a header, the documented one-line enablement would break.
"$BINARY" "${base_args[@]}" --socket "$RUN_DIR/notify3.sock" \
  --api-socket "$API_SOCK" --api-token "$TOKEN" \
  >"$TMP/uds2.log" 2>&1 &
WORKER_PID=$!
for _ in $(seq 1 600); do
  [[ -S "$API_SOCK" ]] && break
  sleep 0.2
done
check "a read over the socket still needs no bearer with a token set" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      http://localhost/v1/stats)"
check "purge over the socket still needs no bearer with a token set" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      -X POST -H 'Content-Type: application/json' \
      -H 'X-Requested-With: XMLHttpRequest' \
      --data '{"scope":"all","confirm":"purge-all"}' \
      http://localhost/v1/cache/purge)"
check "a bearer is still ACCEPTED over the socket (not rejected)" "200" \
  "$(curl -s --max-time 60 -o /dev/null -w '%{http_code}' --unix-socket "$API_SOCK" \
      -X POST -H 'Content-Type: application/json' \
      -H 'X-Requested-With: XMLHttpRequest' \
      -H "Authorization: Bearer $TOKEN" \
      --data '{"scope":"all","confirm":"purge-all"}' \
      http://localhost/v1/cache/purge)"

stop_pid "$WORKER_PID"; WORKER_PID=""

if [[ "$fails" -gt 0 ]]; then
  echo "smoke: $fails FAILURE(S)" >&2; exit 1
fi
echo "smoke: all checks passed"
exit 0
