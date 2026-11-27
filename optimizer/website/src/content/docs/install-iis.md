---
title: 'Install on IIS'
description: 'Install the mod_pagespeed IIS module on Windows Server: requirements, the signed MSI, IIS Express, the optional optimizer service and the first check.'
order: 7
group: 'Install'
lastUpdated: 2026-10-20
datePublished: 2026-10-08
faq:
  - q: 'Can the IIS module and IISpeed run on the same host?'
    a: 'No. Both register the same handler in IIS and one or both fail at site startup. Uninstall IISpeed, run `iisreset`, then run the MSI. Your IISpeed configuration files keep working.'
  - q: 'Does the IIS installer include the optimizer worker?'
    a: 'Yes, from the 2.5.0 installer on, as the Windows service `WeAmpPageSpeedOptimizer`, installed disabled. Until you turn it on and point the module at it with `DaemonSocketPath` and `DaemonVolumePath`, the module optimizes on its own exactly as before.'
---

mod_pagespeed runs on IIS as a native, in-process module: the successor to
IISpeed. Download the signed MSI, run it, `iisreset`, and the module optimizes
every site that has a `pagespeed.config`. The IIS package ships as a signed
installer on the mod_pagespeed GitHub release.

Machine-readable recipe for coding agents: [modpagespeed.com/recipes/iis.md](https://modpagespeed.com/recipes/iis.md) (see [Install with a coding agent](/docs/agent-install/)).

## Requirements

- Windows Server 2016 or later, with IIS 10 (the release is tested on Windows Server 2016)
- 64-bit only (the Visual C++ runtime is built in; no redistributable is needed)
- The IIS worker process identity needs write access to the cache directory;
  the installer grants it for the default directory

## Install the module {#install-the-module}

:::warning[Maintenance window]
Running the MSI — to install, upgrade, repair or uninstall the module — takes
every IIS site on the server down for the duration, not only the sites it
optimizes. The installer stops the Windows Process Activation Service (WAS)
and the World Wide Web Publishing Service (W3SVC), force-kills any `w3wp.exe`
worker process still running, and starts IIS again when it is done; anything
else that depends on WAS, such as the WCF non-HTTP activation services
(net.tcp, net.pipe, net.msmq), is stopped with it and not restarted. In-flight
requests are dropped. Plan a maintenance window.
:::

1. Download the [IIS MSI installer](/download/#direct-downloads) from the Download
   page's Windows installer link.
2. Check it. On Windows, the MSI's Authenticode signature is the check for
   the installer itself: its publisher must be "We-Amp B.V."
   (`Get-AuthenticodeSignature .\pagespeed-iis-<version>-win-x64.msi` reports
   `Valid`, with a signer certificate subject starting `CN=We-Amp B.V.`). The release also carries `SHA256SUMS` and
   `SHA256SUMS.asc`, a GPG signature of the checksum file made with the
   [release signing key](https://github.com/We-Amp/mod_pagespeed/blob/master/RELEASE-SIGNING-KEY.asc),
   fingerprint `411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313`: the
   signature verifies `SHA256SUMS`, and `SHA256SUMS` then verifies the MSI.
   The steps are in
   [Verifying a Release](https://github.com/We-Amp/mod_pagespeed/blob/master/SECURITY.md#verifying-a-release).
   This key is not the one that signs the apt and dnf repositories.
3. Run it on the Windows Server host. The installer registers the module in
   IIS as a native HTTP module and creates the default cache directory at
   `%ProgramData%\We-Amp\PageSpeed\Cache`.
4. Run `iisreset`.

The module reads `pagespeed.config`. A site is optimized only when its own
physical root holds one; the server-level file at
`%ProgramData%\We-Amp\PageSpeed\pagespeed.config` supplies the defaults for
those sites and turns no site on by itself. The installer seeds a per-site copy
in the Default Web Site root. The minimal file is two lines:

```text
pagespeed on
pagespeed FileCachePath %ProgramData%\We-Amp\PageSpeed\Cache
```

[IIS configuration](/docs/iis-configuration/) documents the file format, the
lookup order and path matching.

### Coming from IISpeed {#migrating-from-iispeed}

If IISpeed is installed on this host, **uninstall it before running the MSI**.
The two modules register the same handler in IIS and cannot coexist. Open
_Apps & features_ (or _Programs and Features_), remove "IISpeed", run
`iisreset`, then run the installer.
[Migrate from IISpeed](/docs/migrate-from-iispeed/) walks through the whole
move, configuration and cache included.

### The optimizer service {#the-optimizer-service}

From 2.5.0 on, the installer also installs the optimizer worker, as the Windows
service `WeAmpPageSpeedOptimizer` under its own virtual account, and leaves it
**disabled**: until you turn it on, the module behaves exactly as it does
without it. Turned on and pointed at with the `DaemonSocketPath` and
`DaemonVolumePath` directives, it takes over in-place optimization: the module
records each eligible resource, the optimizer builds optimized variants, and
the module serves the variant that fits each client. An upgrade or repair with
the installer keeps a service you turned on running and keeps the cache size
you chose (`OPTIMIZERCACHESIZE`, 1 GiB by default).

To turn it on:

1. Set the service to start, and start it, from an elevated prompt:

   ```powershell
   sc.exe config WeAmpPageSpeedOptimizer start= auto
   sc.exe start WeAmpPageSpeedOptimizer
   ```

   Use `start= demand` instead of `start= auto` to start it only by hand. The
   service logs to `C:\ProgramData\We-Amp\PageSpeed\logs\optimizer.log`; once
   it is up, the log says it is listening on `\\.\pipe\pagespeed-optimizer`.

2. Point the module at it. Add these two lines to the server-level
   `C:\ProgramData\We-Amp\PageSpeed\pagespeed.config`:

   ```
   pagespeed DaemonSocketPath pagespeed-optimizer
   pagespeed DaemonVolumePath "C:\ProgramData\We-Amp\PageSpeed\optimizer\cache"
   ```

   `DaemonSocketPath` is the pipe name the service listens on, without the
   `\\.\pipe\` prefix; the module adds it. `DaemonVolumePath` is the cache
   volume's path stem, `…\optimizer\cache`: not the `optimizer` directory the
   service is given as its cache directory, and not the `cache-…` file the
   service creates there.

3. Run `iisreset`.

The application pools reach the pipe and the cache with the permissions the
installer sets; no extra access rules are needed. Start the service before the
application pools: the module attaches to the optimizer when a pool starts, so
if the service was stopped then, or you start or restart it later, run
`iisreset` again for the module to use it (the module does not reattach on its
own yet; tracked in
[mod_pagespeed#104](https://github.com/We-Amp/mod_pagespeed/issues/104)). To turn the optimizer off, remove the
two lines and run `iisreset`; the module goes back to optimizing on its own.

### IIS Express

For local development with IIS Express, add the module to
`applicationhost.config` in `%userprofile%\Documents\IISExpress\config\`:

1. Copy the module DLL to a known location
2. Add the module registration under `<globalModules>` and `<modules>`
3. Restart IIS Express

## Verify it works

On IIS the module emits `X-Page-Speed`, the same header as the nginx module.
Check for it with PowerShell:

```powershell
(Invoke-WebRequest http://localhost/ -UseBasicParsing).Headers["X-Page-Speed"]
```

Then open `/pagespeed_global_admin` on the server itself.
[Is it working?](/docs/is-it-working/) has the other checks.

## Next steps

- [IIS configuration](/docs/iis-configuration/): `pagespeed.config`, lookup
  order, path matching and IIS tuning
- [Filter selection](/docs/filter-selection/): choosing and tuning filters
- [Admin console](/docs/admin-console/): restricting the admin pages
- [Migrate from IISpeed](/docs/migrate-from-iispeed/)
