---
title: 'Cache mode safety: must-revalidate vs aggressive TTLs'
description: 'Why must-revalidate, not max-age, is the real safety net in mod_pagespeed, and when aggressive TTLs with stale-if-error are the right call.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['caching', 'operations', 'performance', 'nginx', 'architecture']
draft: false
product: '2.0'
---

Picture the failure you are actually trying to survive. You roll out a filter
config, and a rewrite produces broken CSS — a stylesheet that parses on your
machine but drops a rule on production. mod_pagespeed serves it, a CDN edge
caches it with `max-age=86400`, and now every visitor for the next 24 hours
gets the broken page until you find the purge button. The TTL is not protecting
you here. It is what holds the bug in place.

Cache mode safety in mod_pagespeed exists to bound exactly that scenario. The
[cache modes doc](/docs/cache-modes/) lays out the two settings, `safe` (the
default) and `aggressive`, and the exact headers each emits. What the reference
doc cannot do, and what this post is for, is editorialize: the recovery time is
the only number that matters, the safety mechanism is `must-revalidate` rather
than the TTL, and a 504 on origin failure is the correct outcome, not a
regression.

## Cache mode safety: the mechanism is must-revalidate, not the TTL

The instinct is to treat a short `max-age` as the safety control — shorter TTL,
faster recovery, safer. That is half right and it misleads people into the wrong
fix.

Safe mode emits `max-age=300, must-revalidate` on CSS and JS, and
`max-age=1800, must-revalidate` on images. The doc is blunt about which half of
that does the work: *"In safe mode, the response includes `must-revalidate`
regardless of your max-age value. The safety mechanism is `must-revalidate`, not
the TTL."* You can bump `pagespeed_css_max_age` to 600 or `pagespeed_image_max_age`
to 3600 and you are still safe, because `must-revalidate` is still there.

The distinction matters because of what each directive promises a downstream
cache. `max-age` says how long the content is fresh — and if you have ever
wondered where a lifetime comes from when the origin sends none, the
[default cache TTL heuristics](/blog/default-cache-ttl-heuristic-freshness/)
post covers how that number gets derived. `must-revalidate` says what
happens *after* that: the cache **must** check with the origin before serving
again — it is forbidden from serving stale. Drop `must-revalidate` and the same
short `max-age` gives a downstream cache permission to keep serving the expired,
possibly-broken response under the staleness rules in RFC 9111. The TTL becomes a
suggestion. With `must-revalidate`, expiry is a hard wall: past it, the edge
either revalidates or it serves nothing.

So the recovery math is not "TTL = time-to-fix." It is "TTL = the maximum window
during which a broken response can still be served without the edge phoning
home." Once that window closes, `must-revalidate` forces the conversation that
lets your corrected config take effect. Safe mode bounds that window to 5 minutes
for CSS/JS and 30 for images. Aggressive mode opens it to 24 hours.

People worry that forcing revalidation every few minutes is expensive. It is
not, and the reason is conditional requests. mod_pagespeed generates ETags on all
cache hits, so a revalidation that finds nothing changed returns 304 Not Modified
with no body — a header exchange, not a re-transfer. The expensive part of a
cache miss is shipping the bytes again, and `must-revalidate` plus ETags skips
exactly that. If you want the mechanics of how 304 revalidation interacts with
the [metadata cache](/how-it-works/metadata-cache/), that is a related rabbit
hole worth following.

## Why a 504 beats serving cached-broken output

Here is the part that surprises operators, and the part a reference table states
without arguing for: in safe mode, *"if the origin is unreachable after TTL
expiry, caches return 504 rather than serving stale content. This is intentional
— a visible error is better than silently serving corrupted content."*

A 504 feels like a downgrade. The origin is down, you had a cached copy, and the
edge chose to fail instead of serving what it had. Why?

Because the cached copy is, by construction, transformed output whose
correctness depends on state the cache cannot see. mod_pagespeed rewrites your
content against a specific filter configuration and a specific software version.
The origin's own cache guarantees do not transfer verbatim to the rewritten
bytes — a config change or a software bug can make yesterday's cached response
wrong today. When you cannot reach the origin to revalidate, you have lost the
one channel that could tell you the cached transform is still valid. Serving it
anyway is a bet that nothing changed, placed precisely when you have no way to
check.

