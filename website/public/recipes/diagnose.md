# Diagnose: check an installed mod_pagespeed module or optimizer worker (read-only)

Source: https://modpagespeed.com/docs/is-it-working/,
https://modpagespeed.com/docs/monitoring/,
https://modpagespeed.com/docs/http-api/,
https://modpagespeed.com/docs/admin-console/,
https://modpagespeed.com/docs/troubleshooting/,
https://modpagespeed.com/docs/rule-packs/ and
https://modpagespeed.com/docs/release-notes-2-3/.
Scope: a server where the module, the middleware or the worker is already
installed. The recipe finds out whether it works and, if not, where it stops,
from what the server already exposes: response headers, the worker's health
socket and API reads, the module's admin pages, logs and counters.

This recipe is read-only. Never purge a cache, send a `PATCH` or `POST`,
restart a service, edit a configuration file or turn on `--log-level debug`.
Stop and ask before anything that would change state, including a step a linked
troubleshooting page suggests. Contact only the operator's own server and
loopback.

## 1. Identify the surface

Pick the surface with the signals the install recipes use, then note which
header to expect and which processes should run.

```bash
nginx -v                                     # nginx
apache2ctl -v 2>/dev/null || httpd -v        # Apache
test -d /usr/local/cpanel && echo 'cPanel'   # cPanel / EasyApache 4
dpkg -s nginx-module-pagespeed mod-pagespeed pagespeed-optimizer   # Debian / Ubuntu: which packages are installed
rpm -q nginx-module-pagespeed mod-pagespeed pagespeed-optimizer ea-apache24-mod_pagespeed   # Enterprise Linux and cPanel
docker compose -p pagespeed ps               # Docker: pidns, worker and nginx running
docker ps -a --filter 'name=^pagespeed$' --format '{{.Names}} {{.Status}}'   # Docker, one container: Up
kubectl get pods -n <namespace> -l app.kubernetes.io/name=pagespeed   # Helm: READY 2/2
```

| Surface        | Header            | Value                                   | Processes                                |
| -------------- | ----------------- | --------------------------------------- | ---------------------------------------- |
| Apache, cPanel | `X-Mod-Pagespeed` | `on` (the version with ExposeVersionInHeaders) | the module; the worker when installed    |
| nginx, IIS     | `X-Page-Speed`    | `on` (the version with ExposeVersionInHeaders) | the module; the worker when attached     |
| Docker, Helm   | `X-PageSpeed`     | `MISS`, `HIT`, `STALE` or `REVALIDATED` | the module and the worker                |
| ASP.NET Core   | `X-PageSpeed`     | `MISS`, then `HIT`                      | the middleware and its in-process worker |

IIS (elevated PowerShell) and ASP.NET Core (a `.csproj` using
`Microsoft.NET.Sdk.Web` that references `WeAmp.PageSpeed.AspNetCore`):

```powershell
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\InetStp').MajorVersion   # IIS present
$uninstall = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
Get-ItemProperty $uninstall -ErrorAction SilentlyContinue | Where-Object DisplayName -like '*PageSpeed for IIS*' | Select-Object DisplayName, DisplayVersion
Select-String -Path *.csproj -Pattern 'WeAmp.PageSpeed.AspNetCore'   # ASP.NET Core: run in the project directory
```

Stop and ask if more than one surface applies. If nothing named pagespeed is
installed, stop and say so: there is nothing to diagnose, and installing is the
install recipes' job (https://modpagespeed.com/recipes/README.md).

## 2. Is the module answering

Request the site's own page with the verification marker and read the header
the table above names:

```bash
curl -s -o /dev/null -D - '<site-url>/?mps-verify=agent' | grep -i -E '^(x-mod-pagespeed|x-page-speed|x-pagespeed):'
```

PowerShell (IIS, ASP.NET Core on Windows):

```powershell
(Invoke-WebRequest '<site-url>/?mps-verify=agent' -UseBasicParsing).Headers['X-Page-Speed']   # IIS
# ASP.NET Core (Windows)
curl.exe -s -o NUL -D - 'http://localhost:<port>/<html-route>?mps-verify=agent' | Select-String '^x-pagespeed:'
```

