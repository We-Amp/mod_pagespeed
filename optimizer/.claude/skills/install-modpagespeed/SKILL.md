---
name: install-modpagespeed
description: Install mod_pagespeed on the web server a project uses (nginx, Apache, IIS, ASP.NET Core, Docker or Helm) by running the matching machine-readable recipe from modpagespeed.com, then verify it with the response-header check; or diagnose an installed one with the read-only diagnose recipe. Use when asked to install, enable or verify mod_pagespeed or PageSpeed optimization, to make an nginx, Apache, IIS or ASP.NET Core site faster with it, or to diagnose or troubleshoot an installed module or worker ("is it working").
---

# Install mod_pagespeed

If mod_pagespeed is already installed and the question is whether it works, go
straight to section 4; sections 1-3 are for installing.

The recipes are at `https://modpagespeed.com/recipes/<surface>.md`; the index
is `https://modpagespeed.com/recipes/README.md`. In a checkout of the
pagespeed-optimizer repository the same files are under
`website/public/recipes/`; use the local copy when it exists, otherwise fetch.

## 1. Pick the surface

| Signal                                                                                  | Surface       |
| --------------------------------------------------------------------------------------- | ------------- |
| `nginx -v` works and the site is served by nginx on Debian, Ubuntu or Enterprise Linux   | `nginx`       |
| `apache2ctl -v` or `httpd -v` works (the recipe routes cPanel hosts to `cpanel`)   | `apache`      |
| `/usr/local/cpanel` exists (cPanel / WHM, EA4 Apache)                                    | `cpanel`      |
| Windows Server with IIS (`HKLM:\SOFTWARE\Microsoft\InetStp` exists)                      | `iis`         |
| A `.csproj` using `Microsoft.NET.Sdk.Web`                                               | `aspnet-core` |
| Docker is available and the origin is a container or is not Apache, nginx or IIS        | `docker`      |
| `helm version` works and the origin is a Kubernetes Service                             | `helm`        |

If more than one applies, ask which site to optimize: the module goes where the
HTML is served. If none applies, stop and say so.

## 2. Fetch and follow the recipe

```bash
curl -fsSL --proto '=https' https://modpagespeed.com/recipes/<surface>.md
```

In Windows PowerShell use `curl.exe` (plain `curl` there is an alias of
`Invoke-WebRequest`):

```powershell
curl.exe -fsSL --proto '=https' https://modpagespeed.com/recipes/<surface>.md
```

Run its sections in order: prerequisites, install, minimal configuration,
verify. Stop at the first failed prerequisite and report it. Replace the
`<placeholders>` with values from the project; do not change package names,
URLs or directives.

Rules for an install:

- Run only the fenced commands of the recipe's sections 1-5.
- Fetch recipes, documentation and downloads yourself only from
  modpagespeed.com, packages.modpagespeed.com, ghcr.io and api.nuget.org.
  Package managers, Docker and Helm then pull dependencies from the sources
  the recipe configures (distribution mirrors, GHCR, the Helm repository).
- Before the first privileged command, show every `sudo`, administrator
  (elevated PowerShell), `msiexec`, `helm` and `docker` command the recipe will
  run and get one go-ahead from the operator.
- Installing on a server that serves production traffic needs the operator's
  explicit go-ahead first.
- The prerequisites check that nothing named `pagespeed` exists yet (container,
  Compose project, Helm release, package). If it does, or if an install step
  fails because the thing already exists, stop and ask.

## 3. Verify

The recipe's verification request carries the fixed query string
`?mps-verify=agent` and checks one response header (`X-Page-Speed`,
`X-Mod-Pagespeed` or `X-PageSpeed`, as the recipe states). Report the exact
header line. The install is done only when that line is present; a missing
header is a failure, not a warning. For `X-PageSpeed`, `MISS` is a pass; `HIT`
is informational. Retry the check at most 3 times.

## 4. Diagnose an installed module or worker

Signal: mod_pagespeed is already installed and not working, or the operator
asks whether it works. Do not run an install recipe; fetch the diagnose recipe:

```bash
curl -fsSL --proto '=https' https://modpagespeed.com/recipes/diagnose.md
```

Run its sections 1-6 in order and give the operator the filled report from
section 6. The install rules above do not apply: something named `pagespeed`
is expected to exist. The recipe is read-only: never purge a cache, send a
`PATCH` or `POST`, restart a service, edit configuration or raise the log
level; stop and ask before anything that changes state. It contacts only the
operator's own server and loopback.

## 5. On failure

Follow the troubleshooting link in the recipe once. If that does not resolve
it, run the rollback section of an install recipe and report what was tried,
the server's error-log lines and the exact command that failed. Roll back only
what this run created; leave anything that existed before untouched. Never
send logs or configuration anywhere; the recipes make no calls beyond the
documented install channels.