A 504 is loud. It pages someone, it shows up in your monitoring, it gets fixed.
Silently serving a stale, possibly-broken optimized response is the failure mode
that does not page anyone — visitors see a subtly broken layout, bounce, and you
find out from a support ticket three days later. Safe mode picks the loud failure
on purpose. It is the same philosophy behind preferring a hard cache invalidation
over hoping content ages out, which we have written about in the context of
[single-URL purge on an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/).

This is a genuine trade-off, not a free lunch. A 504 during a transient origin
blip is a worse visitor experience than a slightly-stale-but-fine page would have
been. Safe mode is calibrated for the case where you do not yet trust the
correctness of the cached transform more than you trust your origin's
availability. That assumption is exactly what aggressive mode lets you reverse.

## When aggressive mode plus stale-if-error is the correct call

Aggressive mode is not "safe mode for people who do not care." It is the right
default once a specific set of conditions hold, and the headers reflect a
different bet: `public, max-age=86400, stale-if-error=86400`, with
`stale-while-revalidate` synthesis enabled.

`stale-if-error=86400` is the inverse of the safe-mode 504. During an origin
outage, the edge serves the cached copy for up to 24 hours instead of failing.
`stale-while-revalidate` lets the browser serve a stale copy immediately while
revalidating in the background, so the visitor never waits on the round-trip.
Both directives trade recovery speed for availability and latency. You are now
betting that your cached transforms are correct, because if one is broken, it
stays broken — and served — for up to a day, or until you purge it at the CDN.

The doc names the conditions under which that bet is sound, and they are worth
treating as a checklist rather than a vibe:

- **You have run in safe mode for at least a week without issues.** Safe mode's
  short, must-revalidate TTLs are how you earn confidence that your filter config
  does not produce broken output under real traffic. Skipping this step is how
  you cache a bug for 24 hours on day one.
- **Your filter configuration is stable and tested.** Aggressive mode amplifies
  the cost of a bad config change. Stable config is the precondition that makes
  the amplification acceptable.
- **You have CDN purge capability for emergency corrections.** With a 24-hour
  TTL, purge *is* your recovery mechanism. Without it, your recovery time is
  measured in hours.
- **Cache efficiency matters more than instant recovery.** This is the actual
  decision. CDN-backed production deployments live and die on edge hit ratio;
  forcing revalidation every 5 minutes throws that away. If you are serving real
  traffic through a CDN you have configured deliberately, aggressive mode matches
  the deployment.

That last point is why aggressive mode exists at all. Safe mode's whole design —
short TTLs, `must-revalidate`, 504-on-failure — assumes you would rather recover
fast than cache long. A mature CDN deployment usually wants the opposite, and
forcing it into safe-mode revalidation cadence is leaving performance on the
floor for a safety margin you no longer need. The migration path is the point:
start safe, prove correctness, then opt in.

One detail that survives both modes: mod_pagespeed strips `immutable` from all
transformed content, in safe *and* aggressive. Your origin may mark fingerprinted
assets `immutable`, but the optimized output depends on mutable state — config,
software version, capability detection — so an immutability claim on it would be
a lie that no purge can undo (an `immutable` asset is one the browser will not
revalidate even on reload). Aggressive mode keeps the long lifetime from
`pagespeed_immutable_max_age` without making that false promise. You get the
cache performance; you keep the escape hatch.

## Related

- [Why optimized URLs carry a content hash](/blog/content-hash-urls/)
- [How default cache TTL heuristics decide freshness](/blog/default-cache-ttl-heuristic-freshness/)
- [Single-URL cache purge on an optimizing proxy](/blog/single-url-cache-purge-optimizing-proxy/)
- [Reduce TTFB at the server layer](/blog/reduce-ttfb-server-layer-2026/)
- [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/)
- [How the metadata cache works](/how-it-works/metadata-cache/)
- [Choose a cache mode (reference)](/docs/cache-modes/)

The mode you want depends on how much you trust your cached transforms relative
to your origin's uptime, and the answer on a fresh deployment is "not yet."
Start in safe mode, let the short must-revalidate TTLs surface any broken output
within minutes, and switch to aggressive once a week of clean traffic and a
working CDN purge have earned it. mod_pagespeed ships with safe as the default
for exactly that reason. If you want to try it on your own stack, the
[download](/download/) is free to run — the line is licensed under
Apache-2.0 — and the
[cache-control reference](/docs/cache-control/) covers the header behavior in
full, so you can prove the cache-mode trade-off on your real traffic.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
