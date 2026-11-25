---
title: 'Migrate from ModPageSpeed 2.0'
description: 'Move a ModPageSpeed 2.0 Docker Compose or Helm deployment to mod_pagespeed: unprivileged worker, loopback-bound API, configuration carries over.'
order: 11
group: 'Upgrade and migrate'
lastUpdated: 2026-11-15
faq:
  - q: 'Do I have to recreate my cache volume?'
    a: 'No. On its first start, the optimizer worker (2.1.0 and later) migrates a volume created by 2.0 in place: it adopts the cache files into the unprivileged `pagespeed` user and keeps the warm cache, deleting nothing. Native-package installs are different: there the cache lives in a versioned directory and a package upgrade starts it cold by design.'
  - q: 'Do I have to migrate right away?'
    a: 'No — migrate on your own schedule. Migrating earlier moves you onto the current release sooner.'
---

The current mod_pagespeed converges the mod_pagespeed lineage and the ModPageSpeed 2.0
engine into one product. If you run 2.0 today, the engine you rely on continues
as the **optimizer worker** of mod_pagespeed. The migration upgrades the deployment around
it rather than rebuilding it: your nginx directives and worker flags carry
over. What to review is who the worker runs as, how it exposes the
management API and browser analysis by default, and (for Helm) the pod
security defaults, all of which changed in 2.1.0.

Migration is manual — there is no automated path — but for most deployments it
is an image bump plus a review of the defaults below.

## Who this guide is for

This guide covers ModPageSpeed 2.0 Docker Compose and Helm deployments,
including the combined evaluation image. The other starting points have their
own pages:

- **mod_pagespeed 1.15 installs** (the native Apache, nginx and IIS module):
  [Migrate from mod_pagespeed 1.15](/docs/migrate-from-1-15/).
- **The open-source ngx_pagespeed**, compiled or from a third-party build:
  [Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/).
- **The archived open-source mod_pagespeed**:
  [Migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/).
- **IISpeed**: [Migrate from IISpeed](/docs/migrate-from-iispeed/).

It is not for users of the ASP.NET Core middleware: `WeAmp.PageSpeed.AspNetCore`
is the ASP.NET Core form of mod_pagespeed, and there is no migration step —
see [ASP.NET Core performance](/aspnet-core-performance/) for that path.

## What changed in 2.1.0 {#what-changes-in-21}

### The worker runs unprivileged

Since 2.1.0 the worker runs as the dedicated `pagespeed` user at a fixed UID/GID of
**918**, with an empty capability set and new privileges disabled. The shared
cache volume is owned by it: `/data` is group-readable (`2750`), cache files
and sockets are `0660`, and the serving side reaches them through membership
in group `pagespeed` — the nginx image already joins it.

A volume created by 2.0 is **migrated in place on first start**: the
entrypoint adopts the cache files into the `pagespeed` user, logs each file it
takes over, and keeps your warm cache. If it finds content it cannot safely
adopt, it refuses **before** the worker starts, exits with status **78**,
names the path, and prints the one-line remedy. Exit 78 always means the
configuration or the volume rather than a transient fault — worth
distinguishing in your restart alerting. Set `PAGESPEED_ADOPT_VOLUME=off` to
skip the automatic migration and fix ownership by hand instead.

Three setups need an explicit change:

- You run the worker with an explicit `user:` / `--user`: use `918:918`, and
  make sure a bind-mounted data directory is owned by it.
- Kubernetes: set `securityContext.fsGroup: 918`.
- A third container mounts the cache read-only: add `918` to that container's
  supplementary groups (`group_add: ["918"]`).

### Management API: loopback and a token, by default

Since 2.1.0 the container binds the management API to loopback and requires a
token; a non-loopback bind refuses to start unless you set both
`PAGESPEED_API_ALLOW_REMOTE=true` and a token. If you deliberately publish the
API, set both — and keep your reverse proxy's own authentication in front of
it as well.

When the API is enabled and no token is supplied, the entrypoint generates one
per container start, together with a generated `PURGE` token so cache
invalidation works out of the box. It writes both to `/data/generated-tokens`
(mode 0600, one `KEY=value` per line) and logs only that path; read them with
`docker exec <container> cat /data/generated-tokens`. (Releases before this
change printed the values to the container log.) Set `PAGESPEED_API_TOKEN` and
`PAGESPEED_PURGE_TOKEN` yourself for values that stay the same across
restarts.

### Browser analysis: the sandbox is required

Since 2.1.0 browser analysis requires the Chrome sandbox by default, and the
unprivileged worker satisfies that requirement — provided the container
runtime's seccomp profile permits user namespaces. Where it does not, browser
analysis refuses to start and `/v1/health` reports
`browser_sandbox: "unavailable"` naming the cause; optimization keeps serving.
Run with a Chrome-compatible seccomp profile, or set
`PAGESPEED_BROWSER_SANDBOX=off` to accept an unsandboxed browser deliberately.

