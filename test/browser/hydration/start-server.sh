#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
# Container entry point for run_module_check.sh: the worker as its systemd unit
# would start it, the module under test (MODULE_SO, or the installed package's
# own when unset), the fixtures as the document root, and
# DetectHydratedPages as DETECT says, in the server-wide configuration.
set -e
SERVER=$1
getent group pagespeed >/dev/null || groupadd -r pagespeed
id pagespeed >/dev/null 2>&1 || useradd -r -g pagespeed -d /nonexistent -s /usr/sbin/nologin pagespeed
usermod -a -G pagespeed www-data
mkdir -p /run/pagespeed-optimizer /var/cache/pagespeed-optimizer/v3 /var/cache/ngx_pagespeed
chown pagespeed:pagespeed /run/pagespeed-optimizer /var/cache/pagespeed-optimizer /var/cache/pagespeed-optimizer/v3
chown www-data /var/cache/ngx_pagespeed
chmod 0750 /run/pagespeed-optimizer
chmod 2770 /var/cache/pagespeed-optimizer/v3
su -s /bin/sh pagespeed -c 'umask 0007; /usr/bin/pagespeed-optimizer --cache-dir /var/cache/pagespeed-optimizer/v3 --socket /run/pagespeed-optimizer/notify.sock --cache-size 536870912 --log-level info > /tmp/worker.log 2>&1 &'
for _ in $(seq 1 100); do
  [ -S /run/pagespeed-optimizer/notify.sock ] && break
  sleep 0.1
done
if [ "$SERVER" = apache ]; then
  [ -z "${MODULE_SO:-}" ] || cp "$MODULE_SO" /usr/lib/apache2/modules/mod_pagespeed.so
  cat > /etc/apache2/conf-enabled/zz-hydration.conf <<CONF
ModPagespeedDetectHydratedPages $DETECT
ModPagespeedLoadFromFile http://$HOST_ALIAS/ /fixtures/
<Directory /fixtures>
  Require all granted
</Directory>
DocumentRoot /fixtures
CONF
  sed -i 's#DocumentRoot .*#DocumentRoot /fixtures#' /etc/apache2/sites-enabled/000-default.conf
  exec apache2ctl -D FOREGROUND
fi
[ -z "${MODULE_SO:-}" ] || cp "$MODULE_SO" /usr/lib/nginx/modules/ngx_pagespeed_module.so
cat > /etc/nginx/conf.d/zz-hydration.conf <<CONF
pagespeed DetectHydratedPages $DETECT;
CONF
cat > /etc/nginx/sites-enabled/default <<CONF
server {
    listen 80 default_server;
    root /fixtures;
    pagespeed on;
    pagespeed FileCachePath /var/cache/ngx_pagespeed;
    pagespeed LoadFromFile http://$HOST_ALIAS/ /fixtures/;
    pagespeed DaemonSocketPath /run/pagespeed-optimizer/notify.sock;
    pagespeed DaemonVolumePath /var/cache/pagespeed-optimizer/v3/cache;
    location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" { add_header "" ""; }
    location ~ "^/pagespeed_static/" { }
}
CONF
exec nginx -g 'daemon off;'
