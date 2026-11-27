#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Package-install verification, booted-systemd half (design §9, legs 1 and
# 5): the unit as systemd actually runs it, and the restart-backoff
# latch.  Runs INSIDE a container whose PID 1 is systemd; the host-side driver
# (daemon-install-rig.sh) starts that container and falls back to a documented
# skip when the runner cannot host one.
#
#   verify-daemon-systemd.sh --package PATH [--work DIR]
#
# Nothing here is emulated: the maintainer scripts take their systemd branch,
# systemd-sysusers/systemd-tmpfiles run as the package intends, User=/Group=/
# CapabilityBoundingSet=/RuntimeDirectory= are applied by systemd, and the
# assertions read systemd's own view of the service.
#
# The backoff leg is the one that cannot be observed any other way.  A
# PERMANENT refusal to start (§6 degrade rules: cache directory missing,
# unwritable, or holding foreign-owned daemon files) must LATCH `failed` so an
# operator and monitoring see it -- not restart every RestartSec forever.  That
# depends on StartLimitIntervalSec=/StartLimitBurst= sitting in [Unit], where
# systemd honours them; in [Service] systemd ignores them with a warning and
# the daemon would loop.  The deb/rpm self-tests already assert the DIRECTIVES
# are in [Unit]; this asserts the resulting BEHAVIOUR.
set -euo pipefail

PACKAGE=""
WORK="/work"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --package) PACKAGE="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    *) echo "usage: verify-daemon-systemd.sh --package PATH [--work DIR]" >&2; exit 2 ;;
  esac
done
[[ -f "$PACKAGE" ]] || { echo "error: package not found: $PACKAGE" >&2; exit 2; }
[[ -d /run/systemd/system ]] || {
  echo "error: no booted systemd in this container" >&2; exit 2; }
mkdir -p "$WORK"

PKG=pagespeed-optimizer
CACHE_DIR=/var/cache/$PKG/v3
RUN_DIR=/run/$PKG
fails=0
LEG_NAME=""; LEG_NOTE=""; LEG_FAILS=0
check() { # label expected actual
  if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
    echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails + 1)); fi
}
leg_begin() { LEG_NAME="$1"; LEG_NOTE="$2"; LEG_FAILS="$fails"; echo "-- leg: $1"; }
leg_end() {
  local status=PASS
  [[ "$fails" -gt "$LEG_FAILS" ]] && status=FAIL
  echo "RIG-RESULT: ${LEG_NAME}|${status}|${LEG_NOTE}"
}
show() { systemctl show "$PKG.service" -p "$1" --value; }

# ---------------------------------------------------- H6 leg helpers ----
# (Everything below observes the SHIPPED unit and the
# SHIPPED binary; nothing is emulated and no directive is retyped here --
# the drop-ins come out of the package payload.)
DROPIN_DIR="/etc/systemd/system/$PKG.service.d"
DOCDIR="/usr/share/doc/$PKG"
mkdir -p "$DROPIN_DIR"

