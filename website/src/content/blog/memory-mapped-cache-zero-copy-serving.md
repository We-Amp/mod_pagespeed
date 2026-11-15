---
title: 'The memory-mapped cache: zero-copy serving from nginx'
description: 'How mod_pagespeed shares one memory-mapped cache between nginx and the worker: large hits zero-copy via sendfile, small ones with a single copy.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['architecture', 'caching', 'performance', 'deep-dive', 'nginx']
draft: false
product: '2.0'
faq:
  - q: 'How does mod_pagespeed serve a cache hit without copying the data?'
    a: 'nginx and the optimizing worker share one memory-mapped cache file. For a large hit, nginx streams the bytes straight from that file to the socket with sendfile, so the payload never enters the serving process. For a small hit, it copies the bytes once from the mapped region, because the sendfile syscall would cost more than the copy.'
  - q: 'Why does 2.0 use sendfile for large cache entries but a copy for small ones?'
    a: 'The sendfile syscall has fixed setup overhead. Above about 32 KB, avoiding the payload copy is worth that overhead; below it, copying the few kilobytes out of mapped memory is cheaper than setting up the kernel transfer. The crossover is empirically tuned.'
---

In mod_pagespeed 1.x, a request for `/styles/site.css` or `/photo.jpg` paid for its own optimization. The web server ran the rewrite in-flight, on the request thread, and only then sent bytes to the client. The work was cached afterwards, but the architecture meant transformation latency sat on the critical path of real requests, in every web server process, every time the cache was cold.

The mod_pagespeed optimizer worker takes that work off the request path entirely. The optimizing process and the serving process are separate, and they meet at a single memory-mapped cache: one disk file that both nginx and the worker `mmap` into their own address space. When a request hits a cached variant, nginx hands the client a buffer that points straight at the mapped pages. No copy, no re-parse. The bytes the worker wrote are the bytes nginx serves.

## Two processes, one memory-mapped cache

The 2.0 runtime is three cooperating pieces, and the cache is the one they all share:

1. A thin C++ nginx interceptor that classifies requests, serves cached variants, and proxies misses to the origin.
2. A standalone optimizer worker process running a libuv event loop. It receives notifications from nginx, reads original content from cache, optimizes it, and writes variant alternates back.
3. Cyclone, a memory-mapped disk cache shared between the two.

Cyclone stores everything in one memory-mapped volume file rather than a directory tree. Both nginx and the worker map that same file. The consequence is the part that matters for serving: writes from either process are immediately visible to the other, because both open the cache with multi-process sharing enabled. The worker finishes encoding a WebP variant, writes it into the mapped region, and the next nginx request can read it without any handoff, flush, or re-open. There is no message that ships content between the two processes. The IPC notification carries identifying metadata — the URL, hostname and scheme, a content type, and the 32-bit capability mask — not the bytes. The content lives in the cache; the socket only points at it.

This is a deliberate inversion of the 1.x data flow. In 1.x, the rewritten bytes existed inside the web server's request context and had to be threaded back into the response. In 2.0, the bytes are written once to a shared region and read in place by whoever needs them.

## What "zero-copy" actually means on a cache hit

The interceptor's serving path is short. On a request, it classifies the client into a `CapabilityMask` from the request headers, composes a `CacheKey` from the URL, hostname and scheme, and asks the cache for the best alternate for that mask. The 32-bit mask encodes the dimensions a variant can differ on: image format, viewport class, pixel density, Save-Data, and transfer encoding. Because a single URL can carry many alternates, the selector scores every stored alternate against the mask, and falls back to the original if no optimized variant exists yet.

When a lookup succeeds, the interceptor does not allocate a response buffer and `memcpy` the cached entry into it. The optimization already happened, earlier, in the worker and off this request, so there is no transformation left to do on the wire. What remains is to describe the cached bytes to nginx and let the kernel move them. How that description is built depends on how large the entry is.

### Two ways to move the bytes, chosen by size

For a large entry, the interceptor hands nginx a file buffer: an `ngx_buf_t` with `in_file` set, pointing at the shared cache file and the offset where the variant's bytes live. nginx streams it with `sendfile`, so the payload goes from the kernel's page cache straight to the socket and never enters the serving process at all. This is zero-copy in the strongest sense the operating system offers.

For a small entry, `sendfile` is the wrong tool. The syscall carries fixed setup overhead, and arranging a kernel file transfer to move a few kilobytes costs more than simply copying those kilobytes out. So below a threshold the interceptor serves the entry as a memory buffer that points straight into the mapped file. The bytes are copied once, from mapped memory to the socket, and that single copy is cheaper than the syscall would have been. Either way nginx sets `Content-Type` from the variant's stored metadata and `Content-Length` from the mapped content size, and registers a cleanup handler to release the read handle when the request completes.

The crossover is empirically tuned and sits around 32 KB. The logic is a plain latency comparison: copying 32 KB out of mapped memory costs on the order of a couple of microseconds, small enough to vanish, while copying a megabyte costs closer to a hundred microseconds, large enough to dominate the response. Above the line, skip the copy and pay the syscall; below it, skip the syscall and pay the copy. This is the "except where it doesn't help" part of end-to-end zero-copy: the cache holds one design, and the serving path picks the cheaper mechanism for each object size.

Either path removes the cost 1.x could not avoid. The rewritten bytes no longer live inside the web server's request context, waiting to be threaded back into a response and copied on the way out. The response buffer _is_ the cache.

The first request for a cold URL still gets the original response, marked `X-PageSpeed: MISS`. The worker optimizes asynchronously, and subsequent requests get `X-PageSpeed: HIT` with no processing overhead on the wire. The latency that 1.x charged every request, 2.0 charges once, to a background process, and never to the client.

