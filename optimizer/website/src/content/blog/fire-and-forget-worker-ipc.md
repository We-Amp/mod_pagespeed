---
title: 'Fire-and-forget IPC: keep optimization off the request path'
description: 'mod_pagespeed keeps optimization off the nginx request path: a small notification to the worker, no reply, so a cache miss stays cheap and none waits.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['architecture', 'performance', 'deep-dive', 'nginx', 'caching']
draft: false
product: '2.0'
---

The fastest way to ruin a page-optimizer's reputation is to make the user request wait on the optimization. Re-encoding a JPEG to WebP takes tens of milliseconds. Extracting critical CSS means scanning the HTML and walking stylesheets. If any of that work sits on the request path, you have traded the latency you were trying to win back for latency you created yourself.

mod_pagespeed uses fire-and-forget IPC to keep that work off the request path entirely. The nginx module and the optimization worker talk to each other, but the message between them carries no content and expects no reply. The module sends a small notification — the URL, its content type, and a capability mask — then gets on with serving the response. The worker does the optimization later, in the background. That is what keeps a cache miss in 2.0 cheap: a pass-through plus a tiny socket write, not a synchronous encode.

## The model we walked away from

The earlier design did what most optimizers do: it shipped content over the wire and waited for an answer. The IPC layer carried an `OptimizationRequest` containing the `origin_content`, the module blocked until an `OptimizationResponse` came back with the `optimized_content`, and only then served the result. That is a request-response RPC, and it has two structural problems.

First, the request path is now coupled to optimization time. However fast the worker is, the client is waiting for it. Second, you are copying the full response body into the IPC channel and copying the optimized body back out — content rides the wire twice for work that produces a cacheable artifact you reuse on every subsequent request.

2.0 removes both. The IPC protocol was refactored to notification-only. The `OptimizationRequest`/`OptimizationResponse` pair — the structs that carried `origin_content` and `optimized_content` — were deleted outright. What replaced them is deliberately small.

## What actually crosses the socket

The shared medium between nginx and the worker is the cache, not the socket. The [memory-mapped cache write-up](/blog/memory-mapped-cache-zero-copy-serving/) covers why content lives in Cyclone and gets read in place by whichever process needs it. This post is about the message that points at that content: the protocol the RPC removal left behind.

The message is a `CacheNotification`. The fields that drive optimization are the URL, the content type, and a 32-bit capability mask; alongside those the struct carries the routing and protocol fields the worker needs to act on the right cache entry:

```cpp
struct CacheNotification {
  std::string url;
  std::string hostname;
  std::string scheme;          // "http" or "https"
  ContentType content_type;
  uint32_t capability_mask;
  bool agent_request = false;  // set when the request asked for the agent representation
};
```

It serializes to a flat binary frame:

```
[4 bytes: total_length]
[1 byte:  version]
[4 bytes: url_length][url bytes]
[4 bytes: hostname_length][hostname bytes]
[1 byte:  content_type]
[4 bytes: capability_mask]
[1 byte:  scheme]
[1 byte:  agent_request]
```

No response body, no headers, no image data. The largest field is the URL itself; everything else is a handful of fixed-width bytes. The module writes this frame, does not wait, and moves on.

On a cache miss, the nginx module passes the request to origin, and as the origin response streams back through the proxy it gets recorded into the Cyclone cache as the original (identity) variant. Then the module sends the `CacheNotification` and serves the origin response to the client unchanged. The worker receives the notification, reads the original content back out of the cache by URL, does its work, and writes the optimized variant back into the cache. The socket only ever carries a pointer to the work, never the work itself.

That split is what makes fire-and-forget safe rather than reckless. The worker is not being handed something it might drop; the content is already durably in the cache before the notification is sent. If the worker is busy, slow, or restarting, the request was already served. The optimization is owed to the *next* request for that URL, not this one.

## Why fire-and-forget IPC keeps latency off the request path

Trace a cache miss through the module:

1. Request comes in, gets classified into a `CapabilityMask` (which client capabilities are in play — WebP support, Save-Data, and so on).
2. Cache lookup misses, so the request goes to origin.
3. The origin response streams back through a body filter, which buffers it, writes it to the cache as the original variant, and passes it through to the client unchanged.
4. The module sends one `CacheNotification` to the worker over a Unix socket and does not wait for a reply.
5. The client has its response.

The only optimization-adjacent cost the client paid was the cache write of the original and a single non-blocking socket write of a few dozen bytes. Critical-CSS extraction and image transcoding happen entirely in the worker, after the response is gone. The user request is never blocked on an encode, a stylesheet scan, or a critical-CSS pass.

On the next request for that URL, the optimized variant is in the cache, and the module serves it directly from the memory-mapped cache with a zero-copy buffer pointing at the mmap'd bytes. That is where the win shows up: the first visitor pays for a pass-through and primes the cache; everyone after them gets the optimized variant at cache-hit latency.

The caveat is the same one any asynchronous design carries: there is a window. When the worker processes an HTML page it tries to read the page's stylesheets from the cache, and a stylesheet may not have been recorded yet. In 2.0 the worker skips what it cannot find and the variant is regenerated on a later pass rather than blocking. The trade is intentional: a missed optimization on one early request is cheap; a stalled user request is not. If you want the mechanics of how the worker does that scan-and-extract step once it has the content, the [async rewriting](/how-it-works/async-rewriting/) and [critical-CSS](/how-it-works/css-parsing/) write-ups cover it.

## Related

- [Memory-Mapped Cache: Zero-Copy Serving](/blog/memory-mapped-cache-zero-copy-serving/)
- [Reducing TTFB at the Server Layer](/blog/reduce-ttfb-server-layer-2026/)
- [Running mod_pagespeed with Docker Compose](/blog/run-with-docker-compose/)
- [Migrating from mod_pagespeed 1.x](/blog/migrating-from-1x/)
- [Why I Rebuilt mod_pagespeed](/blog/why-i-rebuilt-mod-pagespeed/)
- [How async rewriting works](/how-it-works/async-rewriting/)
- [The metadata cache](/how-it-works/metadata-cache/)

If you want to see the asynchronous path in action — first request primes the cache, second request serves the optimized variant at cache-hit speed — the quickest way is to [download](/download/) mod_pagespeed and watch a URL go from original to variant across two requests. The [cache modes documentation](/docs/cache-modes/) explains how the worker's write-back behaves under each mode. It is licensed under Apache-2.0 and free to run in development and in production, so you can stand the architecture up and verify the latency behavior on your own traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
