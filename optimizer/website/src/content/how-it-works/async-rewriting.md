---
title: 'How a server optimizer avoids adding latency'
description: "How mod_pagespeed optimizes images, CSS, and JS without slowing the request: the second-request problem, and the optimizer worker that solves it."
order: 10
datePublished: 2026-06-13
lastUpdated: 2026-10-22
---
A server-side optimizer has one hard constraint that never goes away: it sits in the request path. Every page the user asks for passes through it on the way to the browser. So the moment the optimizer decides to compress an image or minify a stylesheet, the user is waiting for that work to finish. Do it inline, and you have traded a smaller page for a slower one.

Transcoding a single image can take tens to hundreds of milliseconds. A photo-heavy article references dozens of them. You cannot run that work in series while the user waits and call the result an optimization. This page explains how mod_pagespeed handles that constraint — first how the original mod_pagespeed solved it in 2011, then how [the optimizer worker](/features/) solves the same problem with a different mechanism. The problem is permanent. The mechanism evolved.

## The trap: the second request is the slow one

When mod_pagespeed first shipped in 2010, it optimized resources in the line of the HTML request. The first time anyone loaded a page, the original images had not been processed yet, so the page rendered quickly with unoptimized assets while the server fetched them in the background. That part was fine.

The second request was the problem. By then the original images were sitting in cache, so the optimizer recompressed and resized every one of them as the HTML streamed through. The last byte of HTML could not leave the server until the last image finished encoding. The second visitor to an image-rich page paid for everyone's optimization. The fast path and the slow path were swapped from what you would want.

This is the counterintuitive shape of the problem, and it is worth stating plainly: a naive server optimizer makes the *cached* case slow, because that is the case where it actually has the bytes to work on. The fix has to make sure that doing optimization work never blocks a response that could have been served immediately.

## The 2011 answer: cache the decision, split the threads

The Apache PageSpeed project worked through this in a 2011 design by Joshua Marantz. Two ideas from that work are the durable part, and both survive in the product today.

**Cache the decision, not just the bytes.** Optimizing a resource produces two different things worth remembering. One is the optimized bytes themselves — the smaller image, the minified script. The other is the *decision*: given this source URL, this requested width and height, and this browser's format support, which optimized variant should we serve, and when does the source expire so we know to recheck it. mod_pagespeed kept these in two logical caches. An HTTP cache held the output bytes. A separate metadata cache mapped the request context to the right output. On a warm cache the optimizer never re-runs the codec; it looks up the decision and serves the stored result. (For how the variant-selection cache works in 2.0, see [how the metadata cache picks a variant](/how-it-works/metadata-cache/).)

**Never let a cache hit queue behind an encode.** The original framework ran most of its bookkeeping — cache lookups, scheduling, fetch callbacks — on one thread, and ran the expensive part, the actual image encoding, on a separate low-priority thread. The point was isolation. A fast cache hit for one resource must not sit in line behind a 200-millisecond JPEG recompression for another. Cheap lookups stayed cheap because the heavy work could not block them.

There was a second pressure behind the design. The team was adapting mod_pagespeed to work in front of distributed caches, where a single lookup might take 20 milliseconds. A page with 200 resources cannot do 200 lookups in series — that is four seconds of pure cache latency before anything paints. So lookups had to batch and run in parallel rather than one at a time. The asynchronous framework existed to make all of that possible without buffering the whole page first.

That machinery — the in-process state machine, the slots and contexts that tracked each resource through the pipeline — was specific to that architecture. It is not how 2.0 works internally, and the old class names do not describe anything in the current product. What carried forward is the two principles above: cache the decision, and keep optimization off the path of anything that could be served now.

## How ModPageSpeed 2.0 solves the same problem

[2.0 is a ground-up rebuild](/blog/why-i-rebuilt-mod-pagespeed/). It keeps the proven low-level optimization libraries from the original project but replaces the orchestration entirely. The constraint is identical — do not make the user wait for encoding — and the answer is more direct than threads inside one process. The optimization work runs in a separate process.

