#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Syscall census for the optimizer daemon (design §2.2).
#
#   syscall-census.sh --binary PATH --library PATH \
#       [--distro debian12|ubuntu2404|alma9] [--work DIR]
#   syscall-census.sh --in-container --binary PATH --library PATH --work DIR
#
# This is the MEASURE half of "measure, then filter".  It is deliberately a
# manual/periodic lane, not a per-PR gate: it needs strace (so, a container
# with CAP_SYS_PTRACE and no seccomp confinement of its own), it is slow, and
# its output is an INPUT to a human decision -- which systemd syscall groups
# the shipped unit names -- not an assertion.  The per-PR assertions live in
# verify-daemon-systemd.sh, which checks the CONSEQUENCE of that decision.
#
# What it produces, per distro:
#
#   census-<distro>.txt      every syscall name observed, per posture
#   census-<distro>.md       the same intersected with this distro's systemd
#                            @system-service group, split into
#                            "already admitted" / "NOT in @system-service"
#
# The second list is the only one that matters: a name in it is a call the
# shipped allow-list would kill, so it must either be explained (it belongs
# to a posture the base profile does not support -- browser analysis) or
# admitted by name.  An EMPTY second list is the result that lets
# `SystemCallFilter=@system-service` ship as-is.
#
# Why names and never numbers: syscall numbering differs per architecture
# (aarch64 has no open/stat/fork/select; x86_64 has no *_time64 forms), and
# systemd owns the name -> number mapping per arch.  The census output is a
# NAME list, intersected with a systemd GROUP, so the arch matrix stays
# systemd's problem and not ours (design §5.1).
#
# Postures (brief §2.2).  P1-P3 and P7 are measured here; P4-P6 are recorded
# as NOT MEASURED with the reason, because the rig image carries no Chrome:
#
#   P1  cold start, cache create+open, three UDS binds, a peer cache open
#       through the shipped client library, SIGTERM shutdown.  API off.
#   P2  P1 + --api-socket           (H5's default transport; AF_UNIX)
#   P3  P1 + --api-port + token     (the TCP posture; AF_INET)
#   P7  P1 + SIGHUP (the reload signal; ignored by design since #1465) before
#       the SIGTERM
#   P4  browser analysis + Chrome     NOT MEASURED (no Chrome in the rig image)
#   P5  agent_optimize curl child     curl measured STANDALONE (see below)
#   P6  license renewal curl child    same child, same set; RC-window only
#
# P5/P6 share one measurable fact -- the syscall set of a curl(1) child that
# inherits the parent's filter -- so the census straces a real curl fetch
# rather than pretending the daemon-side trigger is reachable here.  That is
# the honest half: it is why AF_INET/AF_INET6 stay in the base
# RestrictAddressFamilies= (§2.4), and it is recorded as such.
set -euo pipefail

BINARY=""
LIBRARY=""
DISTRO=""
WORK=""
IN_CONTAINER=0
UNIT_POSTURE=0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

usage() { sed -n '4,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    --library) LIBRARY="$2"; shift 2 ;;
    --distro) DISTRO="$2"; shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --in-container) IN_CONTAINER=1; shift ;;
    --unit-posture) UNIT_POSTURE=1; shift ;;
    *) usage ;;
  esac
done
[[ -n "$BINARY" && -n "$LIBRARY" ]] || usage

