#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The headless Chrome the worker starts reaches no address-literal listener
# through WebSocket, WebTransport, a popup, a shared worker or a
# cross-site (out-of-process) iframe.
#
#   smoke-chrome-egress.sh --chrome PATH [--binary PATH]
#
# 1  Start the worker with browser analysis on and a Chrome wrapper that
#    records the argv the worker gives Chrome.
# 2  Serve a test page on 127.0.0.2 and run the real Chrome with exactly
#    those switches, except that the page's own host is excluded from the
#    host resolver rule so the page can load (the CDP pipe switch and the
#    start URL are replaced by the page; --disable-popup-blocking is added so
#    the popup probe is not stopped by the popup blocker instead).  The page
#    opens each kind of connection to listeners on 127.0.0.1.  None may
#    arrive.
# 3  Control: the same run with 127.0.0.1 excluded as well must see every
#    probe arrive, so a silent listener in 2 shows the switches at work, not
#    a probe that never ran.  A probe the control does not see fails the run
#    as INCONCLUSIVE.
#
# The page analysis removes the service worker API from rendered pages; that
# is defence in depth on top of these switches and is not exercised here.
#
# Linux only; needs python3.  Chrome runs without its sandbox here
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
    *) echo "usage: smoke-chrome-egress.sh --chrome PATH [--binary PATH]" >&2; exit 2 ;;
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

python3 - "$CHROME" "$ARGV_FILE" "$TMP" <<'PY'
import http.server, socket, socketserver, subprocess, sys, threading, time

chrome, argv_file, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
captured = [a for a in open(argv_file).read().splitlines()
            if a and a != "--remote-debugging-pipe" and a != "about:blank"
            and not a.startswith("--user-data-dir=")]
RULE = "--host-resolver-rules=MAP * ~NOTFOUND"
if RULE not in captured:
    print("FAIL: the worker does not pass %s to Chrome" % RULE)
    sys.exit(1)
PAGE_HOST, TARGET = "127.0.0.2", "127.0.0.1"
PROBES = ["websocket", "webtransport", "popup", "sharedworker", "iframe"]


def run(label, rule):
    hits = {p: 0 for p in PROBES}
    # Target listeners on 127.0.0.1: TCP (HTTP / WebSocket upgrade) and UDP
    # (WebTransport over QUIC).
    tcp = socket.socket()
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind((TARGET, 0))
    tcp.listen(16)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((TARGET, 0))
    tp, up = tcp.getsockname()[1], udp.getsockname()[1]
    stop = time.time() + 14

    def on_tcp():
        tcp.settimeout(0.3)
        while time.time() < stop:
            try:
                c, _ = tcp.accept()
            except socket.timeout:
                continue
            c.settimeout(1)
            try:
                line = c.recv(2048).decode("latin-1", "replace").split("\r\n", 1)[0]
            except OSError:
                line = ""
            for p in PROBES:
                if "/" + p in line:
                    hits[p] += 1
            c.close()

    def on_udp():
        udp.settimeout(0.3)
        while time.time() < stop:
            try:
                udp.recvfrom(4096)
                hits["webtransport"] += 1
            except socket.timeout:
                pass

    t = "http://%s:%d" % (TARGET, tp)
    page = ("<!doctype html><title>p</title><script>"
            "try{new WebSocket('ws://%s:%d/websocket');}catch(e){}"
            "try{new WebTransport('https://%s:%d/webtransport');}catch(e){}"
            "try{window.open('%s/popup');}catch(e){}"
            "try{new SharedWorker('/sw.js');}catch(e){}"
            "</script><iframe src='%s/iframe'></iframe>"
            % (TARGET, tp, TARGET, up, t, t))
    worker_js = "fetch('%s/sharedworker').catch(()=>{});" % t

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = worker_js if self.path == "/sw.js" else page
            self.send_response(200)
            self.send_header("Content-Type", "application/javascript"
                             if self.path == "/sw.js" else "text/html")
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, *a):
            pass

    srv = socketserver.ThreadingTCPServer((PAGE_HOST, 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threads = [threading.Thread(target=f) for f in (on_tcp, on_udp)]
    for th in threads:
        th.start()
    switches = [rule if a == RULE else a for a in captured]
    switches.append("--disable-popup-blocking")
    p = subprocess.Popen([chrome, *switches, "--user-data-dir=%s/%s" % (tmp, label),
                          "http://%s:%d/" % (PAGE_HOST, srv.server_address[1])],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(12)
    p.kill()
    p.wait()
    for th in threads:
        th.join()
    srv.shutdown()
    tcp.close()
    udp.close()
    print("%s: %s" % (label, ", ".join("%s=%d" % (k, v) for k, v in hits.items())))
    return hits


fails = 0
shipped = run("shipped-switches", "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE %s" % PAGE_HOST)
control = run("control-target-excluded",
              "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE %s, EXCLUDE %s" % (PAGE_HOST, TARGET))
for probe in PROBES:
    if shipped[probe]:
        print("FAIL: %s reached the address-literal listener" % probe)
        fails += 1
    if not control[probe]:
        print("FAIL: INCONCLUSIVE: %s did not arrive in the control run" % probe)
        fails += 1
print("PASS" if fails == 0 else "FAILED (%d)" % fails)
sys.exit(1 if fails else 0)
PY
