#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The headless Chrome the worker starts keeps WebRTC to proxied
# connections only.
#
#   smoke-chrome-webrtc.sh --chrome PATH [--binary PATH]
#
# 1  Start the worker with browser analysis on and a Chrome wrapper that
#    records the argv the worker gives Chrome, then runs the real Chrome.
# 2  Run the real Chrome with exactly those switches (the CDP pipe switch
#    and the start URL replaced by a test page) on a page that opens an
#    RTCPeerConnection with a STUN server and a TURN-over-TCP server on
#    loopback ports this script listens on.  No datagram and no connection
#    may arrive.
# 3  Control: the same run without the WebRTC switch must reach the STUN
#    listener, so a silent listener in 2 shows the switch at work, not a
#    probe that never ran.  If the control sees nothing the run fails as
#    INCONCLUSIVE.
#
# Linux only; needs python3 and curl.  Chrome runs without its sandbox here
# (--browser-sandbox off), which does not change its network behaviour.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BINARY="$REPO/bazel-bin/src/worker/factory_worker"
CHROME=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    --chrome) CHROME="$2"; shift 2 ;;
    *) echo "usage: smoke-chrome-webrtc.sh --chrome PATH [--binary PATH]" >&2; exit 2 ;;
  esac
done
[[ "$(uname -s)" == "Linux" ]] || { echo "skip: Linux-only smoke" >&2; exit 0; }
[[ -x "$BINARY" ]] || { echo "error: binary not found: $BINARY" >&2; exit 1; }
[[ -n "$CHROME" && -x "$CHROME" ]] || { echo "error: --chrome PATH required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 1; }

TMP="$(mktemp -d)"
WORKER_PID=""
cleanup() {
  if [[ -n "$WORKER_PID" ]] && kill -0 "$WORKER_PID" 2>/dev/null; then
    kill "$WORKER_PID" 2>/dev/null
    wait "$WORKER_PID" 2>/dev/null
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

# 1. The argv the worker gives Chrome.
ARGV_FILE="$TMP/chrome-argv"
cat >"$TMP/chrome-wrapper" <<EOF
#!/bin/bash
[[ -e "$ARGV_FILE" ]] || printf '%s\n' "\$@" >"$ARGV_FILE"
exec "$CHROME" "\$@"
EOF
chmod +x "$TMP/chrome-wrapper"
mkdir -p "$TMP/cache" "$TMP/run"
"$BINARY" --cache-dir "$TMP/cache" --socket "$TMP/run/n.sock" \
  --cache-size 16777216 --api-socket "$TMP/run/api.sock" \
  --enable-browser-analysis --browser-sandbox off \
  --chrome-binary "$TMP/chrome-wrapper" \
  --browser-user-data-dir "$TMP/run/chrome" >"$TMP/worker.log" 2>&1 &
WORKER_PID=$!
for _ in $(seq 1 300); do
  [[ -s "$ARGV_FILE" ]] && break
  sleep 0.2
done
kill "$WORKER_PID" 2>/dev/null
wait "$WORKER_PID" 2>/dev/null
WORKER_PID=""
if [[ ! -s "$ARGV_FILE" ]]; then
  echo "FAIL: the worker started no Chrome within 60 s" >&2
  tail -20 "$TMP/worker.log" >&2
  exit 1
fi
echo "captured $(wc -l <"$ARGV_FILE") switches from the worker"

# 2 and 3. Probe with the captured switches, then without the WebRTC one.
python3 - "$CHROME" "$ARGV_FILE" "$TMP" <<'PY'
import socket, subprocess, sys, threading, time, urllib.parse

chrome, argv_file, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
captured = [a for a in open(argv_file).read().splitlines()
            if a and a != "--remote-debugging-pipe" and a != "about:blank"
            and not a.startswith("--user-data-dir=")]
WEBRTC = "--webrtc-ip-handling-policy=disable_non_proxied_udp"


def probe(switches, label):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    tcp = socket.socket()
    tcp.bind(("127.0.0.1", 0))
    tcp.listen(5)
    up, tp = udp.getsockname()[1], tcp.getsockname()[1]
    hits = {"udp": 0, "tcp": 0}
    stop = time.time() + 12

    def on_udp():
        udp.settimeout(0.3)
        while time.time() < stop:
            try:
                udp.recvfrom(2048)
                hits["udp"] += 1
            except socket.timeout:
                pass

    def on_tcp():
        tcp.settimeout(0.3)
        while time.time() < stop:
            try:
                c, _ = tcp.accept()
                hits["tcp"] += 1
                c.close()
            except socket.timeout:
                pass

    threads = [threading.Thread(target=f) for f in (on_udp, on_tcp)]
    for t in threads:
        t.start()
    page = ("<script>try{const pc=new RTCPeerConnection({iceServers:["
            "{urls:'stun:127.0.0.1:%d'},"
            "{urls:'turn:127.0.0.1:%d?transport=tcp',username:'u',credential:'p'}"
            "]});pc.createDataChannel('x');"
            "pc.createOffer().then(o=>pc.setLocalDescription(o));}catch(e){}"
            "</script>" % (up, tp))
    url = "data:text/html," + urllib.parse.quote(page)
    p = subprocess.Popen([chrome, *switches, "--user-data-dir=%s/%s" % (tmp, label),
                          url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(10)
    p.kill()
    p.wait()
    for t in threads:
        t.join()
    udp.close()
    tcp.close()
    print("%s: %d STUN datagrams, %d TCP connections" %
          (label, hits["udp"], hits["tcp"]))
    return hits


fails = 0
if WEBRTC not in captured:
    print("FAIL: the worker does not pass %s to Chrome" % WEBRTC)
    fails += 1
shipped = probe(captured, "shipped-switches")
if shipped["udp"] or shipped["tcp"]:
    print("FAIL: WebRTC traffic reached loopback with the shipped switches")
    fails += 1
control = probe([a for a in captured if a != WEBRTC], "control-without-switch")
if not control["udp"]:
    print("FAIL: INCONCLUSIVE: the control run sent nothing, so the probe "
          "cannot tell blocked from broken")
    fails += 1
print("PASS" if fails == 0 else "FAILED (%d)" % fails)
sys.exit(1 if fails else 0)
PY