# --------------------------------------------------------------- driver ----
# NOT the driver when --unit-posture is set: that mode is already running
# inside a booted-systemd container, where there is no docker to drive.
if [[ "$IN_CONTAINER" -eq 0 && "$UNIT_POSTURE" -eq 0 ]]; then
  [[ -n "$DISTRO" ]] || DISTRO=debian12
  case "$DISTRO" in debian12|ubuntu2404|alma9) ;; *) usage ;; esac
  BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"
  LIBRARY="$(cd "$(dirname "$LIBRARY")" && pwd)/$(basename "$LIBRARY")"
  ARTDIR="$(dirname "$BINARY")"
  WORK="${WORK:-$(mktemp -d)}"
  mkdir -p "$WORK"

  DOCKERFILE="$HERE/rig/Dockerfile.$DISTRO"
  IMAGE_HASH="$(sha256sum "$DOCKERFILE" | cut -c1-12)"
  IMAGE="pagespeed-rig:${DISTRO}-${IMAGE_HASH}"
  if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building rig image $IMAGE"
    docker build -q -t "$IMAGE" -f "$DOCKERFILE" "$HERE/rig" >/dev/null
  fi

  # ---------------------------------------------------------------------
  # P8: the daemon UNDER ITS OWN UNIT.
  #
  # Everything above runs the binary directly, as root, outside systemd --
  # which is the wrong process for a profile that systemd installs.  Under
  # the unit the daemon runs as User=pagespeed with the whole sandbox in
  # force (ProtectSystem=strict, PrivateTmp, ProtectProc, RuntimeDirectory,
  # the address-family restriction), and those change which syscalls glibc
  # and libuv actually take.  strace goes in as the unit's own ExecStart via
  # a drop-in, so startup is traced too, not just the steady state.
  if [[ "${CENSUS_SKIP_UNIT:-0}" != "1" ]]; then
    echo "== booting a systemd container for the under-unit posture (P8)"
    UNIT_CTR="census-systemd-${DISTRO}-$$"
    if docker run -d --name "$UNIT_CTR" --privileged --cgroupns=host \
         --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
         -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock \
         --tmpfs /tmp -e container=docker \
         -v "$REPO:/repo:ro" -v "$WORK:/work" -v "$ARTDIR:/artifacts:ro" \
         "$IMAGE" /sbin/init >/dev/null 2>&1; then
      ready=""
      for _ in $(seq 1 45); do
        case "$(docker exec "$UNIT_CTR" systemctl is-system-running 2>&1 || true)" in
          running|degraded) ready=1; break ;;
        esac
        sleep 2
      done
      if [[ -n "$ready" ]]; then
        docker exec -e CENSUS_DISTRO="$DISTRO" "$UNIT_CTR" \
          bash /repo/tools/packaging/syscall-census.sh --unit-posture \
            --binary /artifacts/"$(basename "$BINARY")" \
            --library /artifacts/"$(basename "$LIBRARY")" --work /work \
          2>&1 | sed 's/^/   /' || echo "   WARNING: P8 phase failed"
      else
        echo "   WARNING: P8 skipped -- systemd never came up in the container"
      fi
    else
      echo "   WARNING: P8 skipped -- could not start a booted-systemd container"
    fi
    docker rm -f "$UNIT_CTR" >/dev/null 2>&1 || true
  fi


  # strace needs ptrace, and the container's own seccomp profile would
  # otherwise deny PTRACE_ATTACH; --security-opt seccomp=unconfined is the
  # point of this lane (we are here to observe the daemon's calls, not to
  # confine it).
  docker run --rm --init --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
    -v "$REPO:/repo:ro" -v "$WORK:/work" -v "$ARTDIR:/artifacts:ro" \
    -w /repo -e CENSUS_DISTRO="$DISTRO" "$IMAGE" \
    bash /repo/tools/packaging/syscall-census.sh --in-container \
      --binary /artifacts/"$(basename "$BINARY")" \
      --library /artifacts/"$(basename "$LIBRARY")" --work /work
  # Everything under the work dir was written by root inside the container
  # (the daemon's cache files and sockets included); hand it back so the
  # caller can read, re-run and clean up without root.
  docker run --rm -v "$WORK:/work" "$IMAGE" \
    chown -R "$(id -u):$(id -g)" /work >/dev/null 2>&1 || true
  echo "census artefacts in $WORK"
  exit 0
fi