## Serving safely while the cache changes underneath you

Two processes share one mapped file, and the worker keeps writing to it while nginx serves from it. Two failure modes follow: nginx serving a half-written entry, or reading from a region that gets reclaimed while a slow client is still draining the response. Two mechanisms rule both out.

The first is integrity. Every entry Cyclone stores carries a CRC32 checksum, and the write path publishes an entry's directory pointer only after the entry's bytes and checksum are fully written. A reader either sees the old pointer or the new one, never a torn mixture, and if it does catch a partially visible entry on a shared page cache, the checksum fails and the read retries. nginx never serves bytes that fail their checksum. A crash between writing an entry and publishing its pointer leaves an unreferenced entry, not a dangling one.

The second is lifetime. A `sendfile` response can outlive the handler that started it: a slow client keeps draining the socket long after the interceptor has moved on. If the cache were purged, or the volume reopened, in that window, the descriptor `sendfile` reads from could be closed underneath it. So each in-flight response takes its own duplicated descriptor for the cache file and holds a reference that pins the cache generation for the life of the request. A purge can close the shared descriptor and rotate the generation while the response keeps streaming from its private handle to the same bytes. Cleanup releases the descriptor and the reference in a fixed order, once the response is fully sent. Tests lock this down by purging the cache twice under an active request and asserting the response still reads the right bytes.

A note on where that metadata lives, because it differs between the two parts. In the optimizer worker, each variant's metadata — the capability mask, content type, and origin cache-control fields — sits inside the Cyclone volume next to the alternate's bytes, so it inherits the same checksums and write-then-publish ordering and is as durable as the content it describes. The module is arranged differently. It runs in-process, and Cyclone sits in two places there: as the on-disk cache for fetched originals and optimized output, and behind a shared-memory metadata cache shared across server processes. That shared-memory tier is a fast front for reads, but it writes through to Cyclone: every metadata entry, the small hot lookups included, is also recorded in the mapped volume on disk. The shared memory serves the hot path; Cyclone is the source of truth. Page properties — the critical-image and selector data the beacon learns from real traffic — get the same write-through treatment in v1.15.0+r17 and later. The practical consequence is that a restart doesn't throw the metadata away — the process comes back up, reads persist from the on-disk volume, and requests that were already resolved don't have to re-derive which variant to serve. In both parts, the metadata and the optimized bytes it describes are backed by the same checksum and write-then-publish guarantees, and survive a restart together.

## Variants, metadata, and why a directory cache could not do this

The 1.x cache stored each rewritten resource as a separate entry in a directory hierarchy. That model is fine for "one input, one output," but it does not express "one URL, several alternates chosen per request." 2.0 needs the latter: the same `/photo.jpg` may resolve to a WebP variant for a browser that advertises it, or an optimized JPEG as the fallback, each potentially at different viewport sizes and pixel densities. Where a browser advertises a newer format the cache holds a matching alternate for, the negotiation picks it.

Cyclone keeps all of those as alternates of one URL, and each alternate carries its own metadata inside the cache: the capability mask, content type, the origin's cache-control fields, the SSIMULACRA2 perceptual score, content class, ETag, and Last-Modified. That metadata is what lets nginx serve a variant correctly without re-deriving anything — it reads the stored `Content-Type` rather than sniffing, and it stores the origin's cache-control fields alongside the bytes. The image format negotiation that 2.0 does at request time (reading `Accept`, serving the best available bytes from the original URL, no `.webp` extensions or JavaScript detection) only works because the alternates and their metadata sit side by side in the mapped file, addressable by key.

The Cyclone format is not compatible with the 1.x file cache, and there is no migration path. The cache starts cold and warms as traffic flows through it — which is consistent with the rest of the model, since the first request through any URL is the one that records the original and triggers the worker.

## Frequently asked questions

**How does mod_pagespeed serve a cache hit without copying the data?** nginx and the optimizing worker share one memory-mapped cache file. For a large hit, nginx streams the bytes straight from that file to the socket with sendfile, so the payload never enters the serving process. For a small hit, it copies the bytes once from the mapped region, because the sendfile syscall would cost more than the copy.

**Why does 2.0 use sendfile for large cache entries but a copy for small ones?** The sendfile syscall has fixed setup overhead. Above about 32 KB, avoiding the payload copy is worth that overhead; below it, copying the few kilobytes out of mapped memory is cheaper than setting up the kernel transfer. The crossover is empirically tuned.

## Related

- [Cyclone vs. the file cache benchmark](/blog/cyclone-cache-vs-file-cache-benchmark/)
- [How the metadata cache avoids re-optimizing](/how-it-works/metadata-cache/)
- [Migrating from mod_pagespeed 1.x to 2.0](/blog/migrating-from-1x/)
- [Fire-and-forget worker IPC](/blog/fire-and-forget-worker-ipc/)
- [Why I rebuilt mod_pagespeed](/blog/why-i-rebuilt-mod-pagespeed/)
- [Reduce TTFB at the server layer](/blog/reduce-ttfb-server-layer-2026/)
- [Run mod_pagespeed with Docker Compose](/blog/run-with-docker-compose/)
- [How async rewriting works](/how-it-works/async-rewriting/)
- [Cache modes](/docs/cache-modes/)

The two-container Docker Compose stack (an nginx interceptor and an optimizer worker sharing one cache volume) is the smallest setup that exercises this path; the [cache modes documentation](/docs/cache-modes/) covers how they coordinate around the mapped file.

---

_mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently._