Three pieces cooperate:

**The nginx interceptor.** A C++ nginx module classifies each incoming request into a compact capability mask: which image formats the browser accepts (WebP, AVIF, SVG), its viewport class, pixel density, and whether it sent `Save-Data`. The interceptor looks up the URL in the cache, picks the best-fit variant for that mask, and serves it directly from a memory-mapped cache file. No copying, no per-request allocation, no encoding. A cache hit is a lookup and a pointer into mapped memory.

**The Cyclone cache.** A variant-aware cache stores multiple variants of one resource under a single URL key, each tagged with the capability mask it was built for. Selection uses best-fit fallback: if there is no exact match for this client, it picks the closest available variant and degrades gracefully down to the original. The file is [memory-mapped and shared between nginx and the worker](/blog/memory-mapped-cache-zero-copy-serving/), which is how the interceptor serves bytes without going through the worker at all.

**The worker.** A separate C++ process does the encoding. When the interceptor finds no suitable variant, it serves the original immediately and sends a [fire-and-forget notification to the worker](/blog/fire-and-forget-worker-ipc/) over a Unix socket. It does not wait for a reply. The worker reads the source from the shared cache, runs the right optimization, and writes the optimized variant back at that capability mask. The next similar request gets a cache hit.

Walk the first request through it. A new browser asks for a page. The interceptor finds no AVIF variant for that client, so it serves the original image right away and drops a note to the worker. The user's request is already done; nothing blocked. Moments later the worker has written an AVIF variant sized for that viewport. The next visitor on a comparable device gets it from cache. The original "second request is slow" trap is gone because the second request never triggers encoding in the response path — encoding already happened, out of band, in another process.

This is the architecture behind [In-Place Resource Optimization](/blog/reduce-ttfb-server-layer-2026/): the optimizer keeps a resource's original URL while building optimized variants for it asynchronously, so the per-request cost on a hit is a cache lookup rather than a re-encode. It is also why 2.0 can produce [many image variants per source](/blog/viewport-aware-image-optimization/) — different formats, viewports, and densities — without any of that fan-out touching the latency a visitor sees. The variants accumulate in the cache as real traffic asks for them.

## What stayed the same, and what didn't

The continuity is at the level of principle. Both generations agree the request path is sacred, both agree the cached decision is the thing worth storing, and both keep heavy work away from anything that could be answered from cache. An optimizer that violates those rules slows down exactly the pages it was meant to speed up.

The mechanism is where they part. The original did it with cooperating threads inside the serving process and an in-process state machine that tracked each resource. 2.0 does it by moving optimization into a dedicated worker process, dispatching work by content type, and letting nginx serve finished variants straight from a shared memory-mapped cache. Same constraint, a cleaner separation.

A couple of details are worth keeping straight. There is no "fix Core Web Vitals" claim hiding in here — 2.0 reduces page weight and removes render-blocking work, which helps loading metrics, but it does not auto-correct interaction latency or every layout-shift cause. And the asynchronous model means optimized variants appear *after* the first miss, not during it. The first visitor to a brand-new URL gets the original. That is the point: they get it fast.

A practical consequence falls out of caching the decision. If the optimizer finds that no variant is meaningfully smaller than the source, it remembers that too, so it does not re-attempt a losing rewrite on every request. The negative result is a cached decision like any other.

---

The asynchronous-rewriting model originates with the Apache PageSpeed project, in design work by Joshua Marantz and contributors including Maksim Orlovich (2011). We-Amp B.V. was a committer and maintainer on that project, and maintains the open-source mod_pagespeed line today. The 2.0 re-architecture was an independent rebuild that kept the original optimization libraries and re-implemented the orchestration around them; that engine continues as the optimizer worker.

Want to see it run? [Install mod_pagespeed](/features/) and point it at your origin — it is licensed under Apache-2.0 and free to run, so you can watch variants populate the cache on your own traffic.
