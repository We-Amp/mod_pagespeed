- **Redirects no longer pick up repeated PageSpeed query parameters.** When the
  module strips its own query parameters (for example `?PageSpeed=noscript`)
  and the origin answers with a same-site redirect, it adds them back to the
  `Location` only if the target does not already carry them, and before any
  `#fragment`. Redirects to another host are left untouched. Reported in
  https://github.com/apache/incubator-pagespeed-mod/issues/1622 and
  https://github.com/apache/incubator-pagespeed-mod/issues/1024.
