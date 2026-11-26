---
title: 'PageSpeed for Envoy: what the proxy itself costs in CPU'
description: 'PageSpeed for Envoy runs inside Envoy, so every request also pays Envoy''s own CPU. Findings from our Envoy CPU series and how to reproduce them.'
date: 2026-11-26
author: 'Otto van der Schaaf'
tags: ['envoy', 'performance', 'build']
product: '1.1'
draft: false
---

[PageSpeed for Envoy](/docs/install-envoy/) is an experimental HTTP filter. No package is published for it: you build it from source, and it sits in the listener's `http_filters` chain, ahead of the router. Every request it optimizes also pays for the Envoy around it: connection handling, the codecs, routing, and every other filter and setting in your configuration.

So we measured that part, in a series on we-amp.com about where Envoy spends CPU on each request and how much of it can be cut. [The overview](https://we-amp.com/writing/envoy-cpu-performance/) indexes the posts, the method and the results we withdrew. Here are the findings that matter most if you run the filter, and how to check them yourself.

## What was measured

Envoy itself: none of the runs loaded the PageSpeed filter. Most figures come from the official Envoy v1.40.2 binary with 4 workers on one bare-metal server from 2016 (two Intel Xeon E5-2673 v4 sockets, clock fixed at 2.3 GHz), with synthetic plaintext HTTP/1.1 traffic over loopback unless a post says otherwise. The price is CPU cycles on the proxy process, user plus kernel, divided by the responses it served, from paired runs in a balanced order with a 95% interval. Read every figure as a price on that machine, not as a promise for yours.

## Three findings

**Settings can cost more than code saves.** In [the price list](https://we-amp.com/writing/envoy-price-list/), an external authorization check added about 55% CPU per request with an HTTP service and about 81% with gRPC, and 100% trace sampling added 34% (all three lower bounds: the proxy ran busier than the reference, and on that machine busier cores cost less per request). [Ten HTTP filters that took no action](https://we-amp.com/writing/envoy-price-list/#features-that-are-on-and-take-no-action) on a 4 KB proxied request added 9.5% (ten of our choice, about 5k cycles each as their mean). With `disabled: true` on each of those entries, we could not tell the chain from one without the filters. The PageSpeed filter is one entry in that same chain; we did not price it. The post's advice is to list the filters in your chain and, for each one that never acts on a listener's traffic, try `disabled: true` and compare CPU per request.

**A second proxy costs as much as the first.** If an edge Envoy sits in front of the one that runs the filter, each request passes two proxies. Two evenly loaded plain proxies cost +99.6% proxy CPU per request against one. [The second-hop post](https://we-amp.com/writing/envoy-second-hop-cpu-cost/) found that when the second proxy relays TCP (`tcp_proxy`) instead of acting as a minimal HTTP proxy, the pair costs 33% less, and that hop gives up per-request balancing, retries, HTTP stats and logs, and header policy. The Envoy that runs the filter has to proxy HTTP, so in this layout only the edge could relay, and a relaying first hop was not measured. If your edge only passes bytes through, the post shows how to price it as `tcp_proxy` on your own traffic before you size it.

**How the binary is built moves the most.** If you run the filter, you build Envoy yourself, so this lever is yours. A PGO and ThinLTO rebuild of unmodified Envoy used 20% to 40% less CPU per request than a standard optimized build of the same commit, on HTTP/1 and HTTP/2 benchmarks of a router-only proxy on long-lived connections (expect less where TLS or the kernel dominate); [our earlier post](/blog/envoy-filter-pgo-thinlto/) covers what that means for the filter. [The code-layout post](https://we-amp.com/writing/envoy-code-layout/) adds a cheaper step: on v1.40.2 an lld call-graph ordering file, built from a sampled branch profile of the running proxy and applied with a relink, with no instrumented build or training run, cut cycles per request by 7% to 10%. The profile came from the measured traffic, so expect less on yours. Neither was tried on the `envoy_pagespeed` build.

## How to reproduce it

The harness and raw data are public at [github.com/We-Amp/envoy-price-list](https://github.com/We-Amp/envoy-price-list) (code under Apache 2.0, data under CC BY 4.0). At tag `waves-2026-11`, three of its commands need no Envoy: a self-test, a simulated run that checks a configuration, and the report, which regenerates a published summary from our records:

```sh
python3 bin/epl selftest
python3 bin/epl run examples/settings-matrix/04-tracing.json --dry-run --out <dir> \
  --set client.binary=<your nighthawk_client> \
  --set floor_from=data/settings-matrix/aa/results.json
python3 bin/epl report data/settings-matrix/04-tracing/results.json \
  --floor data/settings-matrix/aa/results.json
```

The configurations carry our paths, CPU sets and rates, so adapt them to your machine before a real run. Each post names the harness tag, configurations and data archives behind its figures: the price list beside each section, the code-layout and second-hop posts under "How to reproduce it". The later sessions, including the ten filters and the second hop, are at tag `waves-2026-11d`, where `python3 tools/fetch-wave-data.py` fetches and verifies their archives.

Two rules from the method carry over to any Envoy benchmark. First, spread the load over every worker. With Nighthawk's default HTTP/1 connection reuse, 2 to 4 of Envoy's 4 workers carried the load, and cycles per request followed the busiest worker's share; `--experimental-h1-connection-reuse-strategy lru` spreads it. (The author of this post is an emeritus maintainer of [Nighthawk](https://github.com/envoyproxy/nighthawk), the load generator used here.) Second, run the same binary against itself in the same session, to learn the smallest difference your setup can resolve.

## Read the series

- [Envoy performance: what cut CPU per request, what held up](https://we-amp.com/writing/envoy-cpu-performance/)
- [Envoy with PGO and ThinLTO: 20% to 40% less CPU per request](https://we-amp.com/writing/envoy-pgo-thinlto/)
- [Envoy CPU cost of filters, settings and releases: a price list](https://we-amp.com/writing/envoy-price-list/)
- [What a 1,000-route table costs Envoy per request](https://we-amp.com/writing/envoy-large-route-tables/)
- [Envoy TLS handshakes and HTTP/3: what connections cost in CPU](https://we-amp.com/writing/envoy-connection-cpu-cost/)
- [Envoy code layout: PGO vs AutoFDO, BOLT and lld ordering](https://we-amp.com/writing/envoy-code-layout/)
- [Envoy tcp_proxy vs HTTP proxy: what a second hop costs in CPU](https://we-amp.com/writing/envoy-second-hop-cpu-cost/)

PageSpeed for Envoy stays experimental and is outside the scope of our [security policy](/security/), so [contact us](/contact/) before you run it anywhere that matters. To have the same method applied to your own Envoy, see [We-Amp's Envoy performance consulting](https://we-amp.com/consulting/#envoy).