# `systemd-analyze` colourises its output even with no tty on systemd >= 255,
# so every parse in this file goes through here.  A raw parse silently
# yields a fraction of the real answer, which is worse than failing.
sd() { SYSTEMD_COLORS=0 systemd-analyze "$@" 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g'; }

# `systemd-analyze verify` prints its diagnostics on STDERR, not stdout, so
# sd() above is exactly the wrong tool for reading them: it discards stderr
# before the pipe, and a caller that appends 2>&1 gets nothing back either.
# An earlier revision of the unknown-group guard did that and therefore
# passed for the broken spelling too -- a regression guard that could not
# fail. The 2>&1 here IS the assertion; do not route this through sd().
sd_verify_warnings() { # unit-path -> the warnings, one per line, or empty
  SYSTEMD_COLORS=0 systemd-analyze verify "$1" 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' \
    | grep -iE 'unknown system call group|unknown system call|unknown setting' \
    | tr '|' '/' | head -5 || true
}

install_dropin() { # example-basename installed-basename
  install -D -m 0644 "$DOCDIR/$1" "$DROPIN_DIR/$2"
  systemctl daemon-reload
}
remove_dropin() { rm -f "$DROPIN_DIR/$1"; systemctl daemon-reload; }

# The two postures an operator can put the SHIPPED profile in, each driven
# through the file the package ships -- never a retyped copy -- so a leg that
# passes here is evidence about the documented procedure, not about the rig.
#
# The browser-analysis profile (20-browser-analysis.conf): re-admits what
# headless Chrome's sandbox needs on top of the unit's enforcing allow-list.
# Shipped ACTIVE as a vendor drop-in next to the unit, so "on" is the
# default (nothing under /etc) and "off" is the documented mask: an EMPTY
# same-named file under /etc replaces the vendor one and contributes
# nothing.  Both are what an operator does, through the shipped file.
BROWSER_DROPIN=20-browser-analysis.conf
browser_shipped_path() { # -> the vendor file systemd loaded, or ""
  systemctl show -p DropInPaths --value "$PKG.service" | tr ' ' '\n' \
    | grep -E "^/(usr/)?lib/systemd/system/$PKG.service.d/$BROWSER_DROPIN\$" | head -1 || true
}
browser_profile_on()  { rm -f "$DROPIN_DIR/$BROWSER_DROPIN"; systemctl daemon-reload; }
browser_profile_off() {
  install -D -m 0644 /dev/null "$DROPIN_DIR/$BROWSER_DROPIN"; systemctl daemon-reload
}
# The documented OPT-OUT (90-syscall-filter-off.conf): resets the allow-list
# and the namespace restriction, back to the pre-2.1 log-only posture.
OPTOUT_DROPIN=90-syscall-filter-off.conf
optout_on()  { install_dropin 90-syscall-filter-off.conf.example "$OPTOUT_DROPIN"; }
optout_off() { remove_dropin "$OPTOUT_DROPIN"; }
# systemd's own rendering of the effective allow-list: a long space-separated
# name list when enforcing, a bare `~` (empty deny-list) when nothing is
# filtered.  This -- not the daemon's `syscall_filter` field -- is the
# observation that tells the two postures apart.
scf() { systemctl show -p SystemCallFilter --value "$PKG.service"; }
scf_is_allowlist() { # -> 1 when a non-empty allow-list is in force
  local v; v="$(scf)"
  [[ -n "$v" && "$v" != "~"* ]] && echo 1 || echo 0
}
scf_has() { # name -> 1 when the effective allow-list admits it
  scf | tr ' ' '\n' | grep -qx -- "$1" && echo 1 || echo 0
}

restart_and_wait() { # -> 0 if active within the deadline
  systemctl reset-failed "$PKG.service" 2>/dev/null || true
  systemctl restart "$PKG.service" 2>/dev/null || true
  for _ in $(seq 1 60); do
    [[ "$(systemctl is-active "$PKG.service")" == active ]] && return 0
    [[ "$(systemctl is-active "$PKG.service")" == failed ]] && return 1
    sleep 0.5
  done
  return 1
}

# Detector 1 of §3.4: systemd's own view.  Two properties, measured on the
# rig rather than assumed:
#
#   ExecMainCode is the raw CLD_* NUMBER, not a word -- 1=CLD_EXITED,
#   2=CLD_KILLED, 3=CLD_DUMPED.  A SIGSYS kill reports 3, not 2, and it does
#   so even under LimitCORE=0: the kernel classifies SIGSYS as a
#   core-generating signal whether or not a core is actually written.
#
#   ExecMainStatus is then the signal number, and 31 is SIGSYS on x86_64 and
#   aarch64 alike.
#
# Result= is deliberately not the test -- it says `core-dump` here, which
# reads like a different failure than the kill it actually is.
CLD_EXITED=1
died_of_sigsys() {
  local code status
  code="$(show ExecMainCode)"; status="$(show ExecMainStatus)"
  [[ ( "$code" == 2 || "$code" == 3 ) && "$status" == 31 ]] && echo 1 || echo 0
}
# Detector 2: the syscall NAME, so a red run is actionable in one step.
# Audit is usually unavailable in a container, so the kernel ring buffer is
# the fallback and "no detector available" is reported as such rather than
# quietly passing.
sigsys_syscall_names() {
  { journalctl -k --no-pager -o cat 2>/dev/null; dmesg 2>/dev/null; } \
    | grep -oE 'SECCOMP.*syscall=[0-9]+' | tail -5 | tr '\n' ' ' || true
}

# Kernel SIGSYS records for Chrome PROCESSES (type=1326, code=0x8...),
# shared by the browser legs (h6-browser-analysis and h6-browser-soak).
# `Chrome exited` in the journal covers only the browser process: a renderer
# child killed mid-page leaves the browser up and the render at 200
# (measured -- that is how sched_setaffinity was found).  The ring buffer
# sees the child.  It is host-global in a container, so the legs assert a
# DELTA across their own cycle, never an absolute count; and if the buffer
# is unreadable here that sub-check is a named SKIP, not "none".
#
# Match on exe= as well as comm=: the record carries the faulting THREAD's
# comm, and Chrome renames its threads -- a MemoryInfra-thread mincore kill
# of the whole browser process (rc.11 CI) recorded comm="MemoryInfra" and a
# comm="chrom"-only match reported kills=0 while the journal's own
# `Chrome exited (status=0, signal=31)` line said otherwise.
CHROME_BIN=/usr/bin/chrome-headless-shell
KILL_DETECTOR=1
if dmesg >/dev/null 2>&1; then
  chrome_kills() { dmesg 2>/dev/null | grep -E 'type=1326.*(comm="chrom|exe="[^"]*chrom)' | grep -cE 'code=0x8' || true; }
elif journalctl -k -q -n1 >/dev/null 2>&1; then
  chrome_kills() { journalctl -k --no-pager -o cat 2>/dev/null | grep -E 'type=1326.*(comm="chrom|exe="[^"]*chrom)' | grep -cE 'code=0x8' || true; }
else
  KILL_DETECTOR=0
  chrome_kills() { echo 0; }
fi

# Positive-control kills, counted with the same source and record form as
# chrome_kills (type=1326, code=0x8) but for the control's own process.
control_kills() {
  { if dmesg >/dev/null 2>&1; then dmesg 2>/dev/null
    else journalctl -k --no-pager -o cat 2>/dev/null; fi; } \
    | grep -E 'type=1326.*(comm="(python|h6)|exe="[^"]*(python|h6))' | grep -cE 'code=0x8' || true
}

api_get() { # path -> body on stdout ("" when the API is not up)
  curl -s --max-time 5 --unix-socket "$RUN_DIR/api.sock" \
    "http://localhost$1" 2>/dev/null || true
}
json_field() { # json field
  printf '%s' "$1" | python3 -c '
import json,sys
try: print(json.load(sys.stdin).get(sys.argv[1], ""))
except Exception: print("")' "$2" 2>/dev/null || true
}
# The capture error text from an error envelope ({"error":{"message":...}}).
error_message() { # file
  python3 -c '
import json,sys
try:
    e = json.load(open(sys.argv[1])).get("error", "")
    print(e.get("message", "") if isinstance(e, dict) else e)
except Exception:
    print("")' "$1" 2>/dev/null || true
}
# Retry decision for a failed render.  Two cases earn exactly one retry:
#   - a navigation timeout (a deadline really expired);
#   - a fast 504 (a failed CDP step) when the kill detector is readable and
#     saw no Chrome kill since the cycle began and no 'Chrome exited' line was
#     logged: a transient browser-start race, with no sign of a sandbox kill.
# Anything else stays a FAIL: a non-504, a 504 with a kill record or a Chrome
# exit, and a fast 504 where the kill detector is unavailable (that failure
# could be an unseen kill).  Prints "retry" or "no".
should_retry_render() { # http-code message detector kill_delta exited
  if [[ "$1" == "504" && "$2" == *"navigation timeout"* ]]; then echo retry
  elif [[ "$1" == "504" && "${3:-0}" == "1" && "${4:-1}" == "0" && "${5:-1}" == "0" ]]; then echo retry
  else echo no; fi
}

# Turn the API on for the legs that read /v1/health.  Written as its own
# drop-in so it composes with the H6 ones and is removed with them.
enable_api_dropin() {
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/00-rig-api.conf" <<EOF
[Service]
Environment=OPTIMIZER_OPTS=--api-socket
EOF
  systemctl daemon-reload
}
wait_for_api() {
  for _ in $(seq 1 60); do
    [[ -n "$(api_get /v1/health)" ]] && return 0
    sleep 0.5
  done
  return 1
}
# After a restart the API answers before Chrome is up.  Wait (bounded, 30 s)
# until /v1/health reports the browser running with no consecutive failures,
# so the first render does not race the browser start.  Always returns 0 so
# the checks that follow report the real state.
wait_for_browser() {
  local up
  for _ in $(seq 1 60); do
    up="$(api_get /v1/health | python3 -c '
import json,sys
try:
    b=json.load(sys.stdin).get("browser",{})
    print(1 if b.get("chrome_running") and not b.get("chrome_consecutive_failures") else 0)
except Exception:
    print(0)' 2>/dev/null || true)"
    [[ "$up" == 1 ]] && return 0
    sleep 0.5
  done
  return 0
}

echo "=== $(. /etc/os-release && echo "$PRETTY_NAME") -- booted systemd $(systemctl --version | head -1) ==="

leg_begin systemd-identity \
  "unit runs unprivileged under real systemd; no emulation of User=/RuntimeDirectory="
if [[ "$PACKAGE" == *.deb ]]; then dpkg -i "$PACKAGE"; else rpm -i "$PACKAGE"; fi

# The unit is started by the maintainer script itself (systemctl restart on
# configure); give it a moment to come up on its own.
for _ in $(seq 1 60); do
  [[ "$(systemctl is-active "$PKG.service")" == active ]] && break
  sleep 0.5
done
check "service is active after install" "active" "$(systemctl is-active "$PKG.service")"
check "systemd runs the unit as pagespeed" "pagespeed" "$(show User)"
check "systemd runs the unit in group pagespeed" "pagespeed" "$(show Group)"
check "unit's capability bounding set is empty" "" "$(show CapabilityBoundingSet)"

MAINPID="$(show MainPID)"
check "service has a main pid" "1" "$([[ "$MAINPID" =~ ^[1-9][0-9]*$ ]] && echo 1 || echo 0)"
STATUS=/proc/$MAINPID/status
check "main process runs as uid pagespeed" "$(id -u pagespeed)" \
  "$(awk '/^Uid:/{print $2}' "$STATUS")"
check "main process holds no effective capabilities" "0000000000000000" \
  "$(awk '/^CapEff:/{print $2}' "$STATUS")"
check "main process capability bounding set is empty" "0000000000000000" \
  "$(awk '/^CapBnd:/{print $2}' "$STATUS")"
check "no secret flag on the command line" "0" \
  "$(tr '\0' ' ' < "/proc/$MAINPID/cmdline" | grep -cE -- '--api-token' || true)"
check "runtime dir created 0750 pagespeed:pagespeed" "750 pagespeed:pagespeed" \
  "$(stat -c '%a %U:%G' "$RUN_DIR")"
for _ in $(seq 1 60); do
  [[ -S "$RUN_DIR/notify.sock" ]] && break
  sleep 0.5
done
check "notify socket 0660 pagespeed:pagespeed" "660 pagespeed:pagespeed" \
  "$(stat -c '%a %U:%G' "$RUN_DIR/notify.sock")"
check "versioned cache dir 3770 pagespeed:pagespeed" "3770 pagespeed:pagespeed" \
  "$(stat -c '%a %U:%G' "$CACHE_DIR")"
check "no world-writable entry under the cache root" "0" \
  "$(find /var/cache/$PKG -perm -o+w | wc -l | tr -d ' ')"

leg_end

# ------------------------------------------------- restart-backoff latch ----
leg_begin restart-backoff \
  "a permanent refusal latches failed (start-limit-hit) instead of looping"
# Induce a PERMANENT refusal: hand the daemon a cache directory it cannot
# write.  Per §6 it must refuse (never chown, never fall back), and per the
# [Unit] start limit the retries must stop.
systemctl stop "$PKG.service"
rm -rf "$CACHE_DIR"
install -d -m 0700 -o root -g root "$CACHE_DIR"
systemctl reset-failed "$PKG.service" || true
systemctl start "$PKG.service" || true

deadline=$((SECONDS + 120))
while [[ $SECONDS -lt $deadline ]]; do
  state="$(systemctl is-active "$PKG.service" || true)"
  [[ "$state" == failed ]] && break
  sleep 2
done
NRESTARTS="$(show NRestarts)"
echo "ActiveState=$(show ActiveState) SubState=$(show SubState) Result=$(show Result) NRestarts=${NRESTARTS}"
check "a permanent refusal latches failed" "failed" "$(systemctl is-active "$PKG.service" || true)"

check "it really did retry before latching" "1" \
  "$([[ "$NRESTARTS" -ge 1 ]] && echo 1 || echo 0)"
check "retries are bounded by StartLimitBurst" "1" \
  "$([[ "$NRESTARTS" -le 5 ]] && echo 1 || echo 0)"
# The load-bearing observation: with RestartSec=5 a unit that is still
# looping collects two or three more restarts in this window.  A latched one
# collects none.  This is what goes red if StartLimitBurst= ever slides back
# into [Service], where systemd ignores it.
sleep 15
check "no further restarts after the latch" "$NRESTARTS" "$(show NRestarts)"
check "the unit is still failed, not restarting" "failed" \
  "$(systemctl is-active "$PKG.service" || true)"
journalctl -u "$PKG.service" --no-pager > "$WORK/refusal.log" || true
check "the refusal names the cause in the journal" "1" \
  "$(grep -cim1 -e 'permission denied' -e 'not writable' "$WORK/refusal.log" || true)"
check "systemd says the start rate limit stopped the loop" "1" \
  "$(grep -cim1 'repeated too quickly' "$WORK/refusal.log" || true)"
check "the refusal never chowned the directory" "root:root" \
  "$(stat -c '%U:%G' "$CACHE_DIR")"

# Recover, so the latch is shown to be about the fault and not about the
# package being broken.
rm -rf "$CACHE_DIR"
systemd-tmpfiles --create $PKG.conf
systemctl reset-failed "$PKG.service"
systemctl start "$PKG.service"
for _ in $(seq 1 60); do
  [[ "$(systemctl is-active "$PKG.service")" == active ]] && break
  sleep 0.5
done
check "service starts again once the cause is fixed" "active" \
  "$(systemctl is-active "$PKG.service")"
leg_end

# ------------------------------------------------- reload honesty (#1465) ----
# The daemon's settings are fixed at startup, so `reload` has no honest
# semantics: it must FAIL LOUDLY, never silently restart (SIGHUP's default
# disposition, the bug) and never silently succeed (which ignoring SIGHUP
# alone would produce).  Three observations pin all three halves: the verb
# refuses, the refusal says what to do instead, and the daemon's main pid
# survives both the refused verb AND a direct SIGHUP (the stray-signal
# fence, which the ExecReload= cannot provide).
leg_begin reload-honesty \
  "systemctl reload refuses loudly and points at restart; SIGHUP never kills the daemon"
RELOAD_PID="$(show MainPID)"
RELOAD_RC=0
systemctl reload "$PKG.service" >"$WORK/reload.out" 2>&1 || RELOAD_RC=$?
check "systemctl reload refuses (non-zero exit)" "1" \
  "$([[ "$RELOAD_RC" -ne 0 ]] && echo 1 || echo 0)"
# systemd reports the failure and the control process's stderr lands in the
# journal; either surface may carry the message, so check both.
{ journalctl -u "$PKG.service" --no-pager; cat "$WORK/reload.out"; } \
  > "$WORK/reload.log" 2>/dev/null || true
check "the refusal points at systemctl restart" "1" \
  "$(grep -cim1 'reload is not supported' "$WORK/reload.log" || true)"
check "the refused reload left the main pid alone" "$RELOAD_PID" "$(show MainPID)"
# The other half of the fence: a SIGHUP that does not come from systemctl.
kill -HUP "$RELOAD_PID" 2>/dev/null || true
sleep 1
check "a direct SIGHUP leaves the main pid alone" "$RELOAD_PID" "$(show MainPID)"
check "service still active after SIGHUP" "active" \
  "$(systemctl is-active "$PKG.service")"
leg_end

# ===========================================================================
# The syscall profile.
#
# The shipped unit is ENFORCING (2.1; the allow-list is in the unit itself).
# These legs assert that the default install really enforces (leg
# h6-enforcing-default), that the kill is real and that the DOCUMENTED
# opt-out really turns it off (leg h6-enforce-control, positive under the
# default and negative under the shipped opt-out example), that the real
# daemon survives the profile in every shipped posture including the
# opt-out (leg h6-enforce-service), and that the namespace polarity and the
# sandbox-probe coupling behave as designed -- all against the SHIPPED unit,
# the SHIPPED drop-in files and the SHIPPED binary, never a retyped copy.
# ===========================================================================

# The forbidden-call control.  swapon(2) is outside @system-service on every
# systemd this product targets, needs no privilege to ATTEMPT, and has no
# effect if it is merely permitted (EPERM/ENOENT for an unprivileged caller
# with a path that does not exist).  Written in python because the rig
# images have python3 and no compiler, and run as the unit's own ExecStart so
# it inherits the shipped unit's security context exactly.
cat > /usr/local/bin/h6-forbidden-syscall <<'PYEOF'
#!/usr/bin/python3
import ctypes, sys
libc = ctypes.CDLL(None, use_errno=True)
sys.stderr.write("h6: calling swapon(2)\n"); sys.stderr.flush()
rc = libc.swapon(b"/nonexistent/h6-control", 0)
sys.stderr.write("h6: swapon returned %d -- NOT killed\n" % rc); sys.stderr.flush()
sys.exit(0)
PYEOF
chmod 0755 /usr/local/bin/h6-forbidden-syscall

# The namespace control: attempt one namespace type and report by exit code.
cat > /usr/local/bin/h6-unshare <<'PYEOF'
#!/usr/bin/python3
# Exit codes are the leg's assertion surface, so they are specific:
#   0 = the namespace was created
#   1 = refused with EPERM  (what RestrictNamespaces= does -- an errno, not a kill)
#   3 = refused with some OTHER errno (reported, never conflated with EPERM)
#   2 = the child was killed by a signal (SIGSYS would mean the SYSCALL FILTER
#       denied unshare, which is a different control and a different fix)
import ctypes, errno, os, sys
FLAGS = {"user": 0x10000000, "net": 0x40000000, "uts": 0x04000000}
libc = ctypes.CDLL(None, use_errno=True)
r, w = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(r)
    ctypes.set_errno(0)
    rc = libc.unshare(FLAGS[sys.argv[1]])
    os.write(w, b"%d" % (0 if rc == 0 else ctypes.get_errno()))
    os._exit(0 if rc == 0 else 1)
os.close(w)
saved = os.read(r, 16).decode() or "0"
_, status = os.waitpid(pid, 0)
if os.WIFSIGNALED(status):
    sig = os.WTERMSIG(status)
    sys.stderr.write("h6-unshare: %s KILLED by signal %d%s\n"
                     % (sys.argv[1], sig, " (SIGSYS: the syscall filter, not "
                        "RestrictNamespaces)" if sig == 31 else ""))
    sys.exit(2)
if os.WEXITSTATUS(status) == 0:
    sys.stderr.write("h6-unshare: %s -> allowed\n" % sys.argv[1])
    sys.exit(0)
en = int(saved)
name = errno.errorcode.get(en, str(en))
sys.stderr.write("h6-unshare: %s -> denied with %s\n" % (sys.argv[1], name))
sys.exit(1 if en == errno.EPERM else 3)
PYEOF
chmod 0755 /usr/local/bin/h6-unshare

# Swap the unit's ExecStart for a control binary, with restarts off so the
# terminal state is readable.
control_dropin() { # command...
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/zz-rig-control.conf" <<EOF
[Service]
ExecStart=
ExecStart=$*
Restart=no
EOF
  systemctl daemon-reload
}
run_control() { # -> prints "ExecMainCode ExecMainStatus (Result)"
  systemctl reset-failed "$PKG.service" 2>/dev/null || true
  systemctl start "$PKG.service" 2>/dev/null || true
  for _ in $(seq 1 60); do
    case "$(systemctl is-active "$PKG.service")" in
      activating|active) sleep 0.5 ;;
      *) break ;;
    esac
  done
  echo "$(show ExecMainCode) $(show ExecMainStatus) ($(show Result))"
}

