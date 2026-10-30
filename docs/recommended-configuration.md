# Recommended Configuration

The configuration guide now lives in the canonical product documentation on
modpagespeed.com:

- [Filter selection](https://modpagespeed.com/docs/filter-selection/) — rewrite levels (CoreFilters is the default), and enabling or disabling individual filters
- [Configuration](https://modpagespeed.com/docs/configuration/) — directives and a baseline setup
- [Filter reference](https://modpagespeed.com/docs/filters/) — every filter and which level enables it

See also [install-nginx.md](install-nginx.md) and [install-apache.md](install-apache.md)
for module installation on each platform.

## Admin console and cache purge notes

- **`PurgeMethod`.** On Apache and nginx, when `EnableCachePurge` is on and
  `PurgeMethod` names a request method (for example `PURGE`), the module
  answers every request that uses that method as a cache purge, ahead of the
  admin access options (`StrictAdminAccess`, the per-handler access
  settings). On IIS the purge verb follows `InfoUrlsLocalOnly`; Envoy has no
  `PurgeMethod`. A URL ending in `*` purges the whole cache. Restrict the verb
  at the web server, for example with `<LimitExcept>` or `Require` in Apache,
  or `limit_except` in nginx, so only the hosts that should purge can send it.
- **Expensive console reads.** The graphs and console JSON, the configuration
  dump, the plain message history, cache lookups and the whole-server cache
  listing run one at a time per kind per virtual host in each server process.
  A request for one of these, other than a cache lookup, waits briefly for the
  running one; a request that cannot get its turn is answered with HTTP 429
  and `{"success":false,"error":"busy"}`. The graphs and console JSON keep
  `granularity` between 1000 ms and one day, `start_time` and `end_time`
  non-negative and at most a day ahead, and at most 64 names in `var_titles`.
- **Repeated log lines.** An identical info line is kept in the message history
  once a minute; the next one after that carries `(repeated N times)`
  (`(repeated 1 time)` for a single repeat). The server error log is not
  changed by this.
- **Non-loopback admin warning.** It is logged once per handler family in each
  server process. A server that runs several processes, or that recycles them
  (for example Apache with `MaxConnectionsPerChild`), logs it once in each new
  process, so it can appear again after a recycle. Restrict the admin paths at
  the web server to avoid it.
