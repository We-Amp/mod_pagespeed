#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Console verification rig: Apache plus the SHIPPED mod_pagespeed and
# pagespeed-optimizer packages in a booted-systemd Ubuntu 24.04 container, with
# the optimizer's management API socket enabled so the console's optimizer
# panels have data. Two named vhosts (site-a.test on :80, site-b.test on :81) so per-vhost and
# whole-server answers differ.
#
#   rig.sh up [--version V]   build image, boot, install, configure, warm
#   rig.sh warm               self-checking warm traffic on both vhosts
#   rig.sh workload [K]       controlled workload with client-side expected
#                             values; writes workload/expected.json
#   rig.sh capture [DIR]      save the real JSON of every console leaf
#                             (default: pagespeed/system/console/test-fixtures/rig)
#   rig.sh test [--daemon-down]  assert the console surface answers as expected
#   rig.sh daemon stop|start  toggle the optimizer (the "optimizer absent" state)
#   rig.sh restart-web        restart Apache (module windows reset)
#   rig.sh status | urls | down
#
# Environment: RIG_VERSION (default: the version in install/debian/optimizer-pin.json),
# RIG_CTR (container name), RIG_PORT_A / RIG_PORT_B (host ports, 127.0.0.1 only),
# RIG_PLATFORM (default linux/amd64), RIG_WORK (package and log cache).
#
# Runs wherever the docker CLI points (DOCKER_HOST=ssh://... is fine: packages
# and fixtures travel by `docker cp`, never bind mounts). Ports are published
# on 127.0.0.1 of the docker host only. `down` removes only this rig's container.
set -euo pipefail
CMD="${1:-}"; shift || true
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_ROOT/tools/console-audit"
pin_version() {
  sed -n 's/.*"tag": *"optimizer-v\([0-9][0-9.]*\)".*/\1/p' "$REPO_ROOT/install/debian/optimizer-pin.json" | head -1
}
VERSION="${RIG_VERSION:-$(pin_version)}"
CTR="${RIG_CTR:-console-audit-rig}"
PORT_A="${RIG_PORT_A:-18090}"; PORT_B="${RIG_PORT_B:-18091}"
PLATFORM="${RIG_PLATFORM:-linux/amd64}"
ARCH="${PLATFORM#linux/}"   # package architecture: amd64 or arm64
WORK="${RIG_WORK:-$HOME/.cache/console-audit-rig}"
MODULE_REPO="${RIG_MODULE_REPO:-We-Amp/mod_pagespeed}"
OPTIMIZER_REPO="${RIG_OPTIMIZER_REPO:-We-Amp/pagespeed-optimizer}"
IMAGE="${RIG_IMAGE:-console-audit-rig:ubuntu2404}"
DAEMON_DOWN=0
DAEMON_ACTION=""
POSARGS=()
while [[ $# -gt 0 ]]; do case "$1" in
  --version) VERSION="$2"; shift 2 ;;
  --daemon-down) DAEMON_DOWN=1; shift ;;
  stop|start) DAEMON_ACTION="$1"; shift ;;
  *) POSARGS+=("$1"); shift ;;
esac; done
[[ -n "$VERSION" ]] || { echo "rig: no version (set RIG_VERSION)" >&2; exit 1; }

in_ctr() { docker exec "$CTR" bash -lc "$*"; }
die() { echo "rig: $*" >&2; exit 1; }
# curl runs INSIDE the container so this works over a remote DOCKER_HOST too.
probe() { in_ctr "curl -s -o /dev/null -w '%{http_code} %{content_type}' '$1'"; }
# Same, but from a SIBLING container on the bridge, so the request arrives from
# a source Apache does not consider `local` (the shipped pagespeed.conf gates
# the admin paths with `Require local`).
probe_remote() {
  local ip; ip="$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CTR")"
  docker run --rm --platform "$PLATFORM" "$IMAGE" curl -s -o /dev/null -w '%{http_code} %{content_type}' "http://$ip$1"
}