# ------------------------------------------------- leg: unit hardening ----
# Design §7.3 leg 8: the drift tripwire.  A directive silently dropped from
# the unit is not a test failure anywhere else in this repo.
leg_begin h6-unit-hardening \
  "shipped unit carries the H6 directives incl. the enforcing allow-list, scores under threshold, and the shipped binary has no selftest switch"
systemctl stop "$PKG.service" 2>/dev/null || true
rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
restart_and_wait || true

# The unit FILE, not `systemctl cat`: the latter appends every drop-in, and
# the per-line assertions below are about what the unit itself carries.
UNIT_PATH="$(systemctl show "$PKG.service" -p FragmentPath --value)"
UNIT_TEXT="$(cat "$UNIT_PATH")"
for d in "ProtectKernelTunables=yes" "ProtectKernelModules=yes" \
         "ProtectKernelLogs=yes" "ProtectControlGroups=yes" \
         "ProtectClock=yes" "ProtectHostname=yes" "ProtectProc=invisible" \
         "RestrictSUIDSGID=yes" "RestrictRealtime=yes" "LockPersonality=yes" \
         "SystemCallArchitectures=native" "LimitCORE=0" \
         "RestrictAddressFamilies=AF_UNIX AF_NETLINK AF_INET AF_INET6" \
         "SystemCallFilter=@system-service mincore" \
         "SystemCallFilter=~@privileged @resources" \
         "RestrictNamespaces=yes" \
         "SystemCallLog=~@system-service mincore"; do
  check "unit carries $d" "1" \
    "$(printf '%s' "$UNIT_TEXT" | grep -cxF "$d" || true)"
done
# 2.1: the SHIPPED unit enforces.  The assertion that used to matter most
# here was the negative one (no SystemCallFilter=, the flip must be
# deliberate); the flip has been made, so what must now be impossible is
# enforcement quietly DISAPPEARING -- a reset line in the unit, an errno
# conversion, or a third filter line widening the measured profile.
check "shipped unit carries exactly the two measured filter lines" "2" \
  "$(printf '%s' "$UNIT_TEXT" | grep -c '^SystemCallFilter=' || true)"
check "shipped unit never resets its own filter" "0" \
  "$(printf '%s' "$UNIT_TEXT" | grep -c '^SystemCallFilter=$' || true)"
check "shipped unit converts no denial to an errno" "0" \
  "$(printf '%s' "$UNIT_TEXT" | grep -c '^SystemCallErrorNumber=' || true)"
# ...and systemd's OWN view of the default posture agrees: a real allow-list
# is in force (`systemctl show -p SystemCallFilter` is the documented
# confirmation), it admits what the daemon needs and denies the control's
# forbidden call.
check "systemd shows an enforcing allow-list on the default install" "1" \
  "$(scf_is_allowlist)"
check "the effective allow-list admits read(2)" "1" "$(scf_has read)"
check "the effective allow-list denies swapon(2)" "0" "$(scf_has swapon)"
check "the effective allow-list denies init_module(2)" "0" "$(scf_has init_module)"
SCF_DEFAULT_NAMES="$(scf | tr ' ' '\n' | grep -c . || true)"
# The browser profile is part of the default: systemd loaded the VENDOR
# drop-in (not something under /etc), it is readable, and its re-admissions
# are in the effective list while RestrictNamespaces is lifted.
BROWSER_SHIPPED="$(browser_shipped_path)"
check "systemd loaded the vendor browser drop-in on the default install" "1" \
  "$([[ -n "$BROWSER_SHIPPED" ]] && echo 1 || echo 0)"
check "nothing under /etc contributes to the default posture" "" \
  "$(systemctl show -p DropInPaths --value "$PKG.service" | tr ' ' '\n' | grep '^/etc/' || true)"
