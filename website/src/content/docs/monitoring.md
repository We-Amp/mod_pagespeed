---
title: 'Monitoring'
description: 'Monitor mod_pagespeed: the admin console, the worker health socket and /v1/health, /v1/stats and Prometheus /v1/metrics, logs, and what to alert on.'
order: 55
group: 'Operate'
lastUpdated: 2026-10-22
datePublished: 2026-10-08
faq:
  - q: 'What is the one probe to wire into a load balancer or Kubernetes?'
    a: 'The worker health socket, the notification socket path with `.health` appended, which answers one line starting with `OK` while the worker is up (`DEGRADED` when its cache is unavailable); or `GET /v1/health` on the management API, which needs no token and reports `ready: true` while the worker has capacity. One of them is enough.'
  - q: 'Where do Prometheus metrics come from?'
    a: 'From `GET /v1/metrics` on the worker management API, in text exposition format, or from the `METRICS` command on the management socket. Every counter and gauge in `/v1/stats` is exposed.'
---

Three places tell you how mod_pagespeed is doing: the module's admin
console inside the web server, the optimizer worker's own health and metrics
endpoints, and the logs. This page says what each one shows and which signals
deserve an alert.

## The admin console (Apache, nginx, IIS)

The native module serves its console at `/pagespeed_admin/` (one virtual host)
and `/pagespeed_global_admin/` (the whole server). The Overview page answers
three questions at a glance (is the module working, is the optimizer worker
healthy, what have they saved) and raises alerts above the cards, errors
before warnings:

| Alert                             | Severity | Fires when                                                                                           |
| --------------------------------- | -------- | ---------------------------------------------------------------------------------------------------- |
| Optimizer health check failing    | Error    | one of the worker's health checks reports failure                                                    |
| Optimizer write failures          | Error    | the worker's count of failed cache writes increases (disk pressure, or permissions on its volume)   |
| Optimizer unreachable             | Warning  | the worker is configured but not answering                                                           |
| Optimizer version                 | Warning  | the worker is too old for this console                                                               |
| Optimizer errors                  | Warning  | the worker's error count increases                                                                   |
| Origin sends compressed responses | Warning  | the worker received a response it cannot optimize because the origin compressed it                   |
| Module fetch failures             | Warning  | the module's resource-fetch-failure count increases                                                  |
| Browser analysis stopped          | Warning  | browser analysis is enabled but the browser is not running                                           |
| Optimizer threads busy            | Warning  | every worker thread is busy; new work waits                                                          |
| Optimizer connections             | Warning  | worker connections are above 90% of the maximum; change notifications may be dropped                 |

The Statistics page carries the counters behind them. For the worker pairing,
watch `ipro_daemon_served` (in-place requests answered from the worker's
cache) against `ipro_daemon_fallthrough` (answered by the ordinary path); for
images that stay unoptimized, `resource_url_domain_rejections`. The console's
panels for the worker show its status and health checks, per-type serve
savings and back-pressure. [Admin console](/docs/admin-console/) documents
every page.

## Worker health

Two probes, both answered by the worker itself.

**The health socket** is always on. It is the notification socket's path with
`.health` appended: `/run/pagespeed-optimizer/notify.sock.health` on the
packages, `/data/pagespeed.sock.health` in the container images. It answers
one line:

```bash
socat -u -T 4 UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.health -
# OK 5/128 notifs=1542 variants=986 proactive=724 errors=3 cache_entries=2048 inflight=3
```

The first word is `OK`, or `DEGRADED` when the worker runs without its cache
(the line then ends in `cache=unavailable`); `5/128` is active connections of
the maximum, `inflight` the work in progress, and the rest are running totals.
`-u` makes socat read-only and `-T 4` gives the worker four seconds to answer;
keep both if you write your own probe. A probe that pipes an empty input into
socat gives up half a second after connecting and reports a busy worker as
unhealthy. The Compose file and the Helm chart use this probe for their
container health checks.

**`GET /v1/health`** on the management API needs no token on any transport.
The API is off until you enable a transport; on the packages, set
`OPTIMIZER_OPTS=--api-socket` in `/etc/default/pagespeed-optimizer` and
restart, and it listens on `/run/pagespeed-optimizer/api.sock`:

```bash
curl --unix-socket /run/pagespeed-optimizer/api.sock http://localhost/v1/health
```

The answer carries `status`, `ready` (`true` while in-flight work is below the
thread count), a `checks` table of named health checks, `version`,
`uptime_seconds`, `connections`, `inflight`, `browser_sandbox` and
`syscall_filter`. Monitoring that keyed on a `license` object from an earlier
line has nothing left to read; key on `ready` and `status`.