fetch_packages() {
  mkdir -p "$WORK/pkgs"
  [[ -f "$WORK/pkgs/pagespeed-optimizer_${VERSION}_${ARCH}.deb" ]] || \
    gh release download "optimizer-v${VERSION}" -R "$OPTIMIZER_REPO" -D "$WORK/pkgs" \
      -p "pagespeed-optimizer_${VERSION}_${ARCH}.deb" --clobber
  ls "$WORK"/pkgs/mod-pagespeed_"${VERSION}"-r*_${ARCH}.deb >/dev/null 2>&1 || \
    gh release download "module-v${VERSION}" -R "$MODULE_REPO" -D "$WORK/pkgs" \
      -p "mod-pagespeed_${VERSION}-r*_${ARCH}.deb" --clobber
  ls -1 "$WORK/pkgs"
}

build_image() {
  mkdir -p "$WORK/image"
  cat > "$WORK/image/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN rm -f /etc/dpkg/dpkg.cfg.d/excludes
RUN apt-get -qq update \
    && apt-get -qq -y install --no-install-recommends \
        systemd systemd-sysv procps apache2 curl ca-certificates gnupg python3 file \
    && rm -rf /var/lib/apt/lists/*
# A booted systemd in a privileged container shares the host's binfmt_misc
# table; mask the units that would register or unregister entries in it.
RUN for u in systemd-binfmt.service proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount; do \
        ln -sf /dev/null "/etc/systemd/system/$u"; done
CMD ["/sbin/init"]
DOCKERFILE
  docker build -q --platform "$PLATFORM" -t "$IMAGE" "$WORK/image" >/dev/null
}

boot() {
  docker rm -f "$CTR" >/dev/null 2>&1 || true
  docker run -d --name "$CTR" --platform "$PLATFORM" --privileged --cgroupns=host \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock --tmpfs /tmp \
    -e container=docker -p "127.0.0.1:${PORT_A}:80" -p "127.0.0.1:${PORT_B}:81" \
    "$IMAGE" /sbin/init >/dev/null
  local st
  for _ in $(seq 1 60); do
    st="$(docker exec "$CTR" systemctl is-system-running 2>/dev/null || true)"
    case "$st" in running|degraded) return 0 ;; esac; sleep 2
  done
  die "systemd never reached running/degraded"
}

install_packages() {
  docker cp "$WORK/pkgs" "$CTR:/pkgs"
  # Version-pinned globs: the package cache may hold more than one version.
  in_ctr "apt-get -qq update && apt-get -qq -y install /pkgs/pagespeed-optimizer_${VERSION}_*.deb /pkgs/mod-pagespeed_${VERSION}-r*_*.deb" >"$WORK/install.log" 2>&1 \
    || { tail -30 "$WORK/install.log"; die "package install failed"; }
  # The management API is opt-in; the console's optimizer panels need it.
  in_ctr "test -f /etc/default/pagespeed-optimizer || install -m 0640 -o root -g pagespeed /dev/null /etc/default/pagespeed-optimizer;
          grep -q '^OPTIMIZER_OPTS=' /etc/default/pagespeed-optimizer \
            && sed -i 's/^OPTIMIZER_OPTS=.*/OPTIMIZER_OPTS=--api-socket/' /etc/default/pagespeed-optimizer \
            || echo 'OPTIMIZER_OPTS=--api-socket' >> /etc/default/pagespeed-optimizer;
          systemctl restart pagespeed-optimizer"
}