# ------------------------------------------------- P8: under the unit ----
# Runs INSIDE a booted-systemd container. Builds and installs the package,
# replaces the unit's ExecStart with the same command under strace (so
# startup is traced, not just the steady state), drives real API traffic,
# and extracts the names. The daemon here is User=pagespeed with the whole
# shipped sandbox applied by systemd -- the process the profile is actually
# about.
if [[ "$UNIT_POSTURE" -eq 1 ]]; then
  DISTRO="${CENSUS_DISTRO:-unknown}"
  WORK="${WORK:-/work}"
  PKG=pagespeed-optimizer
  OUT="$WORK/census-${DISTRO}-unit.txt"
  : > "$OUT"
  echo "distro=${DISTRO} arch=$(uname -m) posture=P8-under-unit" | tee -a "$OUT"

  FLAV=deb; command -v dpkg >/dev/null 2>&1 || FLAV=rpm
  mkdir -p "$WORK/unitpkg"
  bash /repo/tools/packaging/build-optimizer-$FLAV.sh --version 0.0.0~census \
    --binary "$BINARY" --library "$LIBRARY" --out "$WORK/unitpkg" >/dev/null 2>&1 || {
      echo "   WARNING: P8 could not build the package" >&2; exit 0; }
  PKGFILE="$(find "$WORK/unitpkg" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) | head -1)"
  [[ -n "$PKGFILE" ]] || { echo "   WARNING: P8 produced no package" >&2; exit 0; }
  if [[ "$PKGFILE" == *.deb ]]; then dpkg -i "$PKGFILE" >/dev/null 2>&1; else rpm -i "$PKGFILE" >/dev/null 2>&1; fi

  D=/etc/systemd/system/$PKG.service.d
  mkdir -p "$D"
  # strace as ExecStart: same argv the unit would have run, wrapped. The
  # daemon still runs as User=pagespeed under every shipped directive --
  # except the syscall ALLOW-LIST, which is reset here: since 2.1 the unit
  # enforces it, strace(1) needs ptrace(2) (outside @system-service), and a
  # census killed by the profile it exists to measure is no census. The
  # empty assignment clears SystemCallFilter= from the unit and from every
  # earlier-sorting drop-in; the `zz-` prefix is what makes this file sort
  # last so the reset wins. SystemCallLog= is untouched. What is measured is
  # therefore "every call the daemon makes under this unit", which is the
  # input the allow-list is decided from -- not the allow-list's verdict on
  # itself, which the rig's h6-* legs assert separately.
  # The trace CANNOT be written to /work: ProtectSystem=strict makes the whole
  # filesystem read-only to this unit apart from its ReadWritePaths and its
  # RuntimeDirectory, so strace -o there fails silently and the posture
  # produces nothing. The cache directory is the one place the daemon may
  # write; the log is copied out afterwards.
  TRACE_IN_UNIT=/var/cache/$PKG/v3/strace-P8.log
  cat > "$D/zz-census-strace.conf" <<EOF
[Service]
SystemCallFilter=
Environment="OPTIMIZER_OPTS=--api-socket"
ExecStart=
ExecStart=/usr/bin/strace -f -qq -e trace=all -o $TRACE_IN_UNIT \\
    /usr/bin/$PKG --cache-dir \${CACHE_DIR} --socket \${SOCKET_PATH} \\
    --cache-size \${CACHE_SIZE} --log-format json --log-level \${LOG_LEVEL} \$OPTIMIZER_OPTS
