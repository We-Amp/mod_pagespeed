---
title: 'Pay-per-crawl at the origin: an experimental RSL-CAP gate'
description: 'mod_pagespeed ships an experimental, off-by-default RSL-CAP gate for nginx: check License capability tokens and refuse unauthorized AI crawlers.'
date: 2026-06-22
author: 'Otto van der Schaaf'
tags: ['ai-agents', 'ai-crawlers', 'licensing', 'nginx']
draft: false
lastUpdated: 2026-10-22
tool: crawler-cost
---

> **Status: experimental preview, not GA.** RSL-CAP enforcement ships in the mod_pagespeed module, off by default and confined to test configurations. Nothing here is priced, sold, or available to buy. Treat this post as the design, not a quickstart.

An AI crawler hits your site. It reads a product page, follows a few links, and pulls a PDF. None of that is free for you. It costs origin CPU, bandwidth, and database queries. The crawler then trains on that content, or answers a user's question from it, and the value flows to someone else's product.

"Pay-per-crawl" is the industry's name for charging or gating that access. The interesting question is not whether to do it. It is where the decision gets made.

Most proposals put the decision somewhere upstream: a CDN edge, a marketplace, a robots-style declaration that bots are free to ignore. The module puts it at the origin you already control. The route that serves the bytes is the route that decides whether to serve them.

## robots.txt is a request, not a control

The standard tools for crawler control are advisory. `robots.txt` lists what you would prefer bots not fetch. RSL (Really Simple Licensing) and similar declarations state your terms. Both are signs on the lawn. A well-behaved crawler reads them. A crawler with an incentive to ignore them ignores them, and your origin serves the bytes anyway.

An advisory layer is still useful. It tells honest agents the rules. But it is not an access control, because nothing at the origin checks compliance before the response goes out. If you want a request refused unless it carries proof of permission, that check has to run in the request path.

## What the module ships: pay-per-crawl as an RSL-CAP gate {#what-115-ships-pay-per-crawl-as-an-rsl-cap-gate}

The module adds RSL-CAP capability-token enforcement for nginx. The mechanism is narrow on purpose.

A request arrives carrying an `Authorization: License <token>` header. The token is a capability: it encodes a license id and a set of scopes the holder is allowed to use. The interceptor validates the token against the issuer's public keys, then checks it against what the route requires.

If the route requires a license and scope that the token actually grants, the request passes through to your normal handling. When the token is missing, invalid, expired, from an untrusted issuer, or does not grant the required license and scope, mod_pagespeed refuses the request inline with a 401/402-class response (the smoke test pins the exact code per case), before it reaches your application code.

mod_pagespeed does not meter, bill, or settle money. A 402 says "payment is required for this," not "we just charged you." You bring the issuer that mints tokens and the billing relationship behind it. mod_pagespeed is the gate, not the payment processor.

This is not generally available. The enforcement primitive ships in the module, off by default, and today it runs only in experimental, test-vhost configurations. It is not priced, not sold, and not fully documented. If you want to run it in production, treat this post as the design, not the quickstart.

## The directives, and their defaults

Enforcement is off until you turn it on. Every directive below ships empty or disabled by default, so installing the module does not gate any route until you enable `RslCapEnforcement` and set a required license and scope.

- `RslCapEnforcement` — the master switch. Default **off**. Turn it on per location with `pagespeed RslCapEnforcement on;`.
- `RslCapKeyDirectoryFile` — path to a local JWKS file holding the issuer's public keys. Tokens are verified against these. Default empty.
- `RslCapRequestedLicense` — the license id this route requires a token to grant. Default empty.
- `RslCapRequestedScope` — the scope this route requires. Default empty.
- `RslCapIssuer` — an optional issuer pin. An otherwise-valid token whose issuer does not match this value is rejected. Default empty.

You decide, per location block, which license and scope a caller must hold. A documentation tree might require one scope, a high-cost search endpoint another, your marketing pages none at all. The gate is local config plus a key file. There is no call out to a licensing service in the hot path.

The optimizer worker ships the same experimental gate, configured through HTTPS JWKS key directories rather than a local file. Its verdict mapping and status codes match; see the [RSL-CAP configuration reference](/docs/rsl-cap/) for the worker's flags and environment variables.

## Why pay-per-crawl belongs at the origin

The case for enforcing here, rather than at a CDN or a third-party marketplace, is about who holds the keys and who sees the bytes.

The origin is the last hop before your content. Whatever reaches your application still has to pass it, so a check there has no upstream layer to route around. (Content already cached at an edge, or reachable from a mirror, is a separate problem you still have to plan for.)

The key file is yours. `RslCapKeyDirectoryFile` is a local file holding the issuer's public keys. The trust decision lives on your server, not in a vendor's account settings.

The policy is per route. Because the directives sit inside nginx location blocks, the same server can gate one path behind a paid scope and leave the rest open. You are not buying one global on/off switch.

## Verify it does what you think

A gate you have not watched refuse a request is a gate you are guessing about.

Do not over-index on exact status codes from this description. The behavior is "unauthorized access is refused with a 401/402-class response," and the precise verdict per case is what the test asserts, not what a blog paragraph should pin down. The repo ships an nginx smoke test that enumerates the verdict cases: valid token that grants the license and scope, missing token, expired token, wrong issuer, right token but wrong scope. Read that test to see exactly what each case returns.

Then exercise it on staging before it touches real traffic. Enable `RslCapEnforcement` on a single location with a real key file and a required license plus scope. Send a request carrying a valid `Authorization: License` token that grants them, and confirm it passes. Send one without, and confirm it is refused. Widen scope from there. Because every directive is default-off, the intended failure mode is to keep serving as before rather than lock out your own users. Verify that on staging rather than taking it on faith.

If you need to back out fast, the master switch is one line: set `RslCapEnforcement off;` on the affected location and the route serves the way it did before.

## Where this sits

Capability-token enforcement is the access-control layer of a larger story: deciding, at your own origin, which automated clients get in and on what terms. It is experimental and off by default; there is no product to buy here today. For the full picture of running the agentic web at the layer you control, read the pillar on [the agentic web at the origin](/blog/agentic-web-at-the-origin/).

The free half of that story is identity: telling a verified crawler apart from one wearing its user-agent string. That is a pre-release Web Bot Auth verifier that ships in 1.15, off by default, observe-only.

If you are not ready to gate anything yet, start with identity instead of enforcement: read [how to verify AI crawlers with web bot auth](/blog/verify-ai-crawlers-web-bot-auth/), stand up the verifier on staging, and watch a real crawler get classified before you decide what to charge for.
