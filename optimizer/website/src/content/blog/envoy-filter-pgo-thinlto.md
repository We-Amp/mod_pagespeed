---
title: 'PageSpeed for Envoy: what a PGO and ThinLTO build means'
description: 'In our proxy benchmarks, PGO and ThinLTO cut stock Envoy''s CPU per request by 20% to 40%. What that means for the experimental PageSpeed filter.'
date: 2026-11-15
author: 'Otto van der Schaaf'
tags: ['envoy', 'performance', 'build']
product: '1.1'
draft: false
---

[PageSpeed for Envoy](/docs/install-envoy/) is an experimental HTTP filter. No package, container image or download is published for it. You build it from the [public source repository](https://github.com/We-Amp/mod_pagespeed). Its standalone target, `envoy_pagespeed`, links the filter into an Envoy binary. If you run that build, you already build Envoy yourself.

That makes a recent measurement relevant to you. We rebuilt unmodified Envoy with clang profile-guided optimization (PGO) and ThinLTO and compared it with a standard optimized build of the same commit. On HTTP/1 and HTTP/2 proxy benchmarks, CPU per request fell by 20% to 40%. The full write-up, with the method, the tables, the costs and the recipe, is on we-amp.com: [Building Envoy with PGO and ThinLTO](https://we-amp.com/writing/envoy-pgo-thinlto/). The method behind these measurements, and the earlier results we dropped along the way, is in [our account of the whole Envoy CPU effort](https://we-amp.com/writing/envoy-cpu-performance/).

This post covers what carries over to the PageSpeed filter, and what does not.

## What was measured

- **Stock Envoy as a plain proxy.** The benchmarks ran Envoy with the router and no other HTTP filter. The PageSpeed filter was not loaded.
- **27% to 40% less CPU per request** across 12 workload shapes at 2,000 requests per second, in 4 of 4 rounds each.
- **20% less near the test rig's limit**, at 16 workers and 32,000 requests per second. The saving was smaller there. How much of that drop comes from load is not resolved.
- **x86-64 Linux only**, with synthetic traffic over long-lived connections. No arm64, no gRPC.

## What was not measured

We have no number for the PageSpeed filter built this way, and this post does not claim one. Two things follow from how the build works:

- **Envoy's own work is the part the measurements cover.** Connection handling, the HTTP codecs and routing run on every request, with or without our filter.
- **The flags apply to the filter as well.** In the `envoy_pagespeed` build the filter is compiled into the same binary, so PGO and ThinLTO also change how the filter's own code is optimized. What they do to it depends on the training traffic.

## Untrained code can get slower

Code that the training run never executes has zero counts in the profile, and the compiler may treat it as cold: optimized for size and moved away from hot code. Such code can end up slower than in a standard build.

In the Envoy measurements, HTTP/3 was left out of the first training run. It still improved, by about a quarter to two thirds of the HTTP/1 gain. A profile that included HTTP/3 traffic brought it to an estimated 35% to 45% fewer CPU cycles per request than the standard build. That figure chains two comparisons and was not measured directly. That is one data point, and it went well. It is not a guarantee.

The filter's HTML rewriting and image optimization are CPU-heavy code, so take the risk seriously. If you try the recipe on a PageSpeed for Envoy build:

1. Train with the filter enabled, on pages like the ones you serve: HTML that gets rewritten, resources served from the cache, and images that have not been optimized yet.
2. Compare the result against your current build, under the same traffic, before you rely on it.

## What carries over to the build

The recipe in the main post is written for stock Envoy's `//source/exe:envoy-static` target. We have not tried it on `envoy_pagespeed`, which builds against Envoy v1.40.1 with its own Bazel configuration, so expect to adapt the flags. A few findings from the Envoy runs apply to any build:

- **Use Bazel's `thin_lto` feature for the ThinLTO step.** With raw `-flto=thin` flags, ThinLTO runs inside one linker process, which peaked at 33 GiB of memory. With the feature, the ThinLTO link stage peaked at about 9 GiB.
- **The instrumented binary is slow.** It is for training, not for serving traffic.
- **Train on the tree you build.** A profile applied to Envoy code 11 days (219 commits) newer than the profile, or 4 weeks older, kept 78% to 92% of the gain. The filter builds against an Envoy release rather than Envoy's main branch, and no gap as large as a minor release was tested. Make your own profile from your own build.

## Energy and cores

Less CPU does not mean proportionally less power. On a dual-socket Intel Xeon E5-2673 v4 at a fixed 2.3 GHz, Envoy's energy per request above idle fell 3.9% to 12.9%, with the larger figures at higher load, while CPU time per request fell 30% to 39%. If CPU is what you provision for, the same traffic fits on fewer cores.

## What Envoy settings cost

A companion post, [What Envoy settings and releases cost in CPU](https://we-amp.com/writing/envoy-price-list/), prices common Envoy settings, a second proxy hop and three releases in CPU per request, with the conditions next to every number. One row is relevant if you are deciding where an optimizing proxy goes: on that rig, a second plain Envoy hop roughly doubled the proxy CPU per request for the pair when both proxies were evenly loaded.

## Tell us what you measure

PageSpeed for Envoy stays experimental and is outside the scope of our [security policy](/security/), so [contact us](/contact/) before you run it anywhere that matters. If you build it with PGO and ThinLTO, tell us what you measure, including a result that comes out worse.