EOF
  systemctl daemon-reload
  systemctl reset-failed "$PKG.service" 2>/dev/null || true
  systemctl restart "$PKG.service" 2>/dev/null || true
  for _ in $(seq 1 60); do
    curl -s --max-time 3 --unix-socket /run/$PKG/api.sock \
      http://localhost/v1/health >/dev/null 2>&1 && break
    sleep 1
  done
  for path in /v1/health /v1/stats /v1/config /v1/cache/urls; do
    curl -s --max-time 5 --unix-socket /run/$PKG/api.sock "http://localhost$path" \
      >/dev/null 2>&1 || true
  done
  curl -s --max-time 5 -X POST -H 'Content-Type: application/json' \
    -d '{"url":"http://example.com/x.css"}' --unix-socket /run/$PKG/api.sock \
    http://localhost/v1/cache/purge >/dev/null 2>&1 || true
  systemctl stop "$PKG.service" 2>/dev/null || true
  sleep 2

  UNITLOG="$WORK/strace-${DISTRO}-P8.log"
  cp -f "$TRACE_IN_UNIT" "$UNITLOG" 2>/dev/null || true
  if [[ ! -s "$UNITLOG" ]]; then
    echo "   P8 diagnostics: active=$(systemctl is-active $PKG.service 2>/dev/null)" >&2
    systemctl status "$PKG.service" --no-pager -n 15 2>&1 | sed 's/^/     /' >&2 || true
  fi
  if [[ -s "$UNITLOG" ]]; then
    {
      echo
      echo "### posture P8 (under the shipped unit, User=pagespeed, API driven)"
      sed -e 's/^\[[^]]*\] //' -e 's/^[0-9][0-9]* *//' "$UNITLOG" \
        | grep -oE '^[a-z_][a-z0-9_]*\(' | tr -d '(' | sort -u
    } >> "$OUT"
    echo "   P8: $(grep -cE '^[a-z_]' "$OUT" || true) names, active=$(systemctl is-active $PKG.service 2>/dev/null)"
  else
    echo "   WARNING: P8 produced no strace output" >&2
  fi
  exit 0
fi

# ---------------------------------------------------------- in container ----
DISTRO="${CENSUS_DISTRO:-unknown}"
WORK="${WORK:-/work}"
mkdir -p "$WORK"
command -v strace >/dev/null 2>&1 || {
  echo "error: strace is not in this image" >&2; exit 2; }

OUT="$WORK/census-${DISTRO}.txt"
MD="$WORK/census-${DISTRO}.md"
: > "$OUT"

SYSTEMD_VER="$(systemctl --version 2>/dev/null | head -1 | awk '{print $2}')"
echo "distro=${DISTRO} systemd=${SYSTEMD_VER} arch=$(uname -m) kernel=$(uname -r)" \
  | tee -a "$OUT"

# Every posture writes its raw strace log here; names are extracted with one
# regex so a posture that dies still contributes what it reached.
names_of() { # strace-log -> sorted unique syscall names
  # strace -f prefixes each line with the pid; the call is the first token
  # before '(' on lines that are not signals, exits, or "unfinished" tails.
  sed -e 's/^\[[^]]*\] //' -e 's/^[0-9][0-9]* *//' "$1" \
    | grep -oE '^[a-z_][a-z0-9_]*\(' | tr -d '(' | sort -u
}

CACHE_ROOT="$WORK/census-cache"
RUN_ROOT="$WORK/census-run"

