# Install recipes for coding agents

One file per server, each with the same five sections: prerequisites check,
install, minimal configuration, verification, rollback. Every command comes
from the documentation linked at the top of the recipe; the documentation stays
the source of truth and the recipe links to it instead of repeating it. Fenced
commands, placeholders in angle brackets, no prose beyond what a command needs.
The diagnose recipe is the exception: it installs nothing, so its sections
check, report and say that there is nothing to roll back.

| Server                    | Recipe                                                            | Header the verification checks |
| ------------------------- | ----------------------------------------------------------------- | ------------------------------ |
| nginx (native module)     | [nginx.md](https://modpagespeed.com/recipes/nginx.md)             | `X-Page-Speed`                 |
| Apache (native module)    | [apache.md](https://modpagespeed.com/recipes/apache.md)           | `X-Mod-Pagespeed`              |
| cPanel / EasyApache 4     | [cpanel.md](https://modpagespeed.com/recipes/cpanel.md)           | `X-Mod-Pagespeed`              |
| IIS (native module)       | [iis.md](https://modpagespeed.com/recipes/iis.md)                 | `X-Page-Speed`                 |
| ASP.NET Core (middleware) | [aspnet-core.md](https://modpagespeed.com/recipes/aspnet-core.md) | `X-PageSpeed`                  |
| Docker (reverse proxy)    | [docker.md](https://modpagespeed.com/recipes/docker.md)           | `X-PageSpeed`                  |
| Kubernetes (Helm chart)   | [helm.md](https://modpagespeed.com/recipes/helm.md)               | `X-PageSpeed`                  |

To check a server where mod_pagespeed is already installed, use
[diagnose.md](https://modpagespeed.com/recipes/diagnose.md): read-only checks of
the response header, the worker's health and API, the admin pages and the logs,
ending in a report for the operator.

In a checkout of the repository the files are under `website/public/recipes/`.
The `install-modpagespeed` skill picks the recipe for the detected server and
runs it, verification included. Source:
https://github.com/We-Amp/pagespeed-optimizer/blob/main/.claude/skills/install-modpagespeed/SKILL.md

Install it in Claude Code:

```bash
claude plugin marketplace add We-Amp/pagespeed-optimizer
claude plugin install modpagespeed@modpagespeed
```

Install it into the current project for other agents that read skill
directories:

```bash
npx skills add We-Amp/pagespeed-optimizer --skill install-modpagespeed
```

The human-readable overview is https://modpagespeed.com/docs/agent-install/.

## Conventions

- `<site-url>` is the scheme and host the site answers on, for example
  `http://localhost` or `https://www.example.com`.
- Linux commands run as a user with `sudo`; the IIS recipe runs in an elevated
  PowerShell.
- Stop at the first failed prerequisite and report it; do not work around it.
- A recipe is complete only when its verification step passes. The diagnose
  recipe has no verification step; it is complete when its report is filled.
- Nothing in a recipe reports anywhere. The recipes themselves call only the
  documented install channels: the package repository, GHCR, the Helm
  repository, nuget.org and the download page. apt, dnf, Docker and Helm then
  pull dependencies from the sources they are configured with. The diagnose
  recipe contacts only the server it checks and loopback.

## The verification marker

Every verification step requests the installed site's own page with a fixed
query string:

```text
?mps-verify=agent
```

The module ignores the parameter. It is there so that whoever runs the server
can count agent-run verifications in the access log the server already writes.
The request goes only to the server being verified, so marker hits land only
in that server's own access log; nobody else sees them.

```bash
grep -c 'mps-verify=agent' /var/log/nginx/access.log     # nginx
grep -c 'mps-verify=agent' /var/log/apache2/access.log   # Apache on Debian/Ubuntu
grep -c 'mps-verify=agent' /var/log/httpd/access_log     # Apache on Enterprise Linux
```

On IIS the marker is in the `cs-uri-query` field of the W3C log under
`%SystemDrive%\inetpub\logs\LogFiles\`.
