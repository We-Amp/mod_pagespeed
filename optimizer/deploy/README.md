# mod_pagespeed Deployment

## Quick Start with Docker Compose

The compose file pulls the published `ghcr.io/we-amp/pagespeed-worker` and
`pagespeed-nginx` images at the current release tag; nothing is built locally.

1. Point it at your origin:
   ```bash
   cd deploy
   cp .env.example .env
   ```
   Set `BACKEND_HOST` and `BACKEND_PORT` in `.env`. The default,
   `host.docker.internal:8081`, is a server on the Docker host. The nginx
   image renders its own config from those two variables; for a config of
   your own, copy `nginx.conf.example` to `nginx.conf`, replace the
   `${BACKEND_HOST}:${BACKEND_PORT}` upstream with your origin (a mounted
   config is used as is, not rendered) and uncomment the `./nginx.conf` mount
   in `docker-compose.yml`.

2. Start services. The compose file runs a third service, `pidns` (the
   digest-pinned Kubernetes pause image): the worker and nginx join its PID
   namespace, which keeps upgrades safe while an older build (which judges
   cache-lock holders by PID) shares the volume. Restarting the worker or
   nginx leaves the other up; stopping `pidns` stops both, and `up -d` brings
   them back (see the comment in `docker-compose.yml`). nginx starts once the
   worker reports healthy.
   ```bash
   docker compose up -d
   ```

3. Verify with GET (the module does not cache HEAD, so `curl -I` never shows
   a HIT):
   ```bash
   curl -s -o /dev/null -D - http://localhost/ | grep -i x-pagespeed
   # X-PageSpeed: MISS on the first request, HIT on the next
   ```

## Systemd Deployment

1. Install the worker binary:
   ```bash
   sudo cp bazel-bin/src/worker/factory_worker /usr/local/bin/
   ```

2. Install the systemd unit:
   ```bash
   sudo cp deploy/pagespeed-optimizer.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now pagespeed-optimizer
   ```
   The unit starts `/usr/bin/pagespeed-optimizer` and runs as the `pagespeed`
   user and group, with its cache under `/var/cache/pagespeed-optimizer` (the
   packages create both from `deploy/pagespeed-optimizer.sysusers.conf` and
   `deploy/pagespeed-optimizer.tmpfiles.conf`). Before the `enable --now` step,
   adjust `ExecStart` to where step 1 installed the binary, create that user and
   group, and create the cache directory, for example with
   `sudo systemd-sysusers deploy/pagespeed-optimizer.sysusers.conf` and
   `sudo systemd-tmpfiles --create deploy/pagespeed-optimizer.tmpfiles.conf`.

3. Install the nginx module and configure nginx:
   ```bash
   sudo cp bazel-bin/src/nginx/ngx_pagespeed_module.so /usr/lib/nginx/modules/
   # Edit /etc/nginx/nginx.conf using deploy/nginx.conf.example as reference
   sudo nginx -t && sudo systemctl reload nginx
   ```

4. Install log rotation:
   ```bash
   sudo cp deploy/logrotate.d/pagespeed /etc/logrotate.d/
   ```

## Syscall filtering (host packages)

The `pagespeed-optimizer` package's systemd unit **enforces** a system-call
allow-list (since 2.1.0): the daemon may make the calls in systemd's
`@system-service` group minus `@privileged` and `@resources`, and the kernel
kills it with `SIGSYS` on the first call outside that set. The list is in the
unit itself, so there is no drop-in to install to get it and none that can be
installed "alone" by mistake. `RestrictNamespaces=yes` ships alongside it.
The measured basis is `deploy/pagespeed-optimizer.syscall-census.md`; the
unit also keeps `SystemCallLog=~@system-service`, so every call outside the
group is still recorded in the audit log.

To confirm the posture, ask systemd — not the daemon:

```bash
systemctl show -p SystemCallFilter pagespeed-optimizer
# enforcing -> a long allow-list (no leading '~')
# opted out -> SystemCallFilter=~   (an empty deny-list: nothing is filtered)
systemd-analyze security pagespeed-optimizer
```

