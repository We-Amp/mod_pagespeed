---
title: 'SSRF protection for headless rendering: pinned fetches'
description: 'SSRF protection for headless rendering: the optimizer worker forces Chrome offline and routes subresources via an IP-pinned fetch, checked per redirect.'
date: 2026-06-14
author: 'Otto van der Schaaf'
tags: ['architecture', 'security', 'headless-chrome', 'agentic', 'deep-dive']
draft: false
product: '2.0'
lastUpdated: 2026-10-22
---

Rendering an untrusted page in a real browser is a server-side request forgery primitive handed to whoever wrote the page. The HTML can point a stylesheet at `http://169.254.169.254/latest/meta-data/iam/security-credentials/`, an XHR at an internal admin port, or a script at a hostname that resolves to RFC1918 space. If the browser's own network stack fetches those, the page has read your cloud metadata endpoint through your renderer. SSRF protection for headless browser rendering is therefore not a feature you bolt on; it is the precondition for letting Chrome touch attacker-controlled markup at all.

The mod_pagespeed optimizer worker has two answers to this, and this post is about the second one. The legacy perf-measurement render (covered in [/blog/headless-lcp-cls-measurement/](/blog/headless-lcp-cls-measurement/) and [/blog/remove-unused-javascript-chrome-coverage/](/blog/remove-unused-javascript-chrome-coverage/)) forces Chrome offline and serves only bytes the worker already cached, blocking everything else. That is deny-by-default with no egress at all. The [`agent_optimize` render](/blog/serve-markdown-to-ai-agents/) needs more: to produce faithful markdown for AI crawlers it has to actually fetch some subresources. So it keeps Chrome's network off and routes every paused request through an out-of-process, IP-pinned fetch that re-runs the full origin and SSRF policy on each redirect hop. The code lives in `src/browser/agent_fetcher.cc`, `lib/net/fetch_policy.cc`, `lib/net/upstream_pin.cc`, and `lib/net/ssrf_guard.cc`.

## SSRF protection for headless rendering: Chrome's network stays off

The render session in `src/browser/page_analysis.cc` sets `Network.emulateNetworkConditions` with `offline: true` and enables `Fetch.enable` with a catch-all `{urlPattern: "*"}`. Both `Send` calls are commented "SSRF defense — MUST succeed": if either fails the session aborts. The result is that no subresource request leaves Chrome's own stack. Every one of them arrives at `HandleFetchRequest` as a `Fetch.requestPaused` event.

That handler is the fork in the road. A cache hit is fulfilled from the worker's in-memory bytes. A 3xx response-stage event is failed outright, because Chrome must never follow a redirect itself — the fetcher follows them under its own policy. And only when `agent && agent->policy` is set does a paused request get scheduled for an out-of-process fetch. With no policy active, the handler's final line is `FailRequest(request_id)`, the legacy block-everything behavior. So the policed-egress path is gated twice: by `config_.agent_optimize` in `browser_analysis_manager.cc` (which decides whether `RunAgentRender` constructs an `AgentRenderOptions` at all) and by the `agent->policy` null check at the request site.

When the policy is active, `ScheduleAgentFetch` pushes the work onto the libuv work pool via `uv_queue_work`. The blocking fetch runs in `AgentFetchWorkCb` off the loop thread; `AgentFetchAfterCb` comes back on the loop thread, re-locks a `weak_ptr<Session>`, and rechecks `completed` before touching CDP — the page may have finished or Chrome may have been torn down while curl ran. The fetch result is handed back to the page through `Fetch.fulfillRequest`, so from Chrome's perspective the bytes came from cache, not the network.

## The policy table: who is even allowed to egress

Before any DNS or socket work, `DecideFetch` in `lib/net/fetch_policy.cc` runs a fixed sequence of gates and returns a `FetchAction`. The order matters and it is fail-closed at every step:

1. **Scheme and parseability.** `ParseHttpUrl` plus `IsAllowedScheme` — anything that is not a parseable `http`/`https` URL returns `kDenyBlockedByClient`.
2. **Images and media.** `kImage` and `kMedia` return `kDenyKeepElement`. The agent render extracts text, not pixels, so it never fetches image bytes; the element (its `src`, `alt`) is kept by the cleaner. This is a deny that preserves the DOM node rather than failing the request.
3. **Resource-class allowlist.** `IsFetchableClass` permits only `kDocument`, `kScript`, `kStylesheet`, `kFont`, `kXhr`, and `kFetch`. WebSocket, EventSource, Ping/beacon, CSP violation reports, and every unmapped or future CDP `resourceType` (which `ClassifyResource` maps to `kOther`) deny *before* the origin checks. A beacon aimed at your pinned upstream can never egress.
4. **The configured upstream (G1).** `IsPinnedUpstream` returns `kProxyUpstream` for the render's own origin.
5. **An operator-allowlisted third-party host (G2).** A host in `policy.allow_hosts` returns `kFetchAllowlisted`. URLs carrying userinfo (`user@host`) are never allowlisted here.
6. **Everything else denies.**

The two "allowed" verdicts are not equivalent, and the difference is the SSRF boundary. `kProxyUpstream` is the render's own backend — which may legitimately live on loopback or RFC1918, because that is where the customer's origin actually is. `kFetchAllowlisted` is a public third party, and it is held to a stricter standard described below. The G1 upstream is set in `browser_analysis_manager.cc`'s `RunAgentRender` from the queued item's own `scheme://hostname` — never from anything the page supplied. `allow_hosts` is operator configuration (`config_.agent_render_allow_hosts`), not page-derived.

## Re-adjudicate on every redirect hop, and pin to a resolved IP

The keystone is in `FetchSubresource`. It is a loop over redirect hops, and at the top of every iteration it calls `DecideFetch` again. A redirect target is a fresh request that must clear G1/G2 on its own merits. This is the bug class that defeats naive SSRF filters: you validate `https://allowed-cdn.example/`, it 302s to `http://169.254.169.254/`, and a filter that only checked the first URL happily follows. Here the second hop is re-classified from scratch.

Within a hop, after the policy verdict, the host is resolved (`deps.resolve`, real implementation `getaddrinfo`) unless it is already an IP literal, and the two verdicts diverge:

- **`kFetchAllowlisted` (G2)** calls `IsPublicUrl(url, addrs)`, which requires *every* resolved address to be public. This defeats DNS rebinding: a hostname that returns one public and one private address is rejected, because `ResolveSafe` blocks if *any* address is private. The pinned IP is then a validated public address.
- **`kProxyUpstream` (G1)** allows private backends, but still rejects if *any* resolved address is link-local. `IsLinkLocalIp` matches `169.254.0.0/16` (which includes the cloud metadata `.169.254`) and `fe80::/10`. The comment is explicit: a hostname for the render's own domain that mis-resolves to metadata space — split-horizon DNS, or an attacker running DNS for their own rendered domain — is never a legitimate backend and would exfiltrate the host's credential endpoint. It checks all resolved addresses, not just the one it pins, so DNS reordering cannot change the verdict.

The fetch then runs `curl` with `BuildPinnedGetArgv`, which carries the resolved IP as a `--resolve` pin and `--noproxy "*"`. The connection goes to the IP that was validated, not to whatever the host re-resolves to a moment later. The TOCTOU window between "we checked the address" and "the socket connects" is closed by pinning the checked address into the connection itself.

The SSRF range logic in `lib/net/ssrf_guard.cc` is worth reading for how literally it treats obfuscation. `IsPrivateV4` blocks `0.0.0.0/8`, `10/8`, `127/8`, `169.254/16`, `172.16/12`, `192.168/16`, `192.0.0.0/24`, CGNAT `100.64/10`, benchmark `198.18/15`, and multicast/reserved `>=224`. Crucially, `ParseHttpUrl` canonicalizes IPv4 literals through the WHATWG forms a browser URL parser accepts — hex `0x7f000001`, dword `2130706433`, octal-looking `0177.0.0.1` — so an encoded loopback address cannot slip past the literal branch into the caller-trusted resolved-address path. The metadata special hostnames `metadata` and `metadata.google.internal` are blocked by name in `IsBlockedHostLiteral`. The C++ is a deliberate port of the scanner's `ssrf.mjs`, byte-for-byte on the verdict, deviating only in the more-blocking direction.

