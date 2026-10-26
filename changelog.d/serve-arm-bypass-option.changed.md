- **A client reload no longer bypasses the optimizer's cached copy of a
  resource unless you ask for it.** Before, on Apache, nginx and IIS, a
  request with `Cache-Control: no-cache` or `Pragma: no-cache` was sent to
  the origin instead of being served from the optimizer's cache (Apache and
  IIS missed the capitalised `No-Cache`), and `Cache-Control: max-age=0`
  never was. Now all of these are served from the cache by default. The
  new option `DaemonServeHonorRequestFreshness` (Apache:
  `ModPagespeedDaemonServeHonorRequestFreshness`; nginx:
  `pagespeed DaemonServeHonorRequestFreshness on;`; IIS:
  `pagespeed DaemonServeHonorRequestFreshness on`; per server or site, default off)
  turns the bypass back on: with it on, `Cache-Control: no-cache` in any
  letter case, `Cache-Control: max-age=0` and `Pragma: no-cache` (only when
  the request carries no `Cache-Control`) go to the origin on every port.