The daemon's own `syscall_filter` field on `/v1/health` is **not** the
confirmation: it reports `"filtered"` in both postures, because
`SystemCallLog=` is itself implemented as a seccomp filter. It answers "is a
filter attached", not "is this profile enforcing". Reading it at all requires
the management API to be enabled (`OPTIMIZER_OPTS=--api-socket` in
`/etc/default/pagespeed-optimizer`); a stock install binds no API at all.

### Browser analysis under the profile

Headless Chrome's own sandbox needs a handful of calls the daemon's list does
not admit (`seccomp`, `chroot`, the privilege-drop and scheduling calls, and a
few probes). The package ships that re-admission list **installed and
active**, as a vendor drop-in next to the unit
(`/usr/lib/systemd/system/pagespeed-optimizer.service.d/20-browser-analysis.conf`;
`/lib/systemd/system/...` on Debian-family hosts), so browser analysis works
under the enforcing unit with no extra step. It only adds to the unit's
allow-list and lifts `RestrictNamespaces=` for the Chrome sandbox. The
package's own verification starts a real Chromium under this default and
asserts it renders across a service restart without a kill.

A host that does not run browser analysis can take the narrower daemon-only
profile back by **masking** the vendor file with an empty file of the same
name under `/etc` — systemd lets a same-named drop-in in `/etc` replace the
vendor one, and an empty drop-in contributes nothing:

```bash
sudo install -D -m 0644 /dev/null \
  /etc/systemd/system/pagespeed-optimizer.service.d/20-browser-analysis.conf
sudo systemctl daemon-reload && sudo systemctl restart pagespeed-optimizer
```

The mask survives upgrades for the same reason the opt-out does (the package
never writes under `/etc/systemd`); remove the empty file and repeat the two
commands to turn the profile back on. With it masked the daemon still starts
and still serves; browser analysis refuses to initialize and names the
restriction, never falling back to an unsandboxed browser. Masking is a
tightening, not an opt-out — the opt-out is the next section.

### Opting out

The documented way off — back to the pre-2.1.0 log-only posture — is a drop-in
of your own, copied from the shipped example, that resets the two directives:

```bash
sudo install -D -m 0644 \
  /usr/share/doc/pagespeed-optimizer/90-syscall-filter-off.conf.example \
  /etc/systemd/system/pagespeed-optimizer.service.d/90-syscall-filter-off.conf
sudo systemctl daemon-reload && sudo systemctl restart pagespeed-optimizer
systemctl show -p SystemCallFilter pagespeed-optimizer   # -> SystemCallFilter=~
```

It survives `apt upgrade` and `dnf upgrade`: the package writes its vendor
configuration under `/usr/lib/systemd` (and `/lib/systemd` on Debian-family
hosts) and never under `/etc/systemd`, so nothing an upgrade does can touch,
prompt on, or re-create a file there. Remove the file and run the same two
commands to go back to enforcing. The `90-` prefix is load-bearing: systemd
applies drop-ins in lexical order of their file names regardless of
directory, and an empty assignment resets the directive instead of adding to
it, so the file has to sort after every vendor drop-in for its resets to win.
Do not edit the unit under `/usr/lib` or `/lib` instead — an upgrade
overwrites it.

> **On a minimal Ubuntu container image the example files may be absent.**
> Those images ship a dpkg config with `path-exclude=/usr/share/doc/*`, so
> dpkg discards the drop-in examples (and `daemon.env.example`) as it
> installs. A normal Ubuntu or Debian host has no such setting and gets them.
> If they are missing, either drop the exclusion
> (`rm /etc/dpkg/dpkg.cfg.d/excludes`) and reinstall, or copy the files out of
> `deploy/` in the source tree — the content is identical.

### Diagnosing a SIGSYS

