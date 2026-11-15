# Changelog

Entries for 2.0.x predate the relicensing of the product under the Apache License 2.0; their license and trial wording describes the behavior of those releases.

## Unreleased

### Cached responses come from an anonymous request

On a cache miss, a response that would be cached is fetched once more as an anonymous request (no signed-in user, cookies or credentials, from the client address `224.0.0.0`); only that response is stored, and the visitor still gets its own. The app therefore handles each response that gets cached twice. Put rules that admit clients by address or identity after `UsePageSpeed()`, or list their paths in `ExcludePaths`. Call `UseRouting()` after `UsePageSpeed()`: an endpoint selected ahead of it is cached only when nothing about its selection can depend on the client. For an app whose GET handlers have side effects, set the new `RecordResponses` option to `false`: pages are still optimized, and nothing new is cached.

### Disabled at startup, enabled later

While the top-level `Enabled` switch (`PageSpeed:Enabled`) is `false`,
`AddPageSpeed` no longer opens the Cyclone cache volume and `WorkerProcessHost`
no longer launches `factory_worker`. Previously the 1 GiB volume file was
created, and the worker launched whenever `Worker.AutoStart` was `true`, even
while the middleware passed every response through.

The first time `Enabled` is `true`, at startup or through a hot reload, both
start: the cache volume is opened and the worker is launched, with no restart
needed, and the next HTML request is optimized. Setting `Enabled` back to
`false` leaves the open cache and the running worker in place. If the volume
cannot be opened at that point, pages are still served, optimized but not
cached, the health check reports `Unhealthy`, and the failure is logged once
rather than per response; the open is retried at the next configuration
change.

While no worker runs, its management API is not running either, so the console
has no worker to show and its `/v1/*` paths answer `503`; the console needs
`Enabled` set to `true`. Until PageSpeed has been enabled, the health check
reports `Healthy` with the description `PageSpeed is disabled`.
`Worker.AutoStart` `false` still means "do not launch a child", and an external
worker's `Worker.SocketPath` is still used whatever `Enabled` says.

## 2.0.20

### Middleware read-back

The `ReadResult.OriginCcFlags` property now exposes the origin Cache-Control
directives as a bitfield (PS_CC_ORIGIN_*), and `ReadResult.RevalidationRequired`
correctly derives the revalidation requirement from these flags. For a private
cache (ASP.NET in-process), only `no-cache` and `must-revalidate` trigger
revalidation; `proxy-revalidate` and `s-maxage` are ignored per RFC 9111
§5.2.2.9-10.

The middleware write path now persists the origin Cache-Control state using
`ps_parse_cache_control`, so cache entries carry the true origin-revalidation
signal and aggressive-mode `stale-if-error` is correctly suppressed when the
origin requires revalidation.

### Constants

Added `CacheControlCcFlags` class with all 14 PS_CC_ORIGIN_* bit constants
(`NoCache`, `MustRevalidate`, `NoStore`, `Private`, `Public`, `Immutable`,
`SMaxagePresent`, `ProxyRevalidate`, `NoTransform`, `HeaderPresent`,
`PrivateQualified`, `PrivateBare`, `NoCacheQualified`, `NoCacheBare`).

## 2.0.19

README alignment with the always-functional licensing model.
Docs-only - no middleware or worker code change versus 2.0.18.

### README