The admin console reads the same health through the module's read-only proxy
when `DaemonApiSocketPath` is set, so a console that shows the worker as
**Running** is a second confirmation.

## Metrics

- **`GET /v1/stats`**: the full statistics as JSON: notifications received and
  skipped, variants written, errors, cache entries and size, processing time
  per content type, image formats produced, serve savings per content type
  and per host, and the serve classes.
  [Call the worker HTTP API](/docs/http-api/#get-v1stats) lists every field.
- **`GET /v1/metrics`**: the same counters and gauges in Prometheus text
  exposition format (`pagespeed_notifications_total`,
  `pagespeed_variants_written_total`, `pagespeed_cache_entries`, the per-host
  `pagespeed_host_*_total` series, and so on). Scrape it over TCP
  (`--api-port`, loopback by default) or through a reverse proxy onto the unix
  socket.
- **The management socket**, the notification socket path with `.mgmt`
  appended, answers the `STATS` and `METRICS` commands without the HTTP API:

  ```bash
  echo "METRICS" | socat -t 5 - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt
  ```

- **The web console** at `/console/` shows the same data live, with the
  dashboard, metrics and bandwidth-savings pages; see
  [Use the web console](/docs/workbench/).
- **The module's own statistics** are on the admin console's Statistics page
  and, on nginx, at `StatisticsPath`; `StatisticsLogging` feeds the console's
  historical graphs.

## Logs

| Where you run      | Worker log                                                                 | Module log                                                                                           |
| ------------------ | -------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Packages (systemd) | `journalctl -u pagespeed-optimizer -f` (JSON lines; `-o cat \| jq .` parses them) | the web server's error log, and the admin console's [message history](/docs/admin-console/#message-history) |
| Docker Compose     | `docker compose logs -f worker`                                            | `docker compose logs -f nginx`                                                                       |
| Kubernetes (Helm)  | `kubectl logs <pod> -c worker`                                             | `kubectl logs <pod> -c nginx`                                                                        |
| IIS                | Windows Event Viewer, Windows Logs › Application                            | Event Viewer, and the admin console's message history                                                |
| ASP.NET Core       | the application's own log output                                           | the console at `/console/` on your app's port                                                        |

The worker also keeps its last 2,000 log entries in memory, readable as
`GET /v1/logs` and streamed at `/v1/ws/logs`; over TCP both require the API
token even in read-open mode, because log lines carry the URLs and hosts of
every site the worker serves. Set `--log-level debug` (packages:
`LOG_LEVEL=debug` in `/etc/default/pagespeed-optimizer`) for a diagnosis, not
for steady state. Log rotation is covered under
[Run in production](/docs/deployment/#log-rotation).

## What to alert on

| Signal                                                                                   | Where                                  | Why                                                                                                          |
| ---------------------------------------------------------------------------------------- | -------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| The health socket not answering, or `/v1/health` reporting `ready: false` past one probe | health probes                          | the worker is down or saturated; the web server keeps serving originals meanwhile                            |
| `errors.total` in `/v1/stats` rising steadily                                            | metrics                                | processing failures; read the worker log                                                                     |
| `cache.size_bytes` approaching the configured cache size                                 | metrics                                | eviction is active; grow the cache (see [cache sizing](/docs/deployment/#cache-sizing-recommendations))     |
| `connections.active` near `connections.max`                                              | metrics; console alert "Optimizer connections" | change notifications from the web server are being dropped; raise `--max-connections`                |
| Console alerts "Optimizer unreachable" or "Optimizer health check failing"               | admin console                          | the pairing is broken; the module serves without the worker                                                  |
| `X-PageSpeed: MISS` on every request, or `$pagespeed_cache_generation` reading `mismatch` | the reverse proxy's access log        | module and worker on different cache formats; upgrade or roll back both together                              |
| Entrypoint exit status 78 (container images)                                             | container status                       | the worker image's entrypoint refused to start on a configuration or volume fault, never a transient one; the message names the path. The packaged unit has no exit 78: a refusal to start exits 1 and, after five tries in a minute, `systemctl status` shows the unit `failed` |
| `systemctl status` showing `code=dumped, status=31/SYS`                                  | systemd                                | the system-call filter stopped the worker; the unit file's comments say how to read the kill                 |
| `notifications.skipped_dedup` a large share of `notifications.received`                  | metrics                                | normal: the worker is avoiding redundant work. Not an alert                                                  |
