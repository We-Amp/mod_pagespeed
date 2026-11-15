- The comment in the nginx module's load snippet
  (`/etc/nginx/modules-enabled/50-mod-pagespeed.conf`) no longer says the file
  is included by "stock noble nginx" on every Debian and Ubuntu release. It
  now says a distribution's nginx includes it, and that nginx.org builds need
  `include /etc/nginx/modules-enabled/*.conf;` in `nginx.conf`. Comment only;
  behaviour is unchanged.