### Helm: the pod seccomp profile

Since 2.1.0 the chart sets `seccompProfile.type: RuntimeDefault` on the pod. If a
workload of yours needs a syscall the runtime's default profile blocks, opt
out with:

```yaml
podSecurityContext:
  seccompProfile: null
```

Helm deep-merges values, so setting the key to `null` is what removes it — an
empty `podSecurityContext: {}` will **not** override the chart default.

### Native packages

mod_pagespeed also ships as native web-server packages: the module plus the
`pagespeed-optimizer` worker, installed and upgraded as a matching pair. On a
package install the worker keeps its cache in a versioned directory
(`/var/cache/pagespeed-optimizer/v3`), and a package upgrade starts that
cache cold by design. See [downloads](/download/) for packages and
platforms.

## Configuration mapping

Your configuration carries over:

- **nginx directives** (`pagespeed on;`, `pagespeed_cache_path`,
  `pagespeed_cache_mode`, the max-age family, and the rest of the
  [configuration reference](/docs/configuration/)) are unchanged in 2.1.
- **Worker flags** (`--cache-dir`, `--cache-size`, image quality, proactive
  variants, browser analysis, and the rest) are unchanged in 2.1.

What to review is operational defaults, not directives:

| Area             | Default since 2.1.0                                                           | Act if                                                                                               |
| ---------------- | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| Management API   | Loopback bind; token required                                               | You reach the API from another host: set `PAGESPEED_API_ALLOW_REMOTE=true` and `PAGESPEED_API_TOKEN` |
| Purge endpoint   | Token required (generated if unset)                                         | You automate purges: set `PAGESPEED_PURGE_TOKEN`                                                     |
| Browser analysis | Sandbox required                                                            | Your runtime's seccomp profile blocks user namespaces: allow them, or opt out deliberately           |
| Helm pod         | `seccompProfile.type: RuntimeDefault`                                       | A workload needs blocked syscalls: set `seccompProfile: null`                                        |
| Worker identity  | `pagespeed` (UID/GID 918)                                                   | You set an explicit `user:`, bind-mount the cache, or share it with a third container                |
| Package paths    | `/var/cache/pagespeed-optimizer/v3`, `/run/pagespeed-optimizer/notify.sock` | Your web-server config points at explicitly configured worker paths: repoint both                    |

:::note
If every request is a cache MISS after upgrading, the usual cause is a path
disagreement between the web server's configuration and where the worker now
keeps its cache and socket — see
[troubleshooting](/docs/troubleshooting/) for the checks.
:::

## The reverse-proxy gap

One 2.0 deployment shape has no equivalent in mod_pagespeed yet: the **any-origin caching
reverse proxy** — running the stack purely as an optimizing proxy in front of
an origin that is otherwise untouched. Closing that gap is a committed part of
the mod_pagespeed roadmap. Until it is covered, that shape stays where it
is, fully supported on 2.0, and this guide applies when the coverage lands.

## Migration steps

1. **Upgrade** to the current images or chart version — see
   [Install with Docker](/docs/installation-docker/) and the
   [Helm deployment guide](/docs/helm-deployment/) for the current tags and
   values.
2. **Review the defaults table above** and set the environment variables and
   values your deployment needs; remove license configuration.
3. **Watch the first start.** The entrypoint logs each cache file it adopts.
   An exit status of 78 names the offending path and prints the remedy — it
   is a configuration or volume fault, not a transient one.
4. **Verify.** Responses carry the `X-PageSpeed` header, and cached
   optimizations begin serving warm — the migrated volume keeps its contents.
5. **Roll back if needed** by returning to your previous 2.0 image tags or
   chart version. On package installs, the module and `pagespeed-optimizer`
   move together as a matching pair — roll both back together.

## Upgrading from mod_pagespeed 1.15 {#upgrading-from-1-15}

The 1.15 upgrade has its own page:
[Migrate from mod_pagespeed 1.15](/docs/migrate-from-1-15/). It is a package
upgrade in place: the configuration carries over, the Apache packages wire the
module to the worker, and the old cache stays on disk for a rollback.

## Coming from open-source mod_pagespeed {#coming-from-open-source}

<a id="before-you-start"></a><a id="upgrade-steps"></a><a id="nginx"></a><a id="apache"></a><a id="cache-migration"></a><a id="configuration-compatibility"></a><a id="rolling-back"></a>Moving from the archived open-source project? The three commands, the directive
compatibility table and the rollback are on
[Migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/)
and, for ngx_pagespeed and third-party builds of it,
[Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/).
[Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) covers
what changed and what did not; the archived 1.0 docs are at [/1.0/](/1.0/).

## Licensing

mod_pagespeed is licensed under the Apache License 2.0, free in
development and in production. What We-Amp sells is
[support](/support/) from the people who build
the product; the software and its signed packages are the same for everyone,
and free.