Retry at most 3 times a few seconds apart. Use a URL that returns an HTML page
(`text/html`); the Docker and Helm proxies add the header only to a 2xx
response, and the ASP.NET Core middleware only to HTML it rewrites.

- `X-Mod-Pagespeed` or `X-Page-Speed` (value `on`, or the version): the module is loaded and
  `pagespeed on` applies to that site.
- `X-PageSpeed: MISS`: the module is active. `HIT` on a later request means the
  worker wrote the optimized variant. `MISS` on every request:
  https://modpagespeed.com/docs/troubleshooting/#cache-miss-on-every-request
- No header: https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

On Apache, nginx and IIS, compare the page with and without the module for one
request. `?PageSpeed=off` needs no token; the Docker proxy ignores it.

```bash
curl -s '<site-url>/?mps-verify=agent' | wc -c                 # bytes with the module
curl -s '<site-url>/?PageSpeed=off&mps-verify=agent' | wc -c   # bytes as the origin serves them
```

The same size on both says the module rewrote nothing on that page; the header
still proves it handled the response. The comparison holds only for a page that
is the same on every request. On the Docker proxy, compare an image
with the `Accept` header instead:

```bash
curl -s -o /dev/null -D - '<site-url>/<image-path>.jpg' -H 'Accept: image/webp' | grep -i '^content-type:'   # image/webp once the worker has processed it; image/jpeg before
```

## 3. Is the worker healthy

Skip this section on nginx or IIS without the worker attached.

Packages. Run as a user in group `pagespeed`, or with `sudo` after the
operator's go-ahead:

```bash
systemctl status pagespeed-optimizer --no-pager   # active (running); "failed" or code=dumped, status=31/SYS is a fault
socat -u -T 4 UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.health -
# OK 5/128 notifs=1542 variants=986 proactive=724 errors=3 cache_entries=2048 inflight=3
echo "STATS" | socat -t 5 - UNIX-CONNECT:/run/pagespeed-optimizer/notify.sock.mgmt   # the statistics without the HTTP API
```

`OK` is healthy. `DEGRADED`, with the line ending in `cache=unavailable`, means
the worker runs without its cache. No answer means the worker is down; the web
server keeps serving originals meanwhile. Keep `-u` and `-T 4`.

The HTTP API is off until a transport is enabled. Find which one is on:

```bash
grep -h -o -E -- '--api-(socket|port|read-open)' /etc/default/pagespeed-optimizer /etc/pagespeed-optimizer/daemon.env 2>/dev/null   # which transport and mode are enabled; the unit reads both files
test -S /run/pagespeed-optimizer/api.sock && echo 'unix socket'
curl -s http://localhost:9880/v1/health                # TCP on the default port
```

Neither on: use the health and management sockets above only. Enabling the API
needs a restart; stop and ask.

Over the unix socket every read is open to a peer in group `pagespeed`:

```bash
S=/run/pagespeed-optimizer/api.sock
curl -s --unix-socket "$S" http://localhost/v1/health          # "ready": true
curl -s --unix-socket "$S" http://localhost/v1/stats | jq '{notifications, variants_written: .variants.written, errors, cache, cache_read_failures}'   # without jq, drop the pipe
curl -s --unix-socket "$S" http://localhost/v1/cache/cooldowns  # URLs held back, with a reason
curl -s --unix-socket "$S" 'http://localhost/v1/logs?limit=100'
```

Over TCP, `/v1/health` needs no token. With a token configured every other read
needs it; in read-open mode (`--api-read-open`) `/v1/stats`, `/v1/metrics`,
`/v1/config` and the `/v1/cache/*` reads are open, but `/v1/logs` always needs
the token on TCP. Ask the operator for the token; never read it out of the
process list.

```bash
curl -s http://localhost:9880/v1/health
curl -s -H "Authorization: Bearer <your-token>" http://localhost:9880/v1/stats
curl -s -H "Authorization: Bearer <your-token>" http://localhost:9880/v1/cache/cooldowns
curl -s -H "Authorization: Bearer <your-token>" "http://localhost:9880/v1/logs?limit=100"
```

401 `UNAUTHORIZED` means the header is missing; 403 `FORBIDDEN` means the token
is wrong. Stop and ask; do not guess tokens.