configure_apache() {
  # The repo's example site, twice (one copy per vhost), at the SAME /example/
  # path on both so one warm loop and one probe list serve both.
  docker cp "$REPO_ROOT/install/mod_pagespeed_example" "$CTR:/var/www/html/example"
  docker cp "$HERE/workload.py" "$CTR:/usr/local/bin/console-audit-workload.py"
  in_ctr "mkdir -p /var/www/site-b && cp -r /var/www/html/example /var/www/site-b/example && chmod -R a+rX /var/www/html/example /var/www/site-b"
  # The controlled-workload assets, generated deterministically on both vhosts.
  in_ctr "python3 /usr/local/bin/console-audit-workload.py gen --images /var/www/html/example/images"
  in_ctr "cat > /etc/apache2/conf-available/console-audit.conf <<'CONF'
# Console verification rig: per-vhost statistics, in-place optimization and a
# two named vhosts (site-a.test on :80, site-b.test on :81). Throwaway container only.
ModPagespeedUsePerVHostStatistics on
ModPagespeedMessageBufferSize 200000
ModPagespeedInPlaceResourceOptimization on
# The shipped pagespeed.conf gates the admin paths with \"Require local\"; a
# browser through a tunnel is not local, so open them on this throwaway rig.
<Location /pagespeed_admin>
    Require all granted
</Location>
<Location /pagespeed_global_admin>
    Require all granted
</Location>
<VirtualHost *:80>
    ServerName site-a.test
    DocumentRoot /var/www/html
    <Directory /var/www/html>
        Require all granted
        Header set Cache-Control \"max-age=600\"
    </Directory>
    <Location /pagespeed_admin>
        Require all granted
        SetHandler pagespeed_admin
    </Location>
    <Location /pagespeed_global_admin>
        Require all granted
        SetHandler pagespeed_global_admin
    </Location>
</VirtualHost>
Listen 81
<VirtualHost *:81>
    ServerName site-b.test
    DocumentRoot /var/www/site-b
    <Directory /var/www/site-b>
        Require all granted
        Header set Cache-Control \"max-age=600\"
    </Directory>
    <Location /pagespeed_admin>
        Require all granted
        SetHandler pagespeed_admin
    </Location>
    <Location /pagespeed_global_admin>
        Require all granted
        SetHandler pagespeed_global_admin
    </Location>
</VirtualHost>
CONF
  a2enmod -q headers pagespeed && a2enconf -q console-audit && apache2ctl configtest && systemctl restart apache2"
}

