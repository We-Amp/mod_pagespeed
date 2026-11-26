---
title: 'Is mod_pagespeed safe? How its rewrites are checked'
description: 'Is mod_pagespeed safe? It rewrites your HTML, CSS, JS and images: how its rewrites are checked, and what happens when a check fails.'
date: 2026-10-19
author: 'Otto van der Schaaf'
tags: ['deep-dive', 'security', 'caching', 'architecture', 'mod_pagespeed']
draft: false
lastUpdated: 2026-11-26
---

Is mod_pagespeed safe? It rewrites the HTML, CSS, JavaScript and images your origin serves, and a wrong rewrite breaks a page or drops a security header. Old reviews call its JavaScript minification risky, and hosting docs warn of crashes. In 2.5.0, a rewrite that fails any check the optimizer runs on it is not applied: mod_pagespeed serves the response without it.

mod_pagespeed runs as a module in Apache, nginx (ngx_pagespeed) or IIS, as container images with an nginx reverse proxy, and as ASP.NET Core middleware. The Apache and nginx modules can hand in-place optimization to a separate optimizer worker ([why the worker was split out of the web server](/blog/why-i-rebuilt-mod-pagespeed/)); the IIS module runs in-process and optimizes on its own, and its installer also carries the optimizer worker as a Windows service, installed disabled ([Install on IIS](/docs/install-iis/)). Older releases versioned the module (1.15.0+rN) and worker (2.0.x) apart; 2.1.0 merged them into one product.

## Will mod_pagespeed break my site?

It can: a rewrite that fails a check is not applied, but one that passes every check can still be wrong, and two that shipped are shown below. A JavaScript parse error writes no variant ([why the JavaScript minifier parses](/blog/safe-javascript-minification-semicolon-insertion/)). A CSS failure, or an `@import` that cannot be inlined, leaves the stylesheet unchanged ([CSS @import edge cases](/blog/flatten-css-imports-edge-cases/)). The optimizer worker keeps a minified or recompressed variant only if it is smaller. Input a parser detects it cannot model, or that exceeds its bounds, passes through.

## Differential fuzzing of the CSS and JavaScript minifiers

The CSS and JavaScript minifiers each have a fuzz harness, built with ASan and UBSan, that checks two oracles.

The first is strict idempotence: `minify(minify(x)) == minify(x)`.

The second compares input with output. For CSS, input and output token streams must match on url tokens, strings, numbers and custom-property regions. For JavaScript, a line break after `return`, `throw`, `yield` and the other restricted keywords must survive. This oracle does not model every line break automatic semicolon insertion relies on, which is why idempotence matters.

The token oracle missed this fuzz-found bug; idempotence caught it:

```js
// input
let /*c*/ row
+4

// served before worker 2.0.41 and module 1.15.0+r22: no longer valid JavaScript
let row+4
```

Review of a related fix, not a fuzzer, found that `.a\35 0.5`, the class `a50`, was served as `.a5` until worker 2.0.41: wrong output that is stable, which idempotence alone cannot see.

Every pull request that touches a minifier replays its seeds through both oracles under ASan+UBSan as a blocking check. The module adds scheduled fuzzers for its CSS, HTML, JavaScript and rule-pack code, and a per-pull-request JavaScript corpus gate.

## Rewriting proxy security: escaping and CSP

On the native Apache and nginx modules the module rewrites HTML itself (since 2.3.0), and `HonorCsp`, on by default, makes script-injecting filters back off. A script carrying `integrity=` is left out of JavaScript rewriting, combining and outlining ([rewriting under a strict Content-Security-Policy](/blog/mod-pagespeed-content-security-policy/)).

In the container images and on ASP.NET Core, the HTML pipeline reads the page's Content-Security-Policy from its meta tag before injecting inline critical CSS or opt-in speculation rules. Since 2.4.0 it also reads the response header in container deployments; on ASP.NET Core the response header is never read, so with a header policy use the [bandwidth-only preset](/docs/aspnet-configuration/#bandwidth-only-rewrites). It applies CSP3 rules and injects only what every policy it reads allows. In the worker, escaping rules are stated per output context and pinned by byte-exact tests.

## In-place optimization: same URL, or the original

In-place resource optimization (IPRO) serves optimized bytes at the original URL instead of a rewritten `.pagespeed.` URL. It is on by default in the module. The trade-off: no content-hashed long-lived URL, a short shared-cache `s-maxage` by default, and no combining.