## Defense in depth around the fetch itself

A correct policy is undermined if the subprocess can be steered by its environment, so `BuildScrubbedCurlEnv` is an allowlist, not a denylist. Only `PATH`, `SSL_CERT_FILE`, `SSL_CERT_DIR`, and `CURL_CA_BUNDLE` reach curl. `HOME`, `CURL_HOME`, `NETRC`, and every `http_proxy`/`https_proxy`/`all_proxy` variable are dropped, so the host environment can neither change curl's behavior nor tunnel the request past the IP pin. The same scrubbing runs identically on the POSIX `posix_spawn` path and the Windows `CreateProcess` path; the IP pin lives in the argv, so it is platform-independent.

Two more bounds protect against a hostile page that wants to abuse the renderer as an amplifier rather than as an SSRF gun. Concurrent out-of-process fetches per render are capped at `kMaxConcurrentAgentFetches = 32` in `page_analysis.cc`; requests over the cap fail closed via `FailRequest`. Because the manager serializes to one render at a time, that also caps global concurrency. And the response body is capped at `max_response_bytes`: the spawn reads in 8 KiB chunks, and on overflow it SIGKILLs (or `TerminateProcess`es) curl and leaves the exit code unset so the caller treats it as did-not-complete and denies. Response headers that describe a transform curl did not perform — `transfer-encoding`, `content-encoding`, `content-length`, `content-md5`, `content-range` — plus hop-by-hop and `Set-Cookie` headers are stripped before the bytes are handed back to Chrome, so the page cannot smuggle a compression bomb or a framing desync into the synthesized response.

One scope note: the markdown post that mentions SSRF for `/llms.txt` fetching describes one consumer; this egress model is the whole architecture underneath it. And it runs *only* on the `agent_optimize` path. The per-viewport perf render that measures LCP, CLS, and CSS coverage still uses the offline, cache-only sandbox with no egress at all. Turning `agent_optimize` on does not change the perf treatment served to ordinary browsers — the perf profile is deliberately collected from the offline render so it is byte-identical whether or not the agent path is enabled.

## Related

- [/blog/headless-lcp-cls-measurement/](/blog/headless-lcp-cls-measurement/) — the offline, cache-only perf render and the injected PerformanceObservers.
- [/blog/remove-unused-javascript-chrome-coverage/](/blog/remove-unused-javascript-chrome-coverage/) — JS coverage in the same block-everything sandbox.
- [/blog/serve-markdown-to-ai-crawlers-llms-txt/](/blog/serve-markdown-to-ai-crawlers-llms-txt/) — what the agent render produces and who consumes it.
- [/blog/agentic-web-at-the-origin/](/blog/agentic-web-at-the-origin/) — the origin-resident, off-by-default agentic model this render feeds.
- [/blog/verify-ai-crawlers-web-bot-auth/](/blog/verify-ai-crawlers-web-bot-auth/) — the sibling experimental feature that labels which bot actually sent a request.
- [/blog/visual-regression-gating-optimizations/](/blog/visual-regression-gating-optimizations/) — another off-request-path browser job.
- [/blog/why-i-rebuilt-mod-pagespeed/](/blog/why-i-rebuilt-mod-pagespeed/) — why 2.0 is an independent rebuild.

If you run a renderer over pages you do not control, the egress model is the part to read before you trust the output. The optimizer worker's `agent_optimize` path is off by default and documented at [/docs/agent-optimize/](/docs/agent-optimize/); the offline render that drives everyday optimization needs none of this and ships on by default. You can pull the worker image and read the policy code yourself from [/download/](/download/); it is licensed under Apache-2.0 and free to run, so you can verify the behavior described here on your own deployment.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