check "the default allow-list admits seccomp(2) for Chrome's sandbox" "1" "$(scf_has seccomp)"
check "the default allow-list admits mincore(2) for Chrome's MemoryInfra" "1" "$(scf_has mincore)"
check "RestrictNamespaces is lifted by the browser profile on the default" "no" \
  "$(systemctl show -p RestrictNamespaces --value "$PKG.service")"

# A release binary must not carry a "make me crash" switch.  Asserted
# against the INSTALLED binary, two ways: the flag string is not in it,
# and it is rejected as unknown if anyone passes it.
check "shipped binary contains no selftest switch" "0" \
  "$(strings -a "/usr/bin/$PKG" 2>/dev/null \
     | grep -c -- '--selftest-forbidden-syscall' || true)"
SELFTEST_OUT="$("/usr/bin/$PKG" --selftest-forbidden-syscall 2>&1 | head -5 || true)"
check "shipped binary rejects the selftest switch by name" "1" \
  "$(printf '%s' "$SELFTEST_OUT" | grep -c 'Unknown option' || true)"

# systemd ACCEPTS a unit naming a syscall group it does not know: it logs
# "Unknown system call group, ignoring: @foo" and loads the unit with that
# group silently dropped from the filter.  For an allow-list that is a
# security hole that looks exactly like success -- the shipped
# 20-browser-analysis.conf originally said `@sandbox`, which does not exist
# before systemd 254, so on Debian 12 and AlmaLinux 9 (both 252) seccomp(2)
# vanished from the allow-list and Chrome's layer-2 sandbox would have been
# killed.  Nothing else in this rig would have caught it.  Verify the unit
# WITH the browser profile in force, the way a browser-analysis host runs it.
browser_profile_on
VERIFY_OUT="$(sd_verify_warnings "$UNIT_PATH")"
check "no unknown syscall group/setting in the unit + the browser profile" "" \
  "$VERIFY_OUT"

# THE NEGATIVE HALF. Everything above is an absence, and an absence is what a
# broken reader reports too, so the guard has to be shown catching a real
# unknown group before its silence means anything. Plant one, assert it is
# SEEN, remove it.
#
# @sandbox exists from systemd 254 on, where it is a legitimate group and
# produces no warning -- so on such a host this control is not applicable and
# says so out loud rather than passing quietly.
NEG_CONTROL="not run"
# Capture FIRST, then test. `systemd-analyze syscall-filter @sandbox` exits
# non-zero when the group is unknown, and with `set -o pipefail` that status
# is the pipeline's -- so the obvious `... | grep -q 'not found'` is FALSE on
# exactly the systemd versions where the group is missing, i.e. it inverted
# this gate and skipped the control on 252 while claiming 252 knew @sandbox.
SANDBOX_PROBE="$(SYSTEMD_COLORS=0 systemd-analyze syscall-filter @sandbox 2>&1 || true)"
if printf '%s' "$SANDBOX_PROBE" | grep -qi 'not found'; then
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/zz-rig-unknown-group.conf" <<'EOF'
[Service]
SystemCallFilter=@sandbox
EOF
  systemctl daemon-reload
  NEG_OUT="$(sd_verify_warnings "$UNIT_PATH")"
  check "the guard SEES an unknown group when one is planted" "1" \
    "$(printf '%s' "$NEG_OUT" | grep -ci 'sandbox' || true)"
  NEG_CONTROL="ran, warning seen"
  rm -f "$DROPIN_DIR/zz-rig-unknown-group.conf"; systemctl daemon-reload
else
  echo "SKIP: unknown-group negative control -- this systemd knows @sandbox" \
       "(254+), so planting it produces no warning and the control cannot" \
       "distinguish anything here. The guard is exercised on the 252 legs."
  NEG_CONTROL="SKIPPED (systemd knows @sandbox)"
fi
# ...and the shipped drop-in files are readable by the operator who uses them.
for f in "$DOCDIR/90-syscall-filter-off.conf.example" "${BROWSER_SHIPPED:-/nonexistent}" \
         "$DOCDIR/chrome-seccomp.json"; do
  check "$(basename "$f") ships 0644" "644" "$(stat -c '%a' "$f" 2>/dev/null)"
done
check "no retired 10-syscall-filter-enforce example is shipped" "0" \
  "$(ls "$DOCDIR" 2>/dev/null | grep -c '10-syscall-filter-enforce' || true)"
check "no browser-analysis example under /usr/share/doc (it ships active)" "0" \
  "$(ls "$DOCDIR" 2>/dev/null | grep -c '20-browser-analysis' || true)"
rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload

# `systemd-analyze security` scores the unit out of 10 (lower is better).
# The threshold is a RATCHET, not a target: it exists so that removing a
# directive goes red here.  Measured on all three rig distros at the time
# this landed; raise it only with a reason in the commit message.
# `set -o pipefail` is in force, so a `grep` that finds nothing fails the
# whole pipeline and, in an assignment, aborts the script under `set -e`.
# Every extraction below therefore ends in `|| true` -- "found nothing" is a
# result these legs report, not a reason to stop.
SEC_LINE="$(sd security "$PKG.service" | grep -i 'Overall exposure level' || true)"
SEC_SCORE="$(printf '%s' "$SEC_LINE" | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)"
H6_SECURITY_THRESHOLD="${H6_SECURITY_THRESHOLD:-4.5}"
check "systemd-analyze produced a score" "1" \
  "$([[ -n "$SEC_SCORE" ]] && echo 1 || echo 0)"
if [[ -n "$SEC_SCORE" ]]; then
  check "exposure score <= $H6_SECURITY_THRESHOLD (got $SEC_SCORE)" "1" \
    "$(python3 -c "print(1 if float('$SEC_SCORE') <= float('$H6_SECURITY_THRESHOLD') else 0)")"
fi
LEG_NOTE="systemd-analyze exposure ${SEC_SCORE:-?} (threshold ${H6_SECURITY_THRESHOLD}); shipped unit ENFORCES (default SystemCallFilter allow-list: ${SCF_DEFAULT_NAMES} names, admits read, denies swapon/init_module); binary carries no selftest switch; unknown-group negative control: ${NEG_CONTROL}"
leg_end

# -------------------------------------------- leg: the enforcing default ----
# Design §7.3 leg 2, on the posture that actually ships (2.1: ENFORCING).  A
# full cycle, and all three detectors of §3.4 asserted ABSENT -- the real
# daemon, doing real work, is not killed by its own default profile.
leg_begin h6-enforcing-default \
  "the shipped default installs an ENFORCING allow-list: systemd shows it, the daemon runs a full cycle under it, and nothing is killed"
enable_api_dropin
restart_and_wait || true
check "service is active on the shipped default" "active" \
  "$(systemctl is-active "$PKG.service")"
check "no SIGSYS on the shipped default" "0" "$(died_of_sigsys)"
check "systemd shows an enforcing allow-list on the shipped default" "1" \
  "$(scf_is_allowlist)"
API_UP=0; wait_for_api && API_UP=1 || true
check "the management API answers" "1" "$API_UP"
HEALTH="$(api_get /v1/health)"
FILTER_STATE="$(json_field "$HEALTH" syscall_filter)"
# Both the enforcing filter and SystemCallLog= are seccomp filters, so the
# daemon must report itself filtered -- and, as documented, that field
# cannot distinguish the enforcing posture from the opt-out; scf() above is
# what does.
check "the daemon reports itself filtered" "filtered" "$FILTER_STATE"
# ...and the assertion above is NOT by itself evidence that the UNIT
# installed anything.  Some container runtimes attach a filter to every
# process in the container (measured: one runtime shows an ambient
# Seccomp_filters of 1 here, another 0), and where they do, "filtered" would
# be true with the directive removed.  The non-vacuous form is the filter
# COUNT: the daemon must carry strictly more filters than a process that did
# not go through the unit.  Both numbers go in the leg note, so a reader can
# see which of the two observations this run could actually make.
filters_of() { awk '/^Seccomp_filters:/{print $2}' "$1" 2>/dev/null; }
AMBIENT_FILTERS="$(filters_of /proc/self/status)"; : "${AMBIENT_FILTERS:=0}"
DAEMON_PID="$(show MainPID)"
DAEMON_FILTERS="$(filters_of "/proc/$DAEMON_PID/status")"; : "${DAEMON_FILTERS:=0}"
check "the unit added a seccomp filter of its own (daemon $DAEMON_FILTERS > ambient $AMBIENT_FILTERS)" "1" \
  "$([[ "$DAEMON_FILTERS" -gt "$AMBIENT_FILTERS" ]] && echo 1 || echo 0)"
# Exercise the cycle the profile has to survive.
systemctl reload-or-restart "$PKG.service" 2>/dev/null || restart_and_wait
wait_for_api || true
systemctl stop "$PKG.service"; restart_and_wait || true
check "still active after a stop/start cycle" "active" \
  "$(systemctl is-active "$PKG.service")"
check "still no SIGSYS after the cycle" "0" "$(died_of_sigsys)"
SOAK="$(sigsys_syscall_names)"
LEG_NOTE="ENFORCING by default (SystemCallFilter allow-list of $(scf | tr ' ' '\n' | grep -c . || true) names); syscall_filter=${FILTER_STATE}; seccomp filters: daemon=${DAEMON_FILTERS} vs ambient=${AMBIENT_FILTERS}; no SIGSYS over start/restart/stop/start; kernel SECCOMP records seen: ${SOAK:-none}"
leg_end