Since 2.1.0 the Debian, Ubuntu and EL9 Apache packages wire the module to the optimizer worker by default (`pagespeed_daemon.conf`), and since 2.5.0 the EL8 and EL10 packages do too. Since 2.2.0 the native nginx module does the same when you set both `pagespeed DaemonSocketPath` and `DaemonVolumePath`. How the worker path behaves (2.4.0 and later):

- If the worker is missing, incompatible or out of reach, in-place optimization turns off and the server serves normally; the module's own in-place path does not take over. Right after a cache-format upgrade the web server can instead refuse to start (below).
- A response with a header the cache cannot reproduce (`Set-Cookie`, a per-request value, a `Vary` on another axis) is served complete and not optimized in place. Security and CORS headers are replayed from a closed allow-list in container deployments, or cause the same pass-through on the native modules. On the native modules, that includes a header your server adds to every response, such as HSTS: on such a site little or nothing is optimized in place (`ipro_daemon_fallthrough` shows it; see [in-place optimization limits](/docs/installation-module/#limits)).
- The worker path never serves an image format the request did not advertise, and marks such responses `Vary: Accept`.
- A resource with a nonce-bearing CSP is not stored or optimized in place.

On the worker path, a WebP or AVIF variant below its [SSIMULACRA2 floor](/blog/ssimulacra2-image-quality-verification/), or one that cannot be measured, is declined. In the container images, a stylesheet is deferred only if the fold, rendered with just the inlined critical CSS, matches the full render ([pixel check](/docs/browser-analysis/#what-the-pixel-comparison-actually-gates)); that needs browser analysis, off by default.

## What the optimizer worker can and cannot touch

The module hands work to the worker in a notification that carries URL metadata, never a response body. The send is non-blocking and time-bounded, with versioned, size-capped frames ([fire-and-forget worker IPC](/blog/fire-and-forget-worker-ipc/)). In normal operation the web server does not depend on the worker, so a worker crash does not take pages down. After a cache-format upgrade, though, the module can refuse to attach, or the web server to start, until the worker has created its new cache; packaged units order this at boot.

On Linux hosts, since 2.1.0 the packaged worker runs as an unprivileged `pagespeed` user with an empty capability set and `NoNewPrivileges`, and its API token stays off the command line. Under the systemd unit, the filesystem is read-only except its cache and socket directories, kernel and host surfaces are closed, and core dumps, which would contain cache pages and tokens, are off. A system-call allow-list is enforced there by default. In containers the worker runs as UID 918 with no capabilities by default, under the runtime's seccomp profile instead.

The management API is off by default, and the worker refuses to start with tokenless remote access; the ASP.NET Core console is an exception, below. Headless Chrome never runs unsandboxed unless you turn the sandbox off yourself; without one, browser analysis stays off. The optimizer sends no telemetry. Its only outbound traffic is what you configure: origin fetches and any key directories you enable ([SSRF pinning](/blog/air-gapped-headless-fetch-ssrf-pinning/)). Since 2.5.0 the Enterprise Linux Apache packages (EL8, EL9 and EL10) ship an SELinux policy, installed where SELinux is enabled, that confines the worker on an enforcing host.

## Cyclone Cache: cache files as untrusted input

Cyclone, the memory-mapped cache under the module and the worker, ships inside mod_pagespeed releases. Document bytes are written before the entry that points at them ([memory-mapped zero-copy serving](/blog/memory-mapped-cache-zero-copy-serving/)). A background sync, every 30 seconds by default, caps crash loss.

The first read verifies a CRC-32C checksum (since 2.2.0; since 2.5.0 it also covers the document header's fixed fields), and the full key is re-checked on every hit, so a collision becomes a miss. Cache files are untrusted input, and an object over 64 MiB is refused by default. On a local filesystem with working byte-range locks, a process will not reset a file a live peer holds.

2.5.0 stops damaged or stale disk-cache contents being served (affected: up to and including 2.4.0; update recommended).

Named regression tests pin each invariant, with public CI on Linux, macOS and Windows.

## Hardened builds and a smaller dependency tree

Linux release builds of the worker, the library and the Apache and nginx modules carry full RELRO, `BIND_NOW`, `-fstack-protector-strong` and `_FORTIFY_SOURCE=2`, and a release-time check fails the build if one is missing. Most optimizer tests also run under ASan+UBSan+LSan and TSan on Linux; the module adds Windows ASan, Application Verifier and scheduled stress tests, and Cyclone runs nightly sanitizers and 6 libFuzzer targets.

In the Apache and nginx module builds, libcurl replaced Serf. Several unmaintained components from the last upstream third-party tree are no longer built, which shrinks the attack surface. libwebp 1.5.0 clears CVE-2023-4863, a fix the upstream 1.14.36.1 source tree predates ([1.15 hardening and dependencies](/blog/mod-pagespeed-1-1-release/)). curl 8.22.0 in 2.2.0 covers 9 curl CVEs (affected: module releases 1.1.0-beta.2 through 1.16.0, the package version of 2.1.0).

## How the work is checked, and where AI fits

The 2.5.0 image-decoder hardening added 17 regression tests, each seen failing before its fix and passing after. In one hardening pass, review found an image-input crash outside the change's scope; it shipped as a denial-of-service fix in worker 2.0.39.

AI agents do the engineering work and the code review. Reviews use multiple independent lenses using top-tier models. Claims are checked against the shipped source.

An AI reviewer is non-deterministic and has blind spots of its own. It can escalate a change but never clears a gate, and its pass is not a sign-off.

Inputs the nightly fuzzer flags are triaged by agents: replayed, minimized and root-caused. Fuzzing covers only the surfaces that have a harness, and an oracle can be wrong.

The gates are scripts: the fuzz-seed replay and binary-hardening check above, and a check that every change to product code carries a release note or a stated reason for none. Several gates have self-tests that plant a defect and expect the gate to fail. A person approves the signing and publishing of every release; agents do everything else, including site deploys, behind automated gates. Critical or challenging changes also get a person's review. [Why our releases have more gates now](/blog/why-our-releases-have-more-gates/) lists what a release passes, and the release behind one of the gates.

## Keeping mod_pagespeed safe: updates and settings to check

Security fixes are listed in one [security updates table](/docs/release-notes/#security-updates) by impact class, affected range and recommendation. Act on these first:

- Update to 2.5.0 (affected: up to and including 2.4.0). Crafted images, scripts, stylesheets or cache contents could exit a worker process, exhaust resources, read out of bounds or expose process memory; the nginx module could cache a truncated response as complete; and some responses, including ones marked private, could be cached longer than the origin allowed. The table lists the rest, including URL-signing fixes. The IIS installer on the download page is now the 2.5.0 build; an IIS host still on the 1.15.0 installer is inside the affected range.
- Before that upgrade, update any package pin, image tag, NuGet range or header match on `1.`: package versions jump from 1.19.0 to 2.5.0.
- Upgrade the module and the worker together, worker first, off-peak: the cache starts cold once and disk use doubles while versions overlap. Change explicit `v2` cache paths to `v3`.
- On the ASP.NET Core middleware 2.0.13 and later, including 2.5.0 (no fix release yet), anyone who can reach the site can use the web console and proxied worker API paths without authentication to read or change the optimizer configuration and purge the cache; 2.0.4 through 2.0.12 served the console and fewer proxied paths, also without authentication. Set `PageSpeed:Console:Enabled` to `false` in production, or put your own authentication in front of both. If the console was reachable, follow the cleanup in the [2.4.0 known issue](/docs/release-notes-2-4/).
- If you run mod_pagespeed or ngx_pagespeed 1.14.36.1 or earlier (including 1.13.35.2), from upstream or a third-party package, that lineage predates fixes such as CVE-2023-4863. Ask your vendor which of those fixes it carries, or move to a maintained line ([why 1.13.35.2 gets no security updates](/blog/mod-pagespeed-deprecated-2026/), [maintained alternatives](/blog/mod-pagespeed-alternatives/)).

For day-to-day operation:

- Test on staging. If a page breaks, compare with `?PageSpeed=off`, disable the filter with `DisableFilters` and purge the URL ([troubleshooting](/docs/troubleshooting/)).
- Keep the cache on a local filesystem with working byte-range locks, not NFS or overlayfs.
- Lock down admin endpoints, domain authorization and HTTPS with the [security hardening guide](/docs/security/).
- Report a suspected vulnerability through the [security policy](/security/).
