# Production Deployment Checklist

Pre-flight checks before deploying mod_pagespeed in production.

## Cache Configuration (Critical)

- [ ] Both nginx and worker open cache with `enable_mmap_directory = true`
  - Nginx: `pagespeed_enable_mmap_directory on;`
  - Worker: `--enable-mmap-directory` (enabled by default)
  - Without this, worker writes are invisible to nginx (silent failure)
- [ ] Worker runs unprivileged as `pagespeed:pagespeed` (packaged unit)
- [ ] Web-server user is in group `pagespeed` (module postinst does the join;
  `id www-data` / `id apache` must list it) and the web server was restarted
  after the join
- [ ] Cache dir `/var/cache/pagespeed-optimizer/v3` is 3770 (setgid+sticky)
  `pagespeed:pagespeed`; volume + serve-stats 0660, shared config 0640 —
  nothing world-writable (`find /var/cache/pagespeed-optimizer -perm -o+w`
  is empty)
- [ ] Cache volume path matches between nginx (`pagespeed_cache_path`) and
  worker (`--cache-dir` / `--cache-path`)

## Socket Configuration

- [ ] Worker socket path configured via `--socket` flag or `PAGESPEED_SOCKET_PATH` env var
  - The worker writes the socket path to `pagespeed-shared.conf` (next to the cache file)
  - Nginx reads the socket path automatically from this shared config file
- [ ] Socket file is 0660 `pagespeed:pagespeed` (nginx reaches it via group
  membership, never world permissions)
- [ ] Management socket accessible for STATS/PURGE operations
- [ ] Secrets come from `/etc/pagespeed-optimizer/daemon.env` (0640
  root:pagespeed), not the command line (`/proc/<pid>/cmdline` shows no
  token values)

## Nginx Module

- [ ] Module loaded: the package's snippet (`sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules` shows exactly one line); add `load_module` by hand only for a self-built module.
- [ ] `pagespeed on;` in server block
- [ ] `proxy_set_header Accept-Encoding "";` to origin (PageSpeed caches raw bytes)
- [ ] Exclusion rules set for API/admin paths (`pagespeed off;` in location blocks)

## Worker Daemon

- [ ] Worker binary or container running and healthy
- [ ] UMask is the backstop `0007` (systemd: `UMask=0007`) — every shared mode
  is set explicitly by the daemon, so correctness never depends on it
- [ ] Chromium installed if browser analysis is enabled
- [ ] Browser sandbox: `GET /v1/health` reports `browser_sandbox: "on"`.
  `"unavailable"` means browser analysis REFUSED to start (the daemon is
  serving without it — read the named ERROR in the log); `"off"` means
  someone passed `--browser-sandbox=off`/`PAGESPEED_BROWSER_SANDBOX=off` and
  untrusted page content is being parsed with no kernel-enforced isolation.
  Container images run as root today and need that opt-out (issue #1429)
- [ ] `shm_size: 256m` or greater for Docker deployments with browser analysis

## Management API

- [ ] The API is OFF unless you enabled it (`--api-socket` or `--api-port`);
  a package install ships it disabled
- [ ] Local access goes over the unix socket
  (`--api-socket /run/pagespeed-optimizer/api.sock`, 0660
  `pagespeed:pagespeed`) — a connection accepted there IS an authenticated
  peer, so no token is asked for (reads and purge alike), and `ss -ltn` shows
  no listener at all. Confirm the group boundary is the one you want: anyone
  in group `pagespeed` can purge the cache
- [ ] A TCP bind is loopback unless you meant otherwise: a non-loopback bind
  needs BOTH `PAGESPEED_API_TOKEN` and `--api-allow-remote`, and the daemon
  REFUSES TO START otherwise. `--api-bind` takes an IPv4 literal only
- [ ] Only one transport: `--api-socket` and `--api-port` together are refused
  at startup
- [ ] `PAGESPEED_API_TOKEN` and `PAGESPEED_PURGE_TOKEN` are present in
  `/etc/pagespeed-optimizer/daemon.env` (the package generated them at
  install time and never rewrites them)
- [ ] If you run tokenless on TCP loopback deliberately, `--api-no-auth` says
  so and a WARNING banner is in the log — anyone who can reach the port can
  purge the cache and read the cached-URL inventory. (The socket transport
  never needs this flag and never prints that banner)
- [ ] Reads are open only if you asked (`--api-read-open`); an absent token no
  longer opens them, and neither does a new GET route in a later release
  unless it's deliberately added to the allow-list. Read-open exposes the
  cached-URL inventory, origin hosts, the cooldown table, and the live
  stats/events streams (the log endpoint/stream stays token-only) -- only
  turn it on behind a reverse proxy that authenticates, never directly on
  the open internet

## Docker/Kubernetes

- [ ] Shared volume mounted at same path in both nginx and worker containers
- [ ] Volume type: `emptyDir` (Kubernetes) or named volume (Docker Compose)
- [ ] Resource limits set appropriately (worker is CPU-intensive during optimization)

## Monitoring

- [ ] Health check endpoint responding (`/health` or worker health socket)
- [ ] `X-PageSpeed` response header visible (HIT/MISS/PASS)
- [ ] Management socket accessible for STATS queries
- [ ] Log levels configured (`--log-level` on worker)

## Verification

```bash
# Check nginx sees optimized content
curl -sI https://yoursite.com/image.jpg -H "Accept: image/webp" | grep X-PageSpeed
# Expected: X-PageSpeed: HIT (after worker processes)

# Check worker health
echo "STATS" | socat - UNIX-CONNECT:/path/to/pagespeed.sock.mgmt
```