# ------------------------------ leg: the positive control and its inverse ----
# Design §7.3 leg 3 -- THE gate on H6 being done.  A control that cannot fail
# proves nothing, so the same control is run twice: once under the SHIPPED
# DEFAULT (must be killed) and once with the DOCUMENTED OPT-OUT installed
# from the shipped example (must survive).  If the second half ever starts
# reporting a kill, the first half was never evidence of anything -- and the
# opt-out an operator is told about does not work.
leg_begin h6-enforce-control \
  "the shipped default kills a forbidden call -- and the documented opt-out lets the same call through"
systemctl stop "$PKG.service" 2>/dev/null || true
rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
control_dropin /usr/local/bin/h6-forbidden-syscall
check "default posture: systemd shows an enforcing allow-list" "1" "$(scf_is_allowlist)"
CK0="$(control_kills)"
POS="$(run_control)"
sleep 1
CK1="$(control_kills)"
check "under the shipped default: killed by a signal, not exited" "1" \
  "$([[ "$(echo "$POS" | awk '{print $1}')" != "$CLD_EXITED" ]] && echo 1 || echo 0)"
check "under the shipped default: SIGSYS (status 31)" "31" \
  "$(echo "$POS" | awk '{print $2}')"
check "under the shipped default: detector 1 agrees it was SIGSYS" "1" \
  "$(died_of_sigsys)"
# Attribution: without this, a control that died during python startup for an
# unrelated reason would look identical to a control that died on swapon(2).
# The helper announces the call on stderr and flushes BEFORE making it, so the
# line reaching the journal is what places the kill after that point.
journalctl -u "$PKG.service" --no-pager -n 60 > "$WORK/h6-poscontrol.log" 2>/dev/null || true
check "the control reached its forbidden call before being killed" "1" \
  "$(grep -c 'h6: calling swapon(2)' "$WORK/h6-poscontrol.log" || true)"
check "...and did NOT get past it" "0" \
  "$(grep -c 'h6: swapon returned' "$WORK/h6-poscontrol.log" || true)"
KILLED_NAME="$(sigsys_syscall_names)"
# Detector 2 of §3.4 (the syscall NAME from the kernel's audit record) needs
# a readable kernel log.  A container shares the host kernel but usually
# cannot read its ring buffer, and there is no auditd here -- so when it is
# empty, that is "this detector is unavailable", NOT "nothing was denied".
# Detectors 1 and 3 are what carry this leg; say which was available.
DETECTOR2="${KILLED_NAME:-unavailable in this container (no audit, kernel log unreadable)}"
# The positive control could not see its own kill: the kernel-log detector
# chrome_kills relies on is blind here (record count did not rise), so the
# browser legs report kills=unreadable, not a measured zero.
[[ "${CK1:-0}" -gt "${CK0:-0}" ]] || KILL_DETECTOR=0

# The inverse, through the documented opt-out and nothing else: the shipped
# 90-syscall-filter-off.conf.example installed where the docs say.
optout_on
check "with the opt-out installed: systemd shows no allow-list (SystemCallFilter=~)" "~" "$(scf)"
NEG="$(run_control)"
check "with the opt-out installed: exited normally, not by signal" "$CLD_EXITED" \
  "$(echo "$NEG" | awk '{print $1}')"
check "with the opt-out installed: nothing was killed" "0" \
  "$(died_of_sigsys)"
check "with the opt-out installed: exit status 0" "0" "$(echo "$NEG" | awk '{print $2}')"
journalctl -u "$PKG.service" --no-pager -n 30 > "$WORK/h6-control.log" 2>/dev/null || true
check "the surviving run says the call was not killed" "1" \
  "$(grep -c 'NOT killed' "$WORK/h6-control.log" || true)"

optout_off
rm -f "$DROPIN_DIR/zz-rig-control.conf"; systemctl daemon-reload
LEG_NOTE="positive (shipped default): $POS; negative control (documented opt-out installed, SystemCallFilter=~): $NEG; detector 2 (syscall name): ${DETECTOR2}"
leg_end

# ------------------------------- leg: the real daemon, enforcing profile ----
# Design §7.3 legs 2 and 4.  io_uring is the §7 R1 risk: libuv 1.52's Linux
# backend compiles it in, so if io_uring_setup/enter were outside the
# allow-list the daemon would be SIGSYS-killed at startup on a kernel that
# offers it.  Running the real daemon under the enforcing profile IS the
# measurement -- and it is run again with UV_USE_IO_URING=0 so the answer
# does not depend on which path libuv happened to take.  Then every other
# posture an operator can put the shipped files in: browser profile on,
# browser profile off, and the documented opt-out.
leg_begin h6-enforce-service \
  "the real daemon starts, serves and shuts down under the enforcing profile (with and without io_uring, with and without the browser profile) and under the documented opt-out"
enable_api_dropin
IOURING_ON=0; restart_and_wait && IOURING_ON=1 || true
check "daemon is active under the enforcing profile" "1" "$IOURING_ON"
check "no SIGSYS under the enforcing profile" "0" "$(died_of_sigsys)"
API2=0; wait_for_api && API2=1 || true
check "it still serves the management API" "1" "$API2"
HEALTH2="$(api_get /v1/health)"
check "and still reports itself filtered" "filtered" \
  "$(json_field "$HEALTH2" syscall_filter)"

install -D -m 0644 /dev/stdin "$DROPIN_DIR/30-rig-no-iouring.conf" <<EOF
[Service]
Environment=UV_USE_IO_URING=0
EOF
systemctl daemon-reload
IOURING_OFF=0; restart_and_wait && IOURING_OFF=1 || true
check "daemon is active with io_uring disabled too" "1" "$IOURING_OFF"
check "no SIGSYS with io_uring disabled" "0" "$(died_of_sigsys)"
remove_dropin 30-rig-no-iouring.conf

# ...with the browser-analysis profile stacked on top, which is the posture
# of a host that runs browser analysis.  It ADDS to the allow-list (seccomp,
# chroot and the privilege-drop calls) after the unit subtracted
# @privileged, so a drop-in that stopped composing with the unit would show
# up here as a daemon that will not start.
browser_profile_on
BOTH_OK=0; restart_and_wait && BOTH_OK=1 || true
check "daemon is active with the browser profile stacked on the unit" "1" "$BOTH_OK"
check "no SIGSYS with the browser profile stacked" "0" "$(died_of_sigsys)"
check "the browser profile widened the allow-list (seccomp admitted)" "1" "$(scf_has seccomp)"

# ...and WITHOUT it, through the documented mask (an empty same-named file
# under /etc).  Before 2.1 this was the mis-install to guard against: the
# browser file installed without the separate enforce drop-in defined a bare
# twelve-syscall allow-list and the daemon died at startup.  The allow-list
# now lives in the unit, so the browser profile is additive and masking it
# is simply the tighter, browser-analysis-off posture.  Asserted rather than
# stated, because "impossible by construction" is a claim too.
browser_profile_off
check "the mask replaced the vendor drop-in (systemd now loads the /etc file)" "0" \
  "$([[ -n "$(browser_shipped_path)" ]] && echo 1 || echo 0)"
# Restart=no pins a terminal state: a daemon that died and came back under
# Restart=on-failure would read as active in the gap between two deaths.
install -D -m 0644 /dev/stdin "$DROPIN_DIR/zz-rig-norestart.conf" <<'EOF'
[Service]
Restart=no
EOF
systemctl daemon-reload
systemctl stop "$PKG.service" 2>/dev/null || true
systemctl reset-failed "$PKG.service" 2>/dev/null || true
systemctl start "$PKG.service" 2>/dev/null || true
for _ in $(seq 1 30); do
  [[ "$(systemctl is-active "$PKG.service" || true)" == active ]] && break
  sleep 0.5
done
sleep 3
NOBROWSER="$(systemctl is-active "$PKG.service" || true)"
check "browser profile masked: the daemon stays up (no bare allow-list exists any more)" "active" "$NOBROWSER"
check "browser profile masked: no SIGSYS" "0" "$(died_of_sigsys)"
check "browser profile masked: seccomp(2) is not admitted" "0" "$(scf_has seccomp)"
check "browser profile masked: mincore(2) is still admitted (the cache's readahead check)" "1" "$(scf_has mincore)"
check "browser profile masked: RestrictNamespaces=yes is back in force" "yes" \
  "$(systemctl show -p RestrictNamespaces --value "$PKG.service")"
NOBROWSER_STATE="state=${NOBROWSER} code=$(show ExecMainCode) status=$(show ExecMainStatus)"
rm -f "$DROPIN_DIR/zz-rig-norestart.conf"; systemctl daemon-reload

# ...and under the documented OPT-OUT: the unit must still start and serve,
# the allow-list must be gone from systemd's view, and the daemon's own
# field must still say "filtered" (SystemCallLog= stays), which is exactly
# why the docs say not to read the posture off that field.
browser_profile_on
optout_on
OPTOUT_OK=0; restart_and_wait && OPTOUT_OK=1 || true
check "daemon is active under the documented opt-out" "1" "$OPTOUT_OK"
check "no SIGSYS under the opt-out" "0" "$(died_of_sigsys)"
check "systemd shows no allow-list under the opt-out (SystemCallFilter=~)" "~" "$(scf)"
check "RestrictNamespaces is lifted under the opt-out" "no" \
  "$(systemctl show -p RestrictNamespaces --value "$PKG.service")"