run_posture() { # name extra-daemon-args...
  local name="$1"; shift
  local log="$WORK/strace-${DISTRO}-${name}.log"
  local cdir="$CACHE_ROOT/$name/v3"
  local rdir="$RUN_ROOT/$name"
  rm -rf "${CACHE_ROOT:?}/${name:?}" "$rdir"
  mkdir -p "$cdir" "$rdir"

  echo "-- posture ${name}"
  # -f follows the thread pool and any child; -qq drops the attach/detach
  # chatter; -e trace=all is the default but stated so the intent is in the
  # command line and not in a default that could change.
  strace -f -qq -e trace=all -o "$log" \
    "$BINARY" --cache-dir "$cdir" --socket "$rdir/notify.sock" \
      --cache-size 16777216 --log-level warning "$@" \
      >"$WORK/daemon-${name}.log" 2>&1 &
  local strace_pid=$!

  # The signals below must reach the DAEMON, not strace: `pkill -f` matches
  # strace's own command line too (it contains the binary path), and SIGHUP
  # to strace ends the trace mid-posture.  strace's only child is the daemon.
  # `|| true`: pgrep exits 1 when the daemon has not been forked yet, and
  # under `set -e` an assignment carries the substitution's status -- so the
  # bare form aborted the whole census at whichever posture lost the race.
  local dpid=""
  for _ in $(seq 1 50); do
    dpid="$(pgrep -P "$strace_pid" 2>/dev/null | head -1 || true)"
    [[ -n "$dpid" ]] && break
    sleep 0.1
  done
  [[ -n "$dpid" ]] || echo "   WARNING: ${name}: never saw strace's child" >&2

  local up=""
  for _ in $(seq 1 150); do
    if [[ -S "$rdir/notify.sock" && -S "$rdir/notify.sock.health" && \
          -S "$rdir/notify.sock.mgmt" ]]; then up=1; break; fi
    sleep 0.1
  done
  if [[ -z "$up" ]]; then
    echo "   WARNING: ${name}: daemon never bound all three sockets" >&2
    sed -n '1,20p' "$WORK/daemon-${name}.log" >&2 || true
  fi

  # Drive REAL requests at the management API when this posture binds one.
  # Without this the daemon binds and then sits there: no connection is ever
  # accepted, so accept4/recvfrom/sendto never appear and the census silently
  # omits the entire serving half of the socket path.  (That omission was
  # real: the first artefact had no accept4 in any posture.)
  # An ARRAY, not a string. The string form was word-split by the shell with
  # its quotes intact, so `-H 'Authorization: Bearer x'` reached curl as four
  # bogus arguments and P3's requests went out unauthenticated -- every
  # authenticated handler answered 401 and its syscalls never entered the
  # census. Arrays keep one argument as one argument.
  if [[ ${#API_CURL[@]} -gt 0 ]]; then
    for path in /v1/health /v1/stats /v1/config /v1/cache/urls; do
      curl -s --max-time 5 "${API_CURL[@]}" "${API_BASE}${path}" >/dev/null 2>&1 || true
    done
    # A purge is the one API call that WRITES through the cache path.
    curl -s --max-time 5 "${API_CURL[@]}" -X POST \
      -H 'Content-Type: application/json' \
      -d '{"url":"http://example.com/x.css"}' \
      "${API_BASE}/v1/cache/purge" >/dev/null 2>&1 || true
    curl -s --max-time 5 "${API_CURL[@]}" -X POST \
      -H 'Content-Type: application/json' -d '{}' \
      "${API_BASE}/v1/cache/reprocess" >/dev/null 2>&1 || true
  fi

  # Touch the cache the way a serving-module peer does, so the DAEMON's side
  # of that interaction is in the trace.  Scope, stated plainly: this loads
  # the shipped client library and enumerates the volume; it does NOT drive a
  # full ps_cache_open handshake (that is the rig's peer-cache-open leg, which
  # asserts, whereas this only observes).  It is the daemon's syscalls that
  # the profile governs -- a peer runs in its own process under its own
  # filter -- so the census subject is the strace log, not this probe.
  LD_LIBRARY_PATH="$(dirname "$LIBRARY")" python3 - "$LIBRARY" "$cdir" <<'PYEOF' \
      >>"$WORK/daemon-${name}.log" 2>&1 || true
import ctypes, glob, os, sys
lib_path, cdir = sys.argv[1], sys.argv[2]
try:
    ctypes.CDLL(lib_path)
except OSError as e:
    print("peer probe: could not load the client library:", e); sys.exit(0)
vols = glob.glob(os.path.join(cdir, "cache-*"))
print("peer probe: volumes seen:", vols)
PYEOF

  # P7's extra: the reload signal before the shutdown.  The daemon ignores
  # SIGHUP by design (#1465: there is no reload semantics to trigger, and the
  # default disposition killed it), so the trace should run UNINTERRUPTED
  # through it; a posture that ends here regressed the fence.
  if [[ "$name" == P7 && -n "$dpid" ]]; then
    kill -HUP "$dpid" 2>/dev/null || true
    sleep 1
    kill -0 "$dpid" 2>/dev/null \
      || echo "   WARNING: P7: daemon died on SIGHUP -- the #1465 fence regressed" >&2
  fi

  # SIGTERM the daemon; strace exits once its tracee is gone.
  if [[ -n "$dpid" ]]; then kill -TERM "$dpid" 2>/dev/null || true; fi
  for _ in $(seq 1 50); do
    kill -0 "$strace_pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL "$strace_pid" 2>/dev/null || true
  wait "$strace_pid" 2>/dev/null || true

  {
    echo
    echo "### posture ${name} ($* )"
    names_of "$log"
  } >> "$OUT"
}

CENSUS_TOKEN=census-token-long-enough-to-pass
API_CURL=(); API_BASE=""
run_posture P1

# --api-socket's compiled-in default is /run/pagespeed-optimizer/api.sock,
# which only the package's RuntimeDirectory creates; name a writable path so
# the posture actually binds instead of refusing to start.
API_CURL=(--unix-socket "$RUN_ROOT/P2/api.sock"); API_BASE="http://localhost"
run_posture P2 --api-socket "$RUN_ROOT/P2/api.sock"

API_CURL=(-H "Authorization: Bearer $CENSUS_TOKEN"); API_BASE="http://127.0.0.1:19880"
run_posture P3 --api-port 19880 --api-bind 127.0.0.1 --api-token "$CENSUS_TOKEN"

API_CURL=(); API_BASE=""
run_posture P7

# P5/P6: the curl child.  It inherits the parent's filter, so its set is in
# scope even though the daemon-side trigger is not reachable in this image.
if command -v curl >/dev/null 2>&1; then
  echo "-- posture P5/P6 (curl child, standalone)"
  strace -f -qq -e trace=all -o "$WORK/strace-${DISTRO}-curl.log" \
    curl -s -o /dev/null --max-time 20 https://example.com/ \
    >/dev/null 2>&1 || true
  {
    echo
    echo "### posture P5/P6 (curl child, standalone)"
    names_of "$WORK/strace-${DISTRO}-curl.log"
  } >> "$OUT"
else
  { echo; echo "### posture P5/P6 (curl child): NOT MEASURED -- no curl in image"; } >> "$OUT"
fi

{ echo; echo "### posture P4 (browser analysis + Chrome): NOT MEASURED -- the rig image ships no Chrome"; } >> "$OUT"

# ------------------------------------------------- group intersection ----
# The union includes the under-unit posture (P8), which ran in a separate
# booted-systemd container before this one and left its names in the shared
# work dir. Leaving it out would intersect the profile against the postures
# that run the binary as root outside systemd -- the wrong process.
ALL="$WORK/all-${DISTRO}.txt"
cat "$OUT" "$WORK/census-${DISTRO}-unit.txt" 2>/dev/null \
  | grep -E '^[a-z_][a-z0-9_]*$' | sort -u > "$ALL"

GROUP="$WORK/system-service-${DISTRO}.txt"
# `systemd-analyze syscall-filter @g` prints the group header, an optional
# `# description` line, then one token per line -- and those tokens are a MIX
# of plain syscall names and NESTED GROUP names (@system-service is mostly a
# union of @aio, @basic-io, @default, @process, ...).  Expanding only the top
# level yields ~45 names instead of ~376 and makes every real member look
# like something the profile would kill, so the expansion has to recurse.
# SYSTEMD_COLORS=0 plus the escape-stripping sed: systemd 255 colourises this
# output even with no tty, so every member line arrives as `<ESC>[0m@aio` and a
# naive parse silently yields ~60 names instead of ~370 -- which then reports
# every syscall the daemon makes as one the profile would kill.
expand_group() {
  local g="$1" tok
  SYSTEMD_COLORS=0 systemd-analyze syscall-filter "$g" 2>/dev/null | tail -n +2 \
    | sed -e 's/\x1b\[[0-9;]*m//g' -e 's/^[[:space:]]*//' | while read -r tok; do
        case "$tok" in
          '#'*|'') ;;
          @*) expand_group "$tok" ;;
          *) printf '%s\n' "$tok" ;;
        esac
      done
}
expand_group @system-service | sort -u > "$GROUP" || true
for g in @privileged @resources; do
  expand_group "$g" | sort -u > "$WORK/${g#@}-${DISTRO}.txt" || true