warm() {
  # Real traffic so statistics, histograms, message history and the optimizer
  # cache have content. Self-checking: a warm URL that does not answer 200 on
  # BOTH vhosts is a rig bug, not warmth.
  in_ctr "bad=0; for i in \$(seq 1 8); do for p in / /images/Puzzle.jpg /styles/all_styles.css /rewrite_javascript.html /rewrite_images.html /collapse_whitespace.html; do
            for u in http://localhost/example\$p http://localhost:81/example\$p; do
              c=\$(curl -s -o /dev/null -w '%{http_code}' \"\$u\"); [ \"\$c\" = 200 ] || { echo \"warm: \$u -> \$c\"; bad=1; }; done; done; sleep 1; done; exit \$bad" \
    || die "warm traffic did not reach both vhosts (see the lines above)"
}

case "$CMD" in
  up)
    fetch_packages; build_image; boot; install_packages; configure_apache; warm
    "$0" test
    "$0" urls ;;
  warm) warm; echo "rig: warm OK (both vhosts answered 200 for every fixture URL)" ;;
  workload)
    docker cp "$HERE/workload.py" "$CTR:/usr/local/bin/console-audit-workload.py"
    in_ctr "python3 /usr/local/bin/console-audit-workload.py run --rounds ${POSARGS[0]:-3}"
    mkdir -p "$HERE/workload"
    docker cp "$CTR:/var/lib/console-audit/expected.json" "$HERE/workload/expected.json"
    echo "rig: wrote $HERE/workload/expected.json" ;;
  capture)
    OUT="${POSARGS[0]:-$REPO_ROOT/pagespeed/system/console/test-fixtures/rig}"
    mkdir -p "$OUT"
    docker cp "$HERE/workload.py" "$CTR:/usr/local/bin/console-audit-workload.py"
    in_ctr "python3 /usr/local/bin/console-audit-workload.py capture /var/lib/console-audit/leaves"
    if [[ ! -f "$HERE/workload/expected.json" && ! -f "$OUT/expected.json" ]]; then
      die "no expected.json: run 'rig.sh workload' before 'rig.sh capture'"
    fi
    # Keep the committed expected.json unless a fresh workload produced a new one.
    KEEP="$(mktemp)"; [[ -f "$OUT/expected.json" ]] && cp "$OUT/expected.json" "$KEEP"
    rm -f "$OUT"/*.json "$OUT"/*.txt
    docker cp "$CTR:/var/lib/console-audit/leaves/." "$OUT/"
    if [[ -f "$HERE/workload/expected.json" ]]; then cp "$HERE/workload/expected.json" "$OUT/expected.json"
    elif [[ -s "$KEEP" ]]; then cp "$KEEP" "$OUT/expected.json"; fi
    rm -f "$KEEP"
    echo "rig: captured $(ls "$OUT" | wc -l | tr -d ' ') files into $OUT" ;;
  test)
    a="$(probe http://localhost/pagespeed_admin/stats_json)"
    [[ "$a" == 200\ application/json* ]] || die "stats_json: $a"
    g="$(probe http://localhost/pagespeed_global_admin/stats_json)"
    [[ "$g" == 200\ application/json* ]] || die "global stats_json: $g"
    b="$(probe http://localhost:81/pagespeed_admin/stats_json)"
    [[ "$b" == 200\ application/json* ]] || die "site-b stats_json: $b"
    spa="$(in_ctr "curl -s http://localhost/pagespeed_admin/ | grep -c '<title>'")"
    [[ "$spa" -ge 1 ]] || die "SPA root did not serve an HTML document"
    h="$(probe http://localhost/pagespeed_admin/v1/daemon/health)"
    if [[ "$DAEMON_DOWN" == 1 ]]; then
      [[ "$h" == 502* ]] || die "optimizer down: expected 502, got $h"
    else
      [[ "$h" == 200\ application/json* ]] || die "optimizer health: $h"
    fi
    nf="$(probe http://localhost/pagespeed_admin/v1/daemon/config)"
    [[ "$nf" == 404* ]] || die "proxy allow-list: v1/daemon/config should be 404, got $nf"
    r="$(probe_remote /pagespeed_admin/stats_json)"
    [[ "$r" == 200\ application/json* ]] || die "remote-source stats_json: $r (admin paths still gated to local?)"
    for port in 80 81; do
      xm="$(in_ctr "curl -sI http://localhost:$port/example/rewrite_css.html | grep -ci '^X-Mod-Pagespeed:' || true")"
      [[ "$xm" -ge 1 ]] || die "vhost on :$port does not serve /example/ through the module (no X-Mod-Pagespeed)"
    done
    echo "rig: OK (daemon_down=$DAEMON_DOWN)" ;;
  daemon)
    in_ctr "systemctl ${DAEMON_ACTION:?stop|start} pagespeed-optimizer"; sleep 2
    in_ctr "systemctl is-active pagespeed-optimizer || true" ;;
  restart-web) in_ctr "systemctl restart apache2"; sleep 2; in_ctr "systemctl is-active apache2" ;;
  status)
    docker ps --filter "name=^${CTR}\$" --format '{{.Names}} {{.Status}} {{.Ports}}'
    in_ctr "systemctl is-active apache2 pagespeed-optimizer" || true ;;
  urls)
    echo "on the docker host: http://127.0.0.1:${PORT_A}/pagespeed_admin/  http://127.0.0.1:${PORT_A}/pagespeed_global_admin/  http://127.0.0.1:${PORT_B}/pagespeed_admin/" ;;
  down) docker rm -f "$CTR" >/dev/null 2>&1 || true; echo "rig: removed $CTR (if it existed)" ;;
  *) sed -n '3,25p' "$0"; exit 2 ;;
esac
