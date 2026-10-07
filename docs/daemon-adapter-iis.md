# Running PageSpeed for IIS with the optimizer

The IIS package can hand in-place resource optimization to the PageSpeed
optimizer, a separate Windows service. The module records each eligible
resource it serves; the optimizer builds optimized variants of it (smaller
images in newer formats, minified stylesheets and scripts) into a cache volume
on disk; the module then answers later requests for that resource from the
volume, choosing the variant that fits each client.

The package installs the optimizer as the Windows service
`WeAmpPageSpeedOptimizer` and leaves it **disabled**. Until you enable it and
set the two directives below, PageSpeed for IIS behaves exactly as it does
without it.

## What the package installs

| Item | Location |
|---|---|
| The optimizer | `C:\Program Files\We-Amp\PageSpeed\factory_worker.exe`, service `WeAmpPageSpeedOptimizer` (display name "We-Amp PageSpeed Optimizer"), start type Disabled |
| The client library the module uses to reach it | `C:\Program Files\We-Amp\PageSpeed\pagespeed.dll` |
| The cache volume directory | `C:\ProgramData\We-Amp\PageSpeed\optimizer\` |
| The optimizer's log | `C:\ProgramData\We-Amp\PageSpeed\logs\optimizer.log` (one JSON object per line) |
| The optimizer's licence and notices | `optimizer-LICENSE.txt`, `optimizer-NOTICE.txt` beside the module |

The service runs under its own virtual account,
`NT SERVICE\WeAmpPageSpeedOptimizer`: no password, not an administrator, and
the rights on the volume and log directories the package grants it on top of
what any service account has. The volume directory is owned by SYSTEM and
inherits nothing from `C:\ProgramData`: the service's account may modify it,
and `IIS_IUSRS` may modify the files in it (the IIS worker processes read the
volume and record into it) but can only list and create files in the
directory itself. If a directory of that name already exists when the package
first installs the optimizer, including when you reinstall after an
uninstall, it is moved aside to `optimizer.pre-install-<number>` and a fresh
one is made: the optimizer starts with an empty volume and rebuilds it as
traffic arrives. Delete the moved-aside directory when you no longer want it.
If the install fails at that step, a program has a file in `optimizer\`
open: close it, or rename the directory yourself, and run the installer
again. If the service stops unexpectedly, Windows restarts it
after one minute, then after one more minute, then after five minutes; the
count resets after a day.

## Turning it on

1. **Start the optimizer and make it start with Windows.** From an elevated
   prompt:

   ```
   sc.exe config WeAmpPageSpeedOptimizer start= auto
   sc.exe start WeAmpPageSpeedOptimizer
   ```

   Within a few seconds the service reports `RUNNING` (`sc.exe query
   WeAmpPageSpeedOptimizer`) and `optimizer.log` has a line saying the
   optimizer is starting.

2. **Point the module at it.** Add both directives to the machine-wide
   `C:\ProgramData\We-Amp\PageSpeed\pagespeed.config` (every site), or to a
   site's own `pagespeed.config` (that site only):

   ```
   pagespeed DaemonSocketPath pagespeed-optimizer
   pagespeed DaemonVolumePath "C:\ProgramData\We-Amp\PageSpeed\optimizer\cache"
   ```

   Set both or neither: one alone leaves in-place optimization off for the
   site. `DaemonVolumePath` must not be the module's own `FileCachePath`.

3. **Recycle the application pools** (or run `iisreset`) so the worker
   processes read the new configuration. Start the optimizer before the
   application pools: with the two directives set, a site whose worker process
   finds the optimizer not answering when it starts has in-place optimization
   off until its pool recycles (the classic in-place path is not used while
   the directives are set).

### Checking it works

Request a stylesheet or an image a page uses, wait a few seconds, and request
it again. The second response carries a weak ETag beginning `W/"ps-` and an
`Age` header: it came from the optimizer's volume. The module's statistics
page (`/pagespeed_admin/statistics`, local requests only) counts these as
`ipro_daemon_served`, and requests the volume could not answer yet as
`ipro_daemon_fallthrough`.

If a site does not engage, the module says why: see the
`X-Pagespeed-Init-Status` values and the event-log entry described in
[IIS limitations](iis-limitations.md).

### Compressed copies

The optimizer also stores a gzip and a brotli copy of each optimized
stylesheet, script and SVG image. On IIS the module serves the uncompressed
optimized copy and IIS compresses it as its own compression settings say;
sending the stored copies directly (`DaemonServeStoredEncodings` on Apache and
nginx) is not available on IIS in this release. If you do not need them for
another server, setting the optimizer's `gzip_level` and `brotli_level` to `0`
stops it writing them.

## The cache size

The volume is 1 GiB unless you choose otherwise. The size belongs to the
optimizer; the module reads it from the volume and needs no setting of its own.
Choose it when you install or upgrade:

```
msiexec /i pagespeed-iis-<version>-win-x64.msi OPTIMIZERCACHESIZE=4294967296
```

The package remembers the value for later upgrades and repairs. To change the
size of a volume that already exists, stop the optimizer, remove the old
volume and apply the new size (the package starts an optimizer set to start
automatically again), then recycle the application pools, in that order:

```
sc.exe stop WeAmpPageSpeedOptimizer
del C:\ProgramData\We-Amp\PageSpeed\optimizer\cache-*
msiexec /i pagespeed-iis-<version>-win-x64.msi REINSTALL=ALL REINSTALLMODE=vamus OPTIMIZERCACHESIZE=<bytes>
iisreset
```

Changing the size without removing the old volume leaves the module attached
to a volume the optimizer no longer writes, and the sites stop being served
from it until the steps above are done. Removing the volume discards the
optimized variants; they are rebuilt as traffic arrives.

## Upgrades and repairs

An upgrade or repair keeps what you chose: an optimizer you set to start
automatically is started again, before IIS comes back, and a remembered cache
size is kept (also across an uninstall and reinstall; the volume itself is
not reused after an uninstall, see above). When a release moves to a new on-disk cache format, the optimizer
creates a new volume beside the old one, so the volume directory needs free
space for a second volume during the change; the release notes say when that
applies.

## Logs

The optimizer appends to `optimizer.log` for as long as it runs; the file is
not rotated. To start a fresh log, stop the service, rename or delete the
file, and start the service again.

## Turning it off

Remove the two directives, recycle the application pools, then stop and
disable the service:

```
sc.exe stop WeAmpPageSpeedOptimizer
sc.exe config WeAmpPageSpeedOptimizer start= disabled
```

Uninstalling the package removes the service and its program files. The
volume directory and its contents are kept, like the module's own cache;
delete `C:\ProgramData\We-Amp\PageSpeed\optimizer\` yourself if you no longer
want them. A later reinstall does not reuse them (see above).
