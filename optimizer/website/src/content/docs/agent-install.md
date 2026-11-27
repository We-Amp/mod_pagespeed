---
title: 'Install with a coding agent'
description: 'Machine-readable install recipes for coding agents, one per server: prerequisites, install, configure, verify and roll back, plus an install skill.'
order: 9.5
group: 'Install'
lastUpdated: 2026-10-30
datePublished: 2026-10-10
---

A coding agent can install mod_pagespeed from a recipe: one Markdown file
per server, written for an agent to run. Each install recipe has the same five
sections: prerequisites check, install, minimal configuration, verification and
rollback; the diagnose recipe is read-only and ends in a report instead. The
commands are fenced, the values to fill in are placeholders in angle brackets,
and there is no prose beyond what a command needs.

The recipes derive from these docs. Every command in a recipe comes from the
docs pages it names at the top; when a command changes, the docs change first
and the recipe follows.

## The recipes

| Server                    | Recipe                                                            | Docs page                                                            |
| ------------------------- | ----------------------------------------------------------------- | -------------------------------------------------------------------- |
| Index of all recipes      | [README.md](https://modpagespeed.com/recipes/README.md)           |                                                                      |
| nginx (native module)     | [nginx.md](https://modpagespeed.com/recipes/nginx.md)             | [Install the module on Apache and nginx](/docs/installation-module/) |
| Apache (native module)    | [apache.md](https://modpagespeed.com/recipes/apache.md)           | [Install the module on Apache and nginx](/docs/installation-module/) |
| cPanel / EasyApache 4     | [cpanel.md](https://modpagespeed.com/recipes/cpanel.md)           | [cPanel / EasyApache 4](/docs/cpanel/)                               |
| IIS (native module)       | [iis.md](https://modpagespeed.com/recipes/iis.md)                 | [Install on IIS](/docs/install-iis/)                                 |
| ASP.NET Core (middleware) | [aspnet-core.md](https://modpagespeed.com/recipes/aspnet-core.md) | [Install ASP.NET Core middleware](/docs/aspnet-getting-started/)     |
| Docker (reverse proxy)    | [docker.md](https://modpagespeed.com/recipes/docker.md)           | [Install with Docker](/docs/installation-docker/)                    |
| Kubernetes (Helm chart)   | [helm.md](https://modpagespeed.com/recipes/helm.md)               | [Deploy with Helm](/docs/helm-deployment/)                           |
| Diagnose (any server)     | [diagnose.md](https://modpagespeed.com/recipes/diagnose.md)       | [Monitoring](/docs/monitoring/)                                      |

## Diagnose an installed server

When mod_pagespeed is already installed and the operator says it is not
working, or asks whether it works, an agent uses the diagnose recipe instead of
an install recipe. It reads what the server already exposes: the response
header, the worker's health socket and API, the admin pages, the counters and
the logs. It changes nothing (no purge, no restart, no configuration edit) and
ends in a short report for the operator.

## How an agent uses a recipe

1. **Prerequisites.** Pick the recipe for the server that serves the site's
   HTML and run its checks. Stop at the first failure and report it.
2. **Install.** Run the install commands as written. Privileged commands
   (`sudo`, an elevated PowerShell, `msiexec`, `docker`, `helm`) need the
   operator's go-ahead, and so does any install on a server that takes
   production traffic.
3. **Configure.** Apply the minimal configuration, replacing each
   `<placeholder>` with a value from the project.
4. **Verify.** Request the site's own page and check one response header:
   `X-Page-Speed` (nginx, IIS), `X-Mod-Pagespeed` (Apache) or `X-PageSpeed`
   (ASP.NET Core, Docker, Helm). The install is complete only when that header
   is present. Its value is `on` by default (the version only when
   `ExposeVersionInHeaders` is set, or whatever `XHeaderValue` sets), so match on
   presence, not on a value.
5. **Roll back.** If verification fails after one pass through the
   troubleshooting link, the rollback section is written to undo only what the
   run created and to leave anything that existed before in place.

## The verification marker

The verification request carries a fixed query string, `?mps-verify=agent`.
mod_pagespeed ignores the parameter. The request goes only to the server being
verified, so the marker lands only in that server's own access log, where the
operator can count agent-run verifications:

```bash
grep -c 'mps-verify=agent' /var/log/nginx/access.log
```

Nothing in a recipe reports anywhere. The recipes themselves call only the
documented install channels: the package repository, GHCR, the Helm repository,
nuget.org and the download page. apt, dnf, Docker and Helm then pull
dependencies from the sources they are configured with.

## The install skill

The `install-modpagespeed` skill wraps the recipes for agents that load skills:
it detects the server, fetches the matching recipe, runs it and runs the
verification. Its source is in the
[pagespeed-optimizer repository](https://github.com/We-Amp/pagespeed-optimizer/tree/main/.claude/skills/install-modpagespeed).

In Claude Code, add the repository as a plugin marketplace and install the
plugin:

```bash
claude plugin marketplace add We-Amp/pagespeed-optimizer
claude plugin install modpagespeed@modpagespeed
```

Claude Code can then pick the skill when you ask for a mod_pagespeed install,
or you can run it directly as `/modpagespeed:install-modpagespeed`.

For other agents that read skill directories, the `skills` command-line tool
copies the skill into the current project:

```bash
npx skills add We-Amp/pagespeed-optimizer --skill install-modpagespeed
```

An agent without skill support can be given a recipe URL directly, for example
"install mod_pagespeed by following https://modpagespeed.com/recipes/nginx.md".
