#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Proves the documented recipe for the nginx Docker image with a locally built
# package: build the image from clean, run it, and make a real request.
# Usage: check-nginx-org-image.sh <image tag, e.g. nginx:1.30.6> <nginx-module-pagespeed-nginxorg .deb>
set -euo pipefail
# shellcheck source=tools/ci/docker_pull_retry.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/ci/docker_pull_retry.sh"
IMAGE="${1:?image}"; DEB="${2:?deb}"
TAG="pagespeed-nginxorg-check-$$"; NAME="${TAG}-run"
CTX="$(mktemp -d)"
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; docker image rm -f "$TAG" >/dev/null 2>&1 || true; rm -rf "$CTX"; }
trap cleanup EXIT
cp "$DEB" "$CTX/module.deb"
docker_pull_retry "$IMAGE" || exit 1
depends="$(dpkg-deb -f "$CTX/module.deb" Depends 2>/dev/null || docker run --rm -v "$CTX":/c "$IMAGE" dpkg-deb -f /c/module.deb Depends)"
echo "Depends: $depends"
case "$depends" in
  *"nginx (>= ${IMAGE##*:}), nginx (<< "*) ;;
  *) echo "FAIL: the package does not pin the image's nginx ${IMAGE##*:}"; exit 1 ;;
esac
cat > "$CTX/ps.conf" <<'CONF'
server {
  listen 8080;
  root /usr/share/nginx/html;
  pagespeed on;
  pagespeed FileCachePath /var/cache/ngx_pagespeed;
  pagespeed EnableFilters collapse_whitespace,rewrite_css,inline_css;
  location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" { add_header "" ""; }
}
CONF
cat > "$CTX/Dockerfile" <<DOCKER
FROM ${IMAGE}
COPY module.deb /tmp/module.deb
RUN dpkg-query -W -f='\${Version}' nginx > /tmp/nginx-before
RUN apt-get update && apt-get install -y curl /tmp/module.deb \\
 && sed -i '1i include /etc/nginx/modules-enabled/*.conf;' /etc/nginx/nginx.conf \\
 && mkdir -p /var/cache/ngx_pagespeed && chown nginx /var/cache/ngx_pagespeed
RUN test "\$(dpkg-query -W -f='\${Version}' nginx)" = "\$(cat /tmp/nginx-before)"
COPY ps.conf /etc/nginx/conf.d/ps.conf
RUN printf '<html><head><link rel=stylesheet href=/a.css></head><body>\n\n\n  <p>hello</p>\n\n  </body></html>' > /usr/share/nginx/html/t.html \\
 && printf 'body {  color : red ;   margin : 0 ; }\n' > /usr/share/nginx/html/a.css
DOCKER
docker build -q -t "$TAG" "$CTX" >/dev/null
# local-image: built above
docker run --rm "$TAG" nginx -t 2>&1 | tail -n 1
# local-image: built above
docker run -d --name "$NAME" "$TAG" >/dev/null
for _ in $(seq 1 20); do
  docker exec "$NAME" sh -c 'exec 3<>/dev/tcp/127.0.0.1/8080' 2>/dev/null && break
  docker exec "$NAME" bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080' 2>/dev/null && break
  sleep 1
done
fetch() { docker exec "$NAME" curl -s "$@"; }
for _ in 1 2 3 4 5 6; do fetch -o /dev/null http://localhost:8080/t.html; sleep 2; done
hdr="$(fetch -D- -o /dev/null http://localhost:8080/t.html | tr -d '\r')"
body="$(fetch http://localhost:8080/t.html)"
echo "$hdr" | grep -iE '^HTTP|^x-page-speed' || true
echo "$body" | head -c 300; echo
echo "$hdr" | grep -qiE '^x-page-speed: ' || { echo "FAIL: no X-Page-Speed header"; exit 1; }
echo "$body" | grep -q '<p>hello</p>' || { echo "FAIL: page body lost"; exit 1; }
if echo "$body" | grep -q 'a.css'; then echo "FAIL: the stylesheet was not rewritten"; exit 1; fi
echo "$body" | grep -q 'body{color:red;margin:0}' || { echo "FAIL: no minified inline stylesheet"; exit 1; }
if docker logs "$NAME" 2>&1 | grep -E '\[(alert|emerg|crit)\]'; then echo "FAIL: alerts in the log"; exit 1; fi
# Removing the package must leave nginx able to start.
out="$(docker exec "$NAME" sh -ec 'rm -f /etc/nginx/conf.d/ps.conf; dpkg -r nginx-module-pagespeed-nginxorg; test ! -e /usr/lib/nginx/modules/ngx_pagespeed_module.so; nginx -t' 2>&1)" || { echo "FAIL: removing the package"; echo "$out"; exit 1; }
echo "removal: nginx -t clean"
echo "image=$IMAGE nginx=$(docker exec "$NAME" nginx -v 2>&1 | cut -d/ -f2)"
echo "PASS"
