#!/bin/bash
# Container entry point: start the optimizer worker as its systemd unit would,
# optionally swap in a locally built module (MODULE_SO), then run the web
# server in the foreground.  usage: start-origin.sh apache|nginx
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
if [ -n "${MODULE_SO:-}" ]; then
  if [ "$SERVER" = apache ]; then
    cp "$MODULE_SO" /usr/lib/apache2/modules/mod_pagespeed.so
  else
    cp "$MODULE_SO" /usr/lib/nginx/modules/ngx_pagespeed_module.so
  fi
fi
su -s /bin/sh pagespeed -c 'umask 0007; /usr/bin/pagespeed-optimizer --cache-dir /var/cache/pagespeed-optimizer/v3 --socket /run/pagespeed-optimizer/notify.sock --cache-size 536870912 --log-level info > /tmp/worker.log 2>&1 &'
for _ in $(seq 1 100); do
  [ -S /run/pagespeed-optimizer/notify.sock ] && break
  sleep 0.1
done
[ -f /var/www/html/big.jpg ] || convert -size 2400x1600 plasma:fractal -quality 95 /var/www/html/big.jpg
if [ "$SERVER" = apache ]; then
  exec apache2ctl -D FOREGROUND
fi
cp /harness/site-nginx.conf /etc/nginx/sites-enabled/default
exec nginx -g 'daemon off;'