OPTOUT_API=0; wait_for_api && OPTOUT_API=1 || true
check "it still serves the management API under the opt-out" "1" "$OPTOUT_API"
check "and still reports itself filtered (SystemCallLog= remains)" "filtered" \
  "$(json_field "$(api_get /v1/health)" syscall_filter)"
optout_off
browser_profile_off
LEG_NOTE="enforcing profile: daemon active with io_uring available (${IOURING_ON}), with UV_USE_IO_URING=0 (${IOURING_OFF}), with the vendor browser profile (${BOTH_OK}); browser profile masked is SAFE (${NOBROWSER_STATE}); documented opt-out: active=${OPTOUT_OK} SystemCallFilter=~ api=${OPTOUT_API}; one per-name syscall addition (mincore, the cache's readahead check)"
leg_end

# --------------------------------------------- leg: namespace polarity ----
# Design §7.3 leg 6: assert the polarity BEHAVIOURALLY, never from the man
# page.  The unit's RestrictNamespaces=yes must actually deny, and the
# browser-analysis profile must actually re-permit -- in that order, because
# the whole point of the drop-in resetting the unit's directive is that the
# drop-in wins.
leg_begin h6-namespace-polarity \
  "the unit's RestrictNamespaces=yes really denies, and the browser-analysis profile really re-permits"
systemctl stop "$PKG.service" 2>/dev/null || true
rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
control_dropin /usr/local/bin/h6-unshare user
browser_profile_off
NS_DENIED="$(run_control)"
check "the namespace refusal is an errno, not a kill" "0" "$(died_of_sigsys)"
# Exit 1 specifically: refused with EPERM, which is what RestrictNamespaces=
# does. Any nonzero would also accept exit 3 (a different errno) or exit 2
# (killed by SIGSYS -- the syscall filter, a different control with a
# different fix), and would have called either of those a pass.
check "with RestrictNamespaces=yes: unshare(user) refused with EPERM" "1" \
  "$(echo "$NS_DENIED" | awk '{print $2}')"

browser_profile_on
NS_ALLOWED="$(run_control)"
check "with the browser-analysis profile: unshare(user) succeeds" "0" \
  "$(echo "$NS_ALLOWED" | awk '{print $2}')"
check "and it exited rather than being killed" "$CLD_EXITED" \
  "$(echo "$NS_ALLOWED" | awk '{print $1}')"
check "and the permitted case was not killed either" "0" "$(died_of_sigsys)"

rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
LEG_NOTE="unshare(CLONE_NEWUSER) under the unit's profile without the browser drop-in: ${NS_DENIED}; with the browser profile: ${NS_ALLOWED}"
leg_end

# ------------------------------------------ leg: probe under the filter ----
# Design §7.3 leg 7, and the coupling §6.1 wanted: under the unit's
# enforcing profile with NO browser-analysis profile, browser analysis must
# refuse and say why.  The daemon keeps serving; it never falls back to an
# unsandboxed browser.
leg_begin h6-probe-under-filter \
  "browser analysis refuses under the enforcing profile without the browser drop-in, names the cause, and never falls back"
# The quotes are load-bearing: systemd splits Environment= on whitespace, so
# the unquoted form assigns OPTIMIZER_OPTS=--api-socket and then treats
# --enable-browser-analysis as a second (invalid) assignment.  Browser
# analysis stays off, the daemon reports browser_sandbox="disabled", and this
# leg passes without testing anything.
install -D -m 0644 /dev/stdin "$DROPIN_DIR/00-rig-api.conf" <<EOF
[Service]
Environment="OPTIMIZER_OPTS=--api-socket --enable-browser-analysis"
EOF
browser_profile_off
restart_and_wait || true
check "the daemon still starts with browser analysis refused" "active" \
  "$(systemctl is-active "$PKG.service")"
check "no SIGSYS -- the refusal is a refusal, not a crash" "0" "$(died_of_sigsys)"
PROBE_API=0; wait_for_api && PROBE_API=1 || true
check "it still serves" "1" "$PROBE_API"
HEALTH3="$(api_get /v1/health)"
SANDBOX_STATE="$(json_field "$HEALTH3" browser_sandbox)"
# "disabled" would mean browser analysis never ran at all -- the leg would
# have proved nothing.  The states that mean the probe RAN and refused are
# "unavailable" (no Chrome sandbox here) and, on a host that could sandbox,
# "on".  Refuse to pass on "disabled".
check "browser analysis was actually enabled for this leg" "0" \
  "$([[ "$SANDBOX_STATE" == "disabled" || -z "$SANDBOX_STATE" ]] && echo 1 || echo 0)"
check "browser_sandbox is not reported as on" "0" \
  "$([[ "$SANDBOX_STATE" == "on" ]] && echo 1 || echo 0)"
journalctl -u "$PKG.service" --no-pager -n 200 > "$WORK/h6-probe.log" 2>/dev/null || true
# A4's message, asserted here for the first time under a REAL unit filter.
check "the refusal names a cause, not 'did not complete'" "0" \
  "$(grep -c 'did not complete' "$WORK/h6-probe.log" || true)"
check "it never falls back to an unsandboxed browser" "0" \
  "$(grep -c -- '--no-sandbox' "$WORK/h6-probe.log" || true)"
# Trimmed and pipe-stripped: the note goes into a `|`-delimited results file
# that the driver renders as a markdown table row, and the full refusal is a
# paragraph.  The whole message is in $WORK/h6-probe.log for anyone who wants
# it; the row only has to identify WHICH refusal fired.
PROBE_REASON="$(grep -oE 'unshare\(CLONE_NEWUSER\)[^"]*' "$WORK/h6-probe.log" \
  | tail -1 | tr '|' '/' | cut -c1-140 || true)"
# Everything asserted above this point is an ABSENCE (no SIGSYS, no
# "did not complete", no --no-sandbox), and every absence is trivially true of
# an empty file -- and the journal capture is `|| true`. So assert the
# PRESENCE of the refusal too, or a leg that captured nothing reports PASS.
check "the refusal was actually captured" "1" \
  "$([[ -n "$PROBE_REASON" ]] && echo 1 || echo 0)"
check "...and it names the namespace or filter cause" "1" \
  "$(printf '%s' "$PROBE_REASON" \
     | grep -cE 'was denied|killed by SIGSYS|syscall filter' || true)"
check "...and the daemon said it is refusing to start browser analysis" "1" \
  "$(grep -c 'REFUSING to start browser analysis' "$WORK/h6-probe.log" || true)"
rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
restart_and_wait || true
LEG_NOTE="browser_sandbox=${SANDBOX_STATE}; probe reason: ${PROBE_REASON:-(not logged -- browser analysis may have declined earlier)}"
leg_end

# ------------------------------------------ leg: address-family measurement ----
# Design §7.3 leg 5 and the §2.4 correction.  This leg MEASURES rather than
# asserts a guess: it removes one family at a time and records what the
# daemon does, so the shipped base set is a recorded observation and the
# INET-free tightening is offered only with evidence behind it.
leg_begin h6-address-families \
  "MEASUREMENT plus two assertions: the shipped four-family set serves, narrower sets are recorded, and the daemon recovers when the base set is restored"
enable_api_dropin
restart_and_wait || true
BASE_OK="$(systemctl is-active "$PKG.service" || true)"
check "the shipped base set (four families) serves" "active" "$BASE_OK"

af_probe() { # families... -> "state|api"
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/40-rig-af.conf" <<EOF
[Service]
RestrictAddressFamilies=
RestrictAddressFamilies=$*
EOF
  systemctl daemon-reload
  restart_and_wait || true
  local st api=0
  st="$(systemctl is-active "$PKG.service" || true)"
  wait_for_api && api=1 || true
  # No `|` in the value: the note it lands in is a `|`-delimited results row
  # the driver renders as a markdown table.
  echo "${st}/api=${api}"
}
NO_NETLINK="$(af_probe AF_UNIX AF_INET AF_INET6)"
NO_INET="$(af_probe AF_UNIX AF_NETLINK)"
# Reduced sets, recorded rather than required.  MEASURED, and it corrected a
# claim this leg first tried to assert: with AF_UNIX alone the daemon still
# starts and still serves.  That is not a defect and not a reason to narrow
# the shipped set -- it says only that the posture the rig can drive (a unix
# socket, no outbound fetch, no DNS) does not need the other three.  The
# families the shipped unit grants are the minimum for the FEATURE SET, not
# for this posture: AF_NETLINK is glibc's NSS/getaddrinfo path and
# AF_INET/AF_INET6 are the agent_optimize curl child and the optional TCP
# API, none of which the rig exercises.  Asserting "AF_UNIX alone must fail"
# here would have been asserting something untrue.
ONLY_UNIX="$(af_probe AF_UNIX)"
remove_dropin 40-rig-af.conf
restart_and_wait || true
check "the daemon recovers once the base set is restored" "active" \
  "$(systemctl is-active "$PKG.service")"
LEG_NOTE="base(four families)=${BASE_OK}; without AF_NETLINK=${NO_NETLINK}; without AF_INET pair=${NO_INET}; AF_UNIX only=${ONLY_UNIX}. MEASURED: this posture (unix socket, no outbound fetch, no DNS) needs only AF_UNIX -- the shipped four are the minimum for the FEATURE SET (NSS, the agent_optimize curl child, the optional TCP API), which this rig does not exercise"
leg_end


