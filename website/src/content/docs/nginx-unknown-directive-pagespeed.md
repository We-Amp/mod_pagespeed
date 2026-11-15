---
title: 'nginx: unknown directive pagespeed after Plesk 18.0.79'
seoTitle: 'nginx: unknown directive "pagespeed" after a Plesk update'
description: 'Plesk Obsidian 18.0.79 removed ngx_pagespeed from its nginx. How to get nginx starting again, and the options for PageSpeed afterwards.'
order: 13.5
group: 'Upgrade and migrate'
datePublished: 2026-11-15
lastUpdated: 2026-11-15
faq:
  - q: 'Why does nginx report unknown directive "pagespeed" after updating Plesk?'
    a: 'Plesk Obsidian 18.0.79 (23 June 2026) removed the ngx_pagespeed module from its nginx. nginx stops with an unknown directive error when a configuration still contains pagespeed lines and the module that defines them is gone. Remove or comment out those lines.'
  - q: 'Does mod_pagespeed support the nginx that Plesk ships?'
    a: 'No. We have not documented or tested our module for Plesk''s own nginx package, and we make no support claim for it.'
---

If nginx stopped starting or reloading after a Plesk update, with a message like
`unknown directive "pagespeed"`, this page explains what changed and how to get
nginx running again.

## What changed

Plesk's release notes record the change in two steps:

- Plesk Obsidian 18.0.77 (31 March 2026) announced that support for the
  `ngx_pagespeed.so` module would be deprecated and removed from the `sw-nginx`
  package.
- Plesk Obsidian 18.0.79 (23 June 2026) removed the module from nginx. Plesk's
  announcement gives the reason as a security vulnerability in the PSOL library
  (libwebp).

Sources: the [Plesk Obsidian change log](https://docs.plesk.com/release-notes/obsidian/change-log/)
and the [18.0.79 announcement](https://www.plesk.com/blog/plesk-news-announcements/plesk-obsidian-18-0-79/).

The update removes the module but not your configuration. Any `pagespeed`
directive left in an nginx configuration now refers to a module that is no
longer loaded, and nginx refuses a configuration that contains a directive it
does not know. That is why the whole server fails to start or reload, not only
the sites that used PageSpeed.

## Get nginx running again

1. Find every place that still refers to the module:

   ```bash
   sudo grep -rn "pagespeed" /etc/nginx
   ```

   Directory layout differs between installations, and a panel may keep
   per-domain configuration elsewhere. Widen the search to wherever your
   configuration lives. Per-domain directives are often visible only in the
   panel, so check there too.

2. Remove or comment out what you find:
   - every `pagespeed ...;` directive, not only `pagespeed on;`. Directives
     such as `pagespeed FileCachePath` fail the same way.
   - any `load_module` line that points at a missing module file.

   Remove them where they were entered. A panel can generate parts of the
   web server configuration, so make the change in the panel's per-domain nginx
   settings rather than in generated files, and in any custom include file you
   created yourself. See Plesk's own documentation for how your version handles
   custom nginx directives.

3. Test the configuration, then reload nginx:

   ```bash
   sudo nginx -t
   sudo systemctl reload nginx
   ```

   If your host has more than one nginx, run `nginx -t` with the binary your
   server actually uses.

Your sites serve normally again, without PageSpeed optimization.

## Options going forward

These are independent choices. None of them is required to get nginx running.

**Do without it.** Leave the lines out. Pages are served unoptimized.

**Use mod_pagespeed with Apache.** The maintained
[mod_pagespeed](/docs/installation-module/) has an Apache module. Our
documentation covers stock Apache on Debian, Ubuntu and the RHEL family. It
does not cover Apache as installed and managed by Plesk, and we have not tested
it there, so we cannot say whether it works in that setup. If you try it, treat
it as unverified and test on a staging domain first. We would like to hear how it goes through the
[contact page](/contact/).

**Run nginx with our module outside Plesk's nginx.** The
[nginx module](/docs/installation-module/#native-nginx-module) loads into the
nginx that your distribution ships. For nginx.org's stable nginx there is a
separate package, `nginx-module-pagespeed-nginxorg`, built for one nginx
version at a time (currently 1.30.6), for amd64 on Debian 13, Ubuntu 24.04 and
Enterprise Linux 9. It cannot be installed together with the distribution
package. The module does not load into a different nginx build, and we have
not documented or tested it for the `sw-nginx` package. We make no claim that
it works there. Running our module means running one of those supported nginx
builds yourself, outside the panel's nginx, and you own that setup and its
upgrades.

If you are replacing another ngx_pagespeed build, see
[Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/): your
`pagespeed` directives carry over to the maintained module, except seven
deprecated names: 2.8.2 refuses them, so delete them before you install it. The
next release accepts them with a deprecation warning.

Plesk is a trademark of its owner. This page is not affiliated with or endorsed
by Plesk.