A kill by the profile shows up three ways: `systemctl status
pagespeed-optimizer` reports `code=dumped, status=31/SYS` when the daemon
itself was killed (`LimitCORE=0` means no dump is written; the kernel still
classifies the signal that way); the journal shows the daemon's own
`Chrome exited (status=0, signal=31)` line when the victim was the browser;
and the kernel writes an audit record of type `1326` naming the call by
**number** (`syscall=N`). Where that record lands depends on `auditd`:

```bash
systemctl is-active auditd
# active   -> ausearch -m SECCOMP -ts recent -i      (-i turns the number into a name)
#             (or: grep -a 'type=SECCOMP' /var/log/audit/audit.log)
# inactive -> journalctl -k | grep 'type=1326'        (the kernel ring; audit records carry
#             no unit field, so `journalctl -u` cannot select them)
# a KILL reads sig=31 (code=0x8...); sig=0 code=0x7ffc0000 lines are the log filter's records
```

When `auditd` is running (the default on RHEL/AlmaLinux/Rocky) it owns the
kernel's audit socket and these records **never reach the journal** — an empty
journal there means nothing at all, not a clean run. To turn a bare number
into a name where nothing did it for you, `ausyscall <N>` (from the audit
package) or `systemd-analyze syscall-filter` (lists every group with its
members). Report the name together with the Chrome version if the victim was
the browser — a name missing from the browser profile is a bug in the shipped
file, and the fix is a name in it, not a host that stays opted out.

## Syscall filtering (containers)

A container has no systemd, so none of the above applies there: the container
runtime's own seccomp profile is the filter, and `/v1/health` will report
`syscall_filter` accordingly.

`deploy/chrome-seccomp.json` is Docker's default profile with four calls
(`clone`, `clone3`, `unshare`, `chroot`) unblocked, which is what headless
Chrome needs to build its own layer-1 sandbox. Use it only if you run browser
analysis in a container:

```bash
docker run --security-opt seccomp=deploy/chrome-seccomp.json ...
```

It is generated, not hand-maintained — regenerate against a newer Docker
release with `tools/packaging/make-chrome-seccomp.sh` and commit the diff.
It is also not sufficient on its own: the host kernel must allow unprivileged
user namespaces, and the daemon must not be running as uid 0 (Chrome refuses
to sandbox itself as root, and the daemon reports that as
`browser_sandbox: "unavailable"` with the reason named).

On Kubernetes the Helm chart sets `podSecurityContext.seccompProfile.type:
RuntimeDefault` by default; `deploy/helm/pagespeed/values.yaml` documents the
`Localhost` variant for the Chrome profile.

## Health Checks

Worker health (from the host running the worker):
```bash
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX)
s.connect('/var/lib/pagespeed/pagespeed.sock.health')
print(s.recv(256).decode())
s.close()
"
# Expected: OK 0/128
```

Nginx health:
```bash
curl http://localhost/health
# Expected: OK
```

The container health checks in `docker-compose.yml` and the Helm chart read
the same socket with `socat`:
```bash
socat -u -T 4 UNIX-CONNECT:/data/pagespeed.sock.health - | grep -q '^OK'
```
`-u` makes socat read only, and `-T 4` is how long it waits for the status
line: four seconds, one second inside the 5 s probe timeout. If you write your
own probe, keep both options. A probe that pipes an empty input into socat
(`echo '' | socat - UNIX-CONNECT:...`) gives up 0.5 s after connecting,
whatever the probe timeout says, and reports a busy optimizer as unhealthy.

## Configuration

See the [configuration reference](https://modpagespeed.com/docs/configuration/)
for all nginx directives and worker flags.

### Environment Variables (Docker)

| Variable | Default | Description |
|----------|---------|-------------|
| `CACHE_SIZE` | `536870912` (512MB) | Cache volume size in bytes |
| `NGINX_PORT` | `80` | Host port for HTTP |
| `NGINX_SSL_PORT` | `443` | Host port for HTTPS |

### Cache Sizing

| Site Type | Recommended |
|-----------|-------------|
| Small blog | 100 MB |
| Medium site | 256-512 MB |
| Large site | 1-4 GB |
| Image-heavy | 2-8 GB |