# ------------------------------------------------ leg: a real headless Chrome ----
# Issue #1472.  Everything above measures the daemon; this measures the one
# child that has its own sandbox.  Two halves, both against the SHIPPED
# unit and the SHIPPED drop-in files, with the daemon's default sandbox mode
# (require -- never off):
#
#   1. the shipped default -- the enforcing unit + the vendor browser
#      profile, nothing under /etc: Chrome starts sandboxed, renders pages
#      across a service restart, and keeps running -- so the browser
#      profile's re-admission list is proven SUFFICIENT, not just necessary.
#      This is the half a log-only soak cannot see (the @resources and
#      @privileged subtractions are inside @system-service and are never
#      logged), and it is the half that goes red first if a hardening
#      directive or a Chrome release breaks the sandbox.
#   2. the documented opt-out (log-only), browser profile still present: the
#      same cycle.  The negative control for half 1 -- a Chrome that dies
#      under the default and survives here died of the profile, not of
#      something else in the unit.
#
# Needs a Chromium in the image at the daemon's default path; the rig images
# for Debian 12 and Ubuntu 24.04 provide one.  Where there is none (EL9's
# base repos ship no Chromium) the leg records SKIP with that reason -- a
# skip that names a blocker, like the SELinux leg, not a quiet pass: the
# result row still says the claim was not verified on that distro.
leg_begin h6-browser-analysis \
  "a real headless Chrome, sandbox=require, under the enforcing unit with the browser profile and under the documented opt-out"
if [[ ! -x "$CHROME_BIN" ]]; then
  LEG_NOTE="SKIP: no Chromium at $CHROME_BIN in this rig image ($(. /etc/os-release && echo "$PRETTY_NAME") -- the base repos ship none), so neither half of the browser claim is verified on this distro"
  echo "RIG-RESULT: ${LEG_NAME}|SKIP|${LEG_NOTE}"