The on-NuGet README still described the previous model ("no-op until a trial
or license is activated"). It now matches shipped behavior: optimization runs
out of the box, unlicensed responses carry an `X-PageSpeed-Warn: unlicensed`
header, and production use requires a license (the software never
locks you out). No behavior change.

## 2.0.18

Product enhancements and changes around license handling. See the release
notes at https://modpagespeed.com/.

## 2.0.17

Various bug fixes and enhancements. No public API change versus 2.0.16.

## 2.0.16

README clarity release. Docs-only - no middleware or worker code change
versus 2.0.15.

### README

The on-NuGet README implied two different things about a fresh install:
the Quick Start said "HTML and image optimization are on out of the box,"
while the trial step noted (lower down) that "without a license token,
requests pass through unchanged." A reader hit the first claim before the
second. The README now states up front - in the lead, at the end of the
setup step, and at the top of the trial step - that the middleware is a
no-op until a trial or license is activated, and that the worker
auto-start ("out of the box") refers to the plumbing, not to optimization
being applied. No behavior change.

## 2.0.7

NuGet page + build-metadata polish. No functional change to the
middleware or worker versus 2.0.6.

### README

The on-NuGet README now links to modpagespeed.com in the lead paragraph,
the activation step, and the Links section.

### Build metadata (PDBs + nuget.info health flags)

`samples/aspnetcore/Directory.Build.props` now sets `<Deterministic>true</Deterministic>`
and `<ContinuousIntegrationBuild Condition="'$(CI)' == 'true'">true</ContinuousIntegrationBuild>`.
Two effects:

- The **Non deterministic** and **Compiler Flags: Missing** checks on
  [nuget.info](https://nuget.info/packages/WeAmp.PageSpeed.AspNetCore)
  flip green.
- Absolute build paths no longer get embedded in shipped PDBs.

The third nuget.info health flag, **Source Link: Missing Symbols**,
remains intentionally red - turning Source Link on would advertise commit SHAs and per-file paths in
PDBs that consumers could not resolve at the time.

## 2.0.6

Republish of 2.0.5 with a corrected worker version string. The v2.0.5
hotfix bumped `<Version>` in `Directory.Build.props` but missed the
matching `kPageSpeedVersion` in `src/product_version/version.h` and
the top-level `VERSION.txt`, so the bundled `factory_worker` reported
`version: "2.0.4"` on `/v1/health` (and in the heartbeat telemetry that
feeds the compliance report) on every 2.0.5 install.

No functional change to the trial/activation flow versus 2.0.5 - this
is purely a version-string + provenance correction. The three sources
of truth are now pinned together by a new xUnit invariant test
(`VersionInvariantTests`) that runs on every PR.

## 2.0.5

Hotfix - trial + paid activation were both broken on a stock 2.0.4 install.
Verified end-to-end against the live license service.

### What was broken

- `WorkerProcessHost` never passed `--license-renewal-url` to the worker
  and `WorkerOptions` had no property for it. Every `POST /v1/license/trial`
  and `/v1/license/activate` returned `503 License service URL not
  configured` - the upstream call was never even attempted.
- The NuGet `factory_worker` correctly stamps `server: "aspnetcore"` on its
  tracking metadata (set by `--copt=-DPAGESPEED_SERVER='"aspnetcore"'` in
  the build), but the license-service allow-list was not updated alongside, so
  even with the URL wired up upstream returns `400 invalid server:
  aspnetcore`.

### Fix

- New `WorkerOptions.LicenseRenewalUrl` (default
  `https://api.modpagespeed.com`). Override only for staging/self-hosted
  license services. Empty disables every license-proxy endpoint.
- Matching license-service allow-list update shipped in lockstep

## 2.0.4

The in-app workbench is back, served by the middleware. Trial activation
works out of the box - `dotnet add package`, `dotnet run`, navigate to
`/console/`, enter your email, get a 14-day trial token.

### Console + license endpoints on the app port

- `GET /console/*` - the SvelteKit workbench SPA is now embedded in
  `WeAmp.PageSpeed.AspNetCore.dll` and served by the middleware. No more
  Worker.ApiPort setup, no extra port to expose, no files in `wwwroot/`.
  The mount path is configurable via `Console.MountPath` (default `/console`).
- `POST /v1/license/trial`, `POST /v1/license/apply`, and related license
  endpoints are now proxied through the middleware to the worker. CSRF gate
  preserved (`Content-Type: application/json` + `X-Requested-With:
  XMLHttpRequest` still required).
- `Worker.ApiPort` semantics changed from "0 = disabled" to "0 = auto-pick
  a loopback ephemeral port at startup". Explicit values still bind on the
  configured port. The worker HTTP API is never exposed externally unless
  you opt in.

### Metadata + packaging

- NOTICE and THIRD-PARTY-NOTICES are now packed in every `.nupkg`. Apache-2.0
  attribution for the statically-linked LLVM libc++/libc++abi/libunwind is
  now in THIRD-PARTY-NOTICES.
- Package icon, release notes, and Core-Web-Vitals-led description are now
  set on the nuget.org listing.
- RepositoryUrl is properly stripped from the .nupkg metadata (SourceLink
  auto-detection disabled).

### Security note

The `/console/` workbench and `/v1/license/*` proxy are served on the same
origin as your app. The worker's CSRF gate blocks cross-origin form posts
but does not protect against same-origin XSS / supply-chain scripts that can
drive `POST /v1/license/trial`. Set `Console.RequireHttps = true` in
production, gate `/console/` at your reverse proxy if you can't trust every
script loaded into the app, and use `Console.MountPath` to move the path
off a guessable location. A one-time CSRF token for trial activation is
planned for a follow-up release. See README "Security" section for details.

### Telemetry attribution

- The bundled `factory_worker` now reports `server=aspnetcore` and
  `distribution=nuget` in heartbeats (was `nginx`/`source`), so compliance
  reports can segment NuGet installs as a distinct distribution channel.
  All four RIDs (linux-x64, linux-arm64, osx-arm64, win-x64) are stamped
  consistently via `PAGESPEED_SERVER` / `PAGESPEED_DISTRIBUTION` copts.
- `WeAmp.PageSpeed.NativeAssets.*` packages now embed a `BUILD_INFO.json`
  at `runtimes/<rid>/native/BUILD_INFO.json` with `git_sha`,
  `git_sha_short`, `build_timestamp_utc`, and `rid`. Supports
  correlating compliance-report heartbeats with a specific package build
  and verifying provenance without unpacking the `.nupkg`.
- `/v1/health` now emits `expired` alongside `valid` in the license block,
  matching the field the middleware reads. The middleware logs a one-shot
  warning if the field is absent, which indicates a NativeAssets package
  pinned to a pre-2.0.4 worker (in which case expiry enforcement
  silently collapses to valid-only - bump NativeAssets to match).

## 2.0.3

Hotfix for 2.0.2's broken linux-x64 binary (GLIBC_2.38 baseline → failed
on RHEL 9 / Debian 12 / Ubuntu 22.04). **2.0.2 has been unlisted from
nuget.org**; pinned references should bump to 2.0.3.

Root cause: `release.yml`'s linux-build job used a static
`pagespeed2-dev:latest` Docker tag instead of the content-hashed image
mechanism. The new Ubuntu 22.04 build environment from 2.0.2 never
reached the linux-x64 binary. Fixed in this release; future Dockerfile
changes will auto-propagate.

linux-arm64, osx-arm64, and win-x64 in 2.0.2 were correct.

## 2.0.2

Packaging-correctness release. `dotnet add package WeAmp.PageSpeed.AspNetCore`
followed by `dotnet run` now works end-to-end - no `dotnet publish` required.

### Linux native binaries are now self-contained

- `WeAmp.PageSpeed.NativeAssets.Linux` ships only `libpagespeed.so` and
  `factory_worker`. libc++/libc++abi/libunwind are statically linked into
  both.
- 2.0.1 shipped these as separate `.so` files. NuGet's restore-time
  native-asset detection silently dropped `libc++.so.1` and `libc++abi.so.1`
  (filenames combining `++` with a `.so.N` version suffix hit an
  auto-detect regex edge case), so `libpagespeed.so` failed to load with
  "libc++.so.1: cannot open shared object file" on consumer machines.

### Worker

- `/v1/health` now reports the actual package version. 2.0.0 and 2.0.1
  both reported `"version": "2.0.0-beta.1"` due to a stale literal.

### LICENSE

- `Licensed Work` field changed from the pre-GA placeholder
  `PageSpeed 2.0 0.1.0` to `ModPageSpeed 2.0`.
- `Change Date` corrected to `2030-05-17` (four years from 2.0.0 GA).

## 2.0.1

Packaging and metadata fixes on top of 2.0.0. No runtime behavior changes.

### Packaging

- `WeAmp.PageSpeed.AspNetCore` now transitively pulls in the matching
  `WeAmp.PageSpeed.NativeAssets.*` package via `WeAmp.PageSpeed`. Consumers
  install a single package and the native binaries for their RID resolve
  automatically.
- `WeAmp.PageSpeed.NativeAssets.*` packages no longer ship
  `runtimes/<rid>/native/console/index.html`. The 2.0.0 stub extracted to the
  publish-output root and collided with consumer assets; the workbench SPA
  will be repackaged in a follow-up release. The `POST /v1/license/trial`
  endpoint, served directly by the middleware, is the supported trial
  activation path for 2.0.1.

### Metadata

- Package `projectUrl` now points at the ASP.NET Core feature section on
  modpagespeed.com.
- Removed `<RepositoryUrl>` from package metadata.
- README rewritten: simpler install (single package), explicit trial
  activation step, accurate description of worker IPC (Unix domain sockets on
  Linux/macOS, Windows Named Pipes on win-x64 - earlier text incorrectly said
  "HTTP polling fallback").

## 2.0.0

First General Availability release of `WeAmp.PageSpeed.AspNetCore`. Bumps the
NuGet package version from `2.0.0-preview.1` to `2.0.0`.

### Distribution

- Signed packages via Microsoft Trusted Signing (Public Trust certificate).
- nuget.org Trusted Publishing - short-lived OIDC credentials, no long-lived
  API key in the repo.

### Configuration reference rewritten

- Every option in `aspnet-configuration.md` now matches `PageSpeedOptions.cs`
  source. Removed previously-documented options that did not exist
  (`RewriteLevel`, `EnabledFilters`, `DisabledFilters`, `StatisticsEnabled`,
  `ConsolePath`, flat `CachePath`); added the real `Cache`, `Html`, and
  `Worker` nested-options tables.
- `Worker.ApiPort` documentation now includes an explicit security warning:
  never expose the worker management API to the public internet (no auth).

### Bug fixes

- Middleware no longer emits `stale-if-error` unconditionally; gated by the
  origin response's revalidation directives (`must-revalidate`, `no-cache`,
  `proxy-revalidate`, `s-maxage`) per RFC 9111 §4.2.4.
- Worker `SetLicenseStatus` writes are now atomic via a mutex; HTTP readers
  on `/v1/health` and `/api/status` cannot observe torn license state under
  the new hot-path enforcement.

## 2.0.0-preview.1

### Breaking Changes

- All `IPageSpeedCache` methods now require a `scheme` parameter ("http" or "https")
- IPC wire protocol bumped to v3 (incompatible with older components)
- Cache keys changed format (full invalidation on upgrade)

### Fixes

- ASP.NET middleware URL now correctly uses path+query only (no scheme://host prefix)
- Worker notification dedup key now includes scheme and hostname