Read from `/v1/stats`:

- `notifications.received` 0: the worker hears nothing from the web server
  (socket path or socket permissions).
- `variants.written` 0 while `notifications.received` is high: the worker
  receives work but writes nothing (permissions or processing errors).
- `errors.total` rising: processing failures; read the worker log.
- `cache.entries` 0: the cache may not open.
- `cache_read_failures` rising: the worker cannot read entries from the cache.

In `/v1/cache/cooldowns`, a `write_failure` reason means a cache write failed.

Docker and Helm. The Compose file and the chart use the health socket as their
health check:

```bash
docker compose -p pagespeed ps                                        # worker running and healthy
kubectl get pods -n <namespace> -l app.kubernetes.io/name=pagespeed   # READY 2/2
```

If the stack publishes the API (`PAGESPEED_API_PORT=9880`), the TCP reads above
work against `localhost:9880`.

ASP.NET Core. The middleware serves `/console/` and `/v1/*` on the app's own
port:

```powershell
curl.exe -s http://localhost:<port>/v1/health
curl.exe -s http://localhost:<port>/v1/stats
```

With `PageSpeed:Console:Enabled` set to `false` these paths fall through to the
app (typically `404`), so there is no API to read; that is the recommended
production setting, not a fault. When the app gates the console with its own
authentication, these reads return `401`, `403` or a sign-in redirect without
the admin's login; ask the user rather than working around it.

## 4. What the module reports

Apache, nginx and IIS serve admin pages at `/pagespeed_admin/` (one virtual
host) and `/pagespeed_global_admin/` (the whole server). They are normally
restricted to localhost, so request them from the server itself. Those are the
paths the docs examples set; on nginx there is no admin page until `AdminPath`
is set. Find the configured paths:

```bash
sudo nginx -T 2>/dev/null | grep -i -E 'AdminPath|MessageBufferSize|AdminDomains'   # nginx
apachectl -M 2>/dev/null | grep pagespeed_module                                     # Apache: module loaded
```

Then read the pages:

```bash
A='http://localhost/pagespeed_admin'
curl -s -o /dev/null -w '%{http_code}\n' "$A/"   # 200: reachable
curl -s "$A/statistics" | grep -E 'ipro_daemon_served|ipro_daemon_fallthrough|resource_url_domain_rejections|rule_pack_load'
curl -s "$A/config"            # active filters and directive values
curl -s "$A/cache"             # cache status; never use its purge
curl -s "$A/message_history"   # recent messages; needs a non-zero MessageBufferSize
```

PowerShell (IIS):

```powershell
(Invoke-WebRequest 'http://localhost/pagespeed_admin/' -UseBasicParsing).StatusCode   # 200
(Invoke-WebRequest 'http://localhost/pagespeed_global_admin/message_history' -UseBasicParsing).Content
```

A 403 or 404 means the pages are restricted, on another path or not set. On
IIS, `InfoUrlsLocalOnly` (on by default) admits only the local machine; on
Apache and nginx, `AdminDomains` and the `location` blocks decide. Stop and
ask. Never widen `AdminDomains`, `InfoUrlsLocalOnly` or the `location` blocks to
reach the pages. If the grep prints nothing, do not read that as zero: the page
may only render in a browser. Open `$A/statistics` in a browser on the server,
read the counters there, or write "not readable with curl" in the report.

Read the counters:

- `ipro_daemon_served` rising: in-place requests are answered from the worker's
  cache, so the pairing optimizes.
- `ipro_daemon_fallthrough` rising while `ipro_daemon_served` stays flat on
  CSS, JavaScript and image requests: the ordinary path answers and the worker
  is not serving. Since 2.3.0, HTML requests always count as fallthrough.
- `resource_url_domain_rejections` rising: images keep their original bytes
  because of a `Disallow` or a domain mismatch.
- `rule_pack_load_errors` above 0: a rule pack did not load.

The Overview page raises alerts such as "Optimizer unreachable" and "Optimizer
health check failing"; either means the module serves without the worker.

Rule packs, only when the page exists (it ships with the `rule_packs` filter):