done
# What the unit's enforcing profile actually allows: @system-service minus
# the two subtracted groups.  This -- not @system-service alone -- is the set
# to compare the census against.
ALLOWED="$WORK/allowed-${DISTRO}.txt"
cat "$WORK/privileged-${DISTRO}.txt" "$WORK/resources-${DISTRO}.txt" 2>/dev/null \
  | sort -u > "$WORK/subtracted-${DISTRO}.txt"
# Plus the names the unit admits one by one (mincore, for the cache's
# readahead residency check).
UNIT_NAMES="mincore"
{ comm -23 "$GROUP" "$WORK/subtracted-${DISTRO}.txt"; printf '%s\n' $UNIT_NAMES; } \
  | sort -u > "$ALLOWED"

{
  echo "# Syscall census -- ${DISTRO} (systemd ${SYSTEMD_VER}, $(uname -m), kernel $(uname -r))"
  echo
  echo "Observed distinct syscall names:                 $(wc -l < "$ALL")"
  echo "@system-service members on this systemd:         $(wc -l < "$GROUP")"
  echo "@privileged + @resources (subtracted):           $(wc -l < "$WORK/subtracted-${DISTRO}.txt")"
  echo "Effective allow-list of the enforcing profile:   $(wc -l < "$ALLOWED")"
  echo
  echo '## Verdict'
  echo
  local_denied="$(comm -23 "$ALL" "$ALLOWED" | tr '\n' ' ')"
  if [[ -z "${local_denied// /}" ]]; then
    echo 'EMPTY -- every call observed in every measured posture is admitted by'
    echo '`SystemCallFilter=@system-service mincore` + `~@privileged @resources`.'
    echo 'The enforcing profile needs no per-name additions beyond mincore on'
    echo 'this distro.'
  else
    echo 'The enforcing profile would KILL these observed calls; each one must'
    echo 'be explained or admitted by name -- the shipped unit enforces:'
    echo
    printf '    %s\n' $local_denied
  fi
  echo
  echo '## Load-bearing spot checks (the questions the design asked)'
  echo
  for s in io_uring_setup io_uring_enter io_uring_register unshare clone clone3 \
           membarrier sched_getaffinity swapon init_module; do
    if grep -qx "$s" "$ALLOWED"; then verdict="ADMITTED"; else verdict="DENIED  "; fi
    if grep -qx "$s" "$ALL"; then seen="observed"; else seen="not observed"; fi
    printf '    %-20s %s  (%s in the measured postures)\n' "$s" "$verdict" "$seen"
  done
  echo
  echo '## Observed AND admitted'
  echo
  comm -12 "$ALL" "$ALLOWED" | sed 's/^/    /'
  echo
  echo '## Observed and NOT admitted'
  echo
  comm -23 "$ALL" "$ALLOWED" | sed 's/^/    /'
} > "$MD"

echo
echo "=== ${DISTRO}: observed calls the enforcing profile would kill ==="
comm -23 "$ALL" "$ALLOWED"
echo "=== end ==="
echo "wrote $OUT and $MD"