else
  # A page for Chrome to render, served on loopback; the daemon needs
  # --allow-private-urls to be pointed at it.  The API screenshot route is
  # the one full render path reachable without the serving module: CDP
  # navigate, renderer + GPU + network children, a captured frame.
  mkdir -p "$WORK/www"
  printf '<!doctype html><title>rig</title><style>h1{color:#123}</style><h1>h6-browser-analysis</h1><script>document.title="ok"</script>\n' \
    > "$WORK/www/index.html"
  ( cd "$WORK/www" && python3 -m http.server 8099 --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > "$WORK/www.pid" )
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/00-rig-api.conf" <<EOF
[Service]
Environment="OPTIMIZER_OPTS=--api-socket --enable-browser-analysis --allow-private-urls"
EOF
  systemctl daemon-reload

  browser_half() { # label -> sets B_HEALTH B_RENDER B_EXITED B_SIGSYS B_KILLS
    restart_and_wait || true
    wait_for_api || true
    wait_for_browser
    local t0; t0="$(date '+%Y-%m-%d %H:%M:%S')"
    local k0; k0="$(chrome_kills)"
    check "$1: service is active" "active" "$(systemctl is-active "$PKG.service")"
    check "$1: daemon not killed by SIGSYS" "0" "$(died_of_sigsys)"
    # Chrome is spawned at Initialize(); give the back-off a chance to show.
    sleep 5
    B_HEALTH="$(api_get /v1/health)"
    check "$1: browser_sandbox=on (require mode, probe passed)" "on" \
      "$(json_field "$B_HEALTH" browser_sandbox)"
    local browser; browser="$(printf '%s' "$B_HEALTH" | python3 -c '
import json,sys
b=json.load(sys.stdin).get("browser",{})
print("%s/%s" % (b.get("chrome_running"), b.get("chrome_consecutive_failures")))' 2>/dev/null || true)"
    check "$1: chrome_running=true with chrome_consecutive_failures=0" "True/0" "$browser"
    # Three renders with a service restart between the second and third:
    # the cycle the measurement behind the drop-in used, and enough for a
    # renderer child (spawned per render) to hit a subtracted syscall.
    B_RENDER=""; B_RETRIED=0; B_ERRS=""
    # Self-test of the retry decision against canned failures, so the branch
    # runs on every rig run without a real timeout.
    check "$1: retry decision: navigation timeout is retried" "retry" \
      "$(should_retry_render 504 'navigation timeout' 1 0 0)"
    check "$1: retry decision: fast 504 with no kill and no exit is retried" "retry" \
      "$(should_retry_render 504 'captureScreenshot failed' 1 0 0)"
    check "$1: retry decision: fast 504 with a kill record is not retried" "no" \
      "$(should_retry_render 504 'captureScreenshot failed' 1 1 0)"
    check "$1: retry decision: fast 504 with a Chrome exit is not retried" "no" \
      "$(should_retry_render 504 'captureScreenshot failed' 1 0 1)"
    check "$1: retry decision: fast 504 without a kill detector is not retried" "no" \
      "$(should_retry_render 504 'captureScreenshot failed' 0 0 0)"
    check "$1: retry decision: non-504 is not retried" "no" \
      "$(should_retry_render 500 'navigation timeout' 1 0 0)"
    local n code code2 msg msg2 label t_a t_b ms
    for n in 1 2 3; do
      render_once() { # outfile -> prints http code
        curl -s -o "$1" -w '%{http_code}' --max-time 90 \
          --unix-socket "$RUN_DIR/api.sock" -H 'Content-Type: application/json' \
          -H 'X-Requested-With: XMLHttpRequest' -X POST \
          http://localhost/v1/capture/screenshot \
          -d "{\"url\":\"http://127.0.0.1:8099/index.html?n=$n\"}" 2>/dev/null || echo 000
      }
      rm -f "$WORK/h6-shot-$n.json" "$WORK/h6-shot-$n-retry.json"
      t_a="$(date +%s%3N)"
      code="$(render_once "$WORK/h6-shot-$n.json")"
      t_b="$(date +%s%3N)"; ms=$(( t_b - t_a ))
      cp "$WORK/h6-shot-$n.json" "$WORK/h6-shot.json" 2>/dev/null || true
      label="$code"
      if [[ "$code" != "200" ]]; then
        msg="$(error_message "$WORK/h6-shot-$n.json")"
        echo "diag: $1: render $n -> $code after ${ms} ms: ${msg:-<no error message>}"
        journalctl -u "$PKG.service" --no-pager --since "$t0" -o cat 2>/dev/null \
          | tail -40 | sed 's/^/diag:   /' || true
        sleep 1
        kd=$(( $(chrome_kills) - k0 ))
        ex="$(journalctl -u "$PKG.service" --no-pager --since "$t0" -o cat 2>/dev/null | grep -c 'Chrome exited' || true)"
        decision="$(should_retry_render "$code" "$msg" "$KILL_DETECTOR" "$kd" "${ex:-0}")"
        if [[ "$decision" == "retry" ]]; then
          code2="$(render_once "$WORK/h6-shot-$n-retry.json")"
          cp "$WORK/h6-shot-$n-retry.json" "$WORK/h6-shot.json" 2>/dev/null || true
          msg2="$(error_message "$WORK/h6-shot-$n-retry.json")"
          echo "diag: $1: render $n retried once -> $code2${msg2:+: $msg2}"
          if [[ "$msg" == *"navigation timeout"* ]]; then
            label="${code}(navigation timeout)->${code2}"
          else
            label="${code}(fast, no kill)->${code2}"
          fi
          B_RETRIED=1
          code="$code2"; [[ "$code" != "200" ]] && msg="$msg2"
        fi
        if [[ "$code" != "200" ]]; then
          B_ERRS="${B_ERRS}${B_ERRS:+; }render $n: $code ${msg:-no message}"
        fi
      fi
      check "$1: render $n through Chrome (screenshot 200)" "200" "$code"
      check "$1: render $n returns image data" "1" \
        "$(grep -c '"data":"iVBOR' "$WORK/h6-shot.json" 2>/dev/null || true)"
      B_RENDER="${B_RENDER}${B_RENDER:+,}${label}"
      if [[ "$n" -eq 2 ]]; then
        systemctl reload-or-restart "$PKG.service" 2>/dev/null || true
        restart_and_wait || true
        wait_for_api || true
        wait_for_browser
        sleep 5
      fi
    done
    sleep 3
    B_KILLS=$(( $(chrome_kills) - k0 ))
    if [[ "$KILL_DETECTOR" -eq 1 ]]; then
      check "$1: no Chrome process killed by the kernel over the cycle (SIGSYS record delta)" "0" "$B_KILLS"
    else
      echo "SKIP: $1: kernel SIGSYS records unreadable in this container -- a killed renderer child would go unseen here"
      B_KILLS="unreadable"
    fi
    journalctl -u "$PKG.service" --no-pager --since "$t0" -o cat > "$WORK/h6-browser.log" 2>/dev/null || true
    B_EXITED="$(grep -c 'Chrome exited' "$WORK/h6-browser.log" || true)"
    B_SIGSYS="$(grep -c 'signal=31' "$WORK/h6-browser.log" || true)"
    if [[ "$B_EXITED" != "0" ]]; then
      # Chrome's own stderr is discarded by the daemon; its exit lines and the
      # host's user-namespace policy are what name the cause.
      echo "diag: $1: first Chrome exit lines:"
      grep 'Chrome exited' "$WORK/h6-browser.log" | head -3 | sed 's/^/diag:   /' || true
      echo "diag: kernel.apparmor_restrict_unprivileged_userns=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo absent)"
    fi
    check "$1: no 'Chrome exited' line over the cycle" "0" "$B_EXITED"
    check "$1: Chrome never died of SIGSYS" "0" "$B_SIGSYS"
    check "$1: still chrome_running after the render" "true" \
      "$(printf '%s' "$(api_get /v1/health)" | python3 -c '
import json,sys; print(str(json.load(sys.stdin).get("browser",{}).get("chrome_running")).lower())' 2>/dev/null || true)"
    check "$1: never falls back to an unsandboxed browser" "0" \
      "$(grep -c -- '--no-sandbox' "$WORK/h6-browser.log" || true)"
  }

  browser_profile_on
  check "shipped default: systemd shows an enforcing allow-list" "1" \
    "$(scf_is_allowlist)"
  check "shipped default: the vendor browser drop-in is what systemd loaded" "1" \
    "$([[ -n "$(browser_shipped_path)" ]] && echo 1 || echo 0)"
  browser_half "shipped default (enforcing unit + vendor browser profile)"
  ENFORCE_NOTE="shipped default (enforcing unit + vendor browser profile): renders=${B_RENDER} exited=${B_EXITED} sigsys=${B_SIGSYS} kills=${B_KILLS} retried=${B_RETRIED:-0} errors=${B_ERRS:-none}"

  optout_on
  check "documented opt-out: systemd shows no allow-list" "~" "$(scf)"
  browser_half "documented opt-out (log-only)"
  OPTOUT_NOTE="opt-out (log-only): renders=${B_RENDER} exited=${B_EXITED} kills=${B_KILLS} retried=${B_RETRIED:-0} errors=${B_ERRS:-none}"
  optout_off

  kill "$(cat "$WORK/www.pid" 2>/dev/null)" 2>/dev/null || true
  rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
  restart_and_wait || true
  LEG_NOTE="$(chrome_version="$("$CHROME_BIN" --version 2>/dev/null | head -1)"; echo "${chrome_version:-chromium}; ${ENFORCE_NOTE}; ${OPTOUT_NOTE} (kills = delta of kernel SIGSYS records for Chrome processes across the leg's own cycle)")"
  leg_end
fi

# ------------------------------------------------ leg: the long-idle soak ----
# Issue #1485.  h6-browser-analysis watches Chrome for ~40 s; the failure it
# could not catch (the 1.16.0-rc.11 flake, fixed in #1483) was a syscall
# with a MINUTES-SCALE period: Chromium's MemoryInfra thread calls
# mincore(2) in a burst every ~3-4 min, so a name missing from the browser
# drop-in kills Chrome only when a burst happens to land inside the window
# (~15-25% per run).  No assertion cadence fixes that; only a window longer
# than the period does.  This leg keeps a real sandboxed Chrome up under the
# enforcing unit + browser profile, IDLE -- the bursts fire without any
# render traffic; the #1483 A/B reproduced two kills 4m25s apart on an idle
# browser -- and asserts zero kills over the window.
#
# The window is H6_SOAK_SECONDS, deliberately defaulting to 0 (SKIP): two
# observed burst periods are ~8 minutes, a periodic-cadence window, not a
# per-PR one.  CI Periodic's daemon-rig-soak-x64 job runs this rig with
# H6_SOAK_SECONDS=570; the per-PR Daemon Privilege Rig matrix records the
# SKIP below, so the gap shows in every summary instead of vanishing.
leg_begin h6-browser-soak \
  "an idle sandboxed Chrome under the enforcing unit + browser profile survives a minutes-scale soak (the window/period blind spot behind the mincore miss)"
H6_SOAK_SECONDS="${H6_SOAK_SECONDS:-0}"
if [[ "$H6_SOAK_SECONDS" -le 0 ]]; then
  LEG_NOTE="SKIP: H6_SOAK_SECONDS=0 here -- the window must exceed two ~3-4 min burst periods (#1483), which is periodic cadence, not per-PR; CI Periodic's daemon-rig-soak-x64 job runs this leg at 570 s"
  echo "RIG-RESULT: ${LEG_NAME}|SKIP|${LEG_NOTE}"
elif [[ ! -x "$CHROME_BIN" ]]; then
  LEG_NOTE="SKIP: no Chromium at $CHROME_BIN in this rig image ($(. /etc/os-release && echo "$PRETTY_NAME") -- the base repos ship none), so the soak claim is not verified on this distro"
  echo "RIG-RESULT: ${LEG_NAME}|SKIP|${LEG_NOTE}"
else
  install -D -m 0644 /dev/stdin "$DROPIN_DIR/00-rig-api.conf" <<EOF
[Service]
Environment="OPTIMIZER_OPTS=--api-socket --enable-browser-analysis"
EOF
  browser_profile_on
  restart_and_wait || true
  check "service is active under the enforcing unit + browser profile" "active" \
    "$(systemctl is-active "$PKG.service")"
  check "systemd shows an enforcing allow-list for the soak" "1" "$(scf_is_allowlist)"
  check "daemon not killed by SIGSYS at startup" "0" "$(died_of_sigsys)"
  SOAK_API=0; wait_for_api && SOAK_API=1 || true
  check "the management API answers" "1" "$SOAK_API"
  # Chrome is spawned at Initialize(); give the back-off a chance to show.
  sleep 5
  SOAK_HEALTH="$(api_get /v1/health)"
  # "No kill" is only evidence if a browser was there to kill -- pin the
  # sandboxed, running state BEFORE the window, the same non-vacuity half
  # the #1483 A/B needed its log-only records for ("no kill" is not "no
  # burst").
  check "browser_sandbox=on (require mode, probe passed)" "on" \
    "$(json_field "$SOAK_HEALTH" browser_sandbox)"
  SOAK_BROWSER="$(printf '%s' "$SOAK_HEALTH" | python3 -c '
import json,sys
b=json.load(sys.stdin).get("browser",{})
print("%s/%s" % (b.get("chrome_running"), b.get("chrome_consecutive_failures")))' 2>/dev/null || true)"
  check "chrome_running=true with chrome_consecutive_failures=0" "True/0" "$SOAK_BROWSER"

  SOAK_T0="$(date '+%Y-%m-%d %H:%M:%S')"
  SOAK_K0="$(chrome_kills)"
  # Poll in slices rather than one long sleep: a kill at minute two fails
  # fast instead of burning the rest of the window, and the heartbeat keeps
  # the CI log honest about what the leg is doing.
  SOAK_ELAPSED=0
  while [[ "$SOAK_ELAPSED" -lt "$H6_SOAK_SECONDS" ]]; do
    sleep 30
    SOAK_ELAPSED=$((SOAK_ELAPSED + 30))
    SOAK_SO_FAR=$(( $(chrome_kills) - SOAK_K0 ))
    echo "soak: ${SOAK_ELAPSED}/${H6_SOAK_SECONDS}s idle, Chrome SIGSYS record delta: ${SOAK_SO_FAR}"
    [[ "$SOAK_SO_FAR" -gt 0 ]] && break
    [[ "$(systemctl is-active "$PKG.service" || true)" == active ]] || break
  done
  SOAK_KILLS=$(( $(chrome_kills) - SOAK_K0 ))
  if [[ "$KILL_DETECTOR" -eq 1 ]]; then
    check "no Chrome process killed by the kernel over the soak (SIGSYS record delta)" "0" "$SOAK_KILLS"
  else
    echo "SKIP: kernel SIGSYS records unreadable in this container -- a killed Chrome would go unseen by this detector"
    SOAK_KILLS="unreadable"
  fi
  journalctl -u "$PKG.service" --no-pager --since "$SOAK_T0" -o cat > "$WORK/h6-browser-soak.log" 2>/dev/null || true
  SOAK_EXITED="$(grep -c 'Chrome exited' "$WORK/h6-browser-soak.log" || true)"
  SOAK_SIGSYS="$(grep -c 'signal=31' "$WORK/h6-browser-soak.log" || true)"
  check "no 'Chrome exited' line over the soak" "0" "$SOAK_EXITED"
  check "Chrome never died of SIGSYS" "0" "$SOAK_SIGSYS"
  check "daemon still not killed by SIGSYS" "0" "$(died_of_sigsys)"
  check "service still active after the soak" "active" \
    "$(systemctl is-active "$PKG.service")"
  check "chrome_running=true after the soak" "true" \
    "$(printf '%s' "$(api_get /v1/health)" | python3 -c '
import json,sys; print(str(json.load(sys.stdin).get("browser",{}).get("chrome_running")).lower())' 2>/dev/null || true)"
  check "never falls back to an unsandboxed browser" "0" \
    "$(grep -c -- '--no-sandbox' "$WORK/h6-browser-soak.log" || true)"
  rm -f "$DROPIN_DIR"/*.conf; systemctl daemon-reload
  restart_and_wait || true
  LEG_NOTE="$(chrome_version="$("$CHROME_BIN" --version 2>/dev/null | head -1)"; echo "${chrome_version:-chromium}; soaked ${SOAK_ELAPSED}s idle under enforcing+20-: kills=${SOAK_KILLS} exited=${SOAK_EXITED} sigsys=${SOAK_SIGSYS} (kills = delta of kernel SIGSYS records for Chrome processes across the soak)")"
  leg_end
fi

rm -f "$DROPIN_DIR"/*.conf /usr/local/bin/h6-forbidden-syscall /usr/local/bin/h6-unshare
systemctl daemon-reload

if [[ "$fails" -gt 0 ]]; then
  echo "verify-daemon-systemd: $fails FAILURE(S)" >&2
  exit 1
fi
echo "verify-daemon-systemd: all checks passed"
exit 0