```bash
A='http://localhost/pagespeed_admin'
curl -s -o /dev/null -w '%{http_code}\n' "$A/rule_packs"   # 200: present; 404: no Rule packs page, skip
curl -s "$A/rule_packs?raw=1"                              # the newest decision-log lines as plain text
```

The page shows whether the filter is on, the pack file and mode, the loaded
pack and the last load error. A signature refusal still shows as loaded there;
read the error log for it.

Optional, only if the operator enables it: `?PageSpeedFilters=+debug` writes the
module's decisions into HTML comments, but it needs the `RequestOptionOverride`
token on the server. Do not set that token or the `debug` filter yourself:
https://modpagespeed.com/docs/troubleshooting/#how-to-get-more-diagnostic-information

## 5. Logs

```bash
journalctl -u pagespeed-optimizer -n 200 --no-pager                       # worker, packages
grep -i pagespeed /var/log/nginx/error.log | tail -50                     # module, nginx
docker compose -p pagespeed logs --tail 200 worker                        # worker, Docker
docker compose -p pagespeed logs --tail 200 nginx                         # module, Docker
docker logs --tail 200 pagespeed                                          # module and worker, one container
kubectl logs -n <namespace> -l app.kubernetes.io/name=pagespeed -c worker --tail 200   # worker, Helm
kubectl logs -n <namespace> -l app.kubernetes.io/name=pagespeed -c nginx --tail 200    # module, Helm
```

On Apache, read the web server's error log the configuration names. On IIS,
read Windows Logs > Application in Event Viewer for entries from the
mod_pagespeed source:

```powershell
Get-WinEvent -LogName Application -MaxEvents 200 | Where-Object ProviderName -like '*pagespeed*' | Format-List TimeCreated, LevelDisplayName, Message
```

On ASP.NET Core, read the application's own log output.

Lines that name a cause:

- `pagespeed: cache-directory generation mismatch`: module and worker are from
  different releases, so the module serves with its cache off and every
  response is a `MISS`.
- `rule pack <path> (sha256=<64 hex>) failed to load: <path>: <json path>: <reason>`:
  the pack did not load; the line names the reason.
- "permission denied" or "socket absent" in the worker log: the web server user
  is not in group `pagespeed`, or the socket path does not match.
- "too large": the worker skipped content above its size limit.

| Symptom                                                      | Likely cause                                                         |
| ------------------------------------------------------------ | -------------------------------------------------------------------- |
| Health socket silent, or `/v1/health` reports `ready: false` | the worker is down or saturated                                      |
| `errors.total` rising                                        | processing failures; read the worker log                             |
| `connections.active` near `connections.max`                  | change notifications from the web server are being dropped           |
| `X-PageSpeed: MISS` on every request                         | can mean module and worker on different cache formats                |
| Container exit status 78                                     | the worker image refused to start on a configuration or volume fault |
| `systemctl status` shows `code=dumped, status=31/SYS`        | the system-call filter stopped the worker                            |

## 6. Report

Fill this template and give it to the operator. Never send logs or
configuration anywhere. Do not paste a `RequestOptionOverride` token that
`/config` may print, or any other value that looks like a token, into the
report.

```text
Surface:            <nginx | Apache | cPanel | IIS | ASP.NET Core | Docker | Helm>
Module version:     <header value or package version>
Worker version:     <version from /v1/health, or "not installed">
Header check:       <exact header line, or "none after 3 tries">
PageSpeed=off size: <bytes with module> / <bytes without>
Worker health:      <health socket line or /v1/health status and ready>
Stats:              notifications.received=<n> variants.written=<n> errors.total=<n> cache.entries=<n> cache_read_failures=<n>
Module counters:    <counters read in section 4, or "not readable with curl">
Log lines:          <up to 5 relevant lines>
Conclusion:         <working | module answers but worker unreachable | no header | cache never hits | rule pack not loaded | unknown>
```

Pick exactly one conclusion. Use `unknown` when the checks disagree or a step
could not run, and say which.

## 7. Rollback

Nothing to roll back: this recipe changes nothing. The one exception: if the
operator asked you to turn on debug logging (`LOG_LEVEL=debug` in
`/etc/default/pagespeed-optimizer`) or a message buffer (`MessageBufferSize`),
say which line to revert and that the service needs the same restart it took
to apply it.
