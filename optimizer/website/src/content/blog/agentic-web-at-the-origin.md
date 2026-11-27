---
title: 'The agentic web hits your origin, not a proxy'
description: 'The agentic web creates four origin problems: identity, economics, what to serve, provenance. Where to solve them, and why on your own servers.'
date: 2026-06-22
author: 'Otto van der Schaaf'
tags: ['ai-agents', 'strategy', 'architecture', 'web-bot-auth']
draft: false
lastUpdated: 2026-10-22
tool: crawler-cost
---

## The agentic web changed your traffic

Look at your access logs for the last quarter. A growing slice of the requests are not browsers. They are AI crawlers building training corpora, retrieval bots fetching pages to answer a user's question right now, and autonomous agents acting on someone's behalf. This is the agentic web arriving at your origin. Multiple CDNs report that automated traffic is a large and rising share of what they front. Your own numbers will vary, but the direction is the same.

This is not the old SEO crawler problem. A search bot fetched your page, indexed it, and sent you a human later. The new clients often consume your content and never send anyone. They cost you bandwidth and compute, and you cannot tell which one you are talking to.

Four problems show up at the origin. Each has a partial answer somewhere in the stack today. The question is *where* you solve it, and the answer that runs through all four is the same: on your own servers, not on a third party's proxy.

## Four problems, one address

### 1. Identity: the User-Agent string is unverifiable

A request arrives with `User-Agent: GPTBot`. Is it GPTBot? The User-Agent string is a free-text field. Anyone can send any value. Reverse-DNS checks help for the major crawlers that publish IP ranges, but they only cover those crawlers and lag behind new ranges.

The emerging answer is cryptographic. Web Bot Auth, built on RFC 9421 HTTP Message Signatures, lets a bot sign its requests with an Ed25519 key whose public half is published. The origin verifies the signature and knows the request came from the holder of that key, not from someone copying a header. That is identity you can act on, not a string you have to trust.

Both parts — the module and the optimizer worker — include an experimental, observe-only Web Bot Auth verifier, off by default. When you enable it, it checks the signature and labels each request with the verified bot's identity, exposed where you can read it in your config and your logs. It does not block. It tells you which requests carry a valid signature and who signed them, so you can build policy on real data before you enforce anything.

The signature format, the key directory, and the verdict surface are covered in [Verify AI crawlers with Web Bot Auth and RFC 9421](/blog/verify-ai-crawlers-web-bot-auth/); the optimizer worker's configuration flags live in the [Web Bot Auth docs](/docs/web-bot-auth/).

### 2. Economics: gating crawl access at the origin

Once you can name a bot, the next question is whether it should be here for free. A bot that pulls ten thousand pages to train a model uses your infrastructure differently than a human reading one article. "Pay per crawl" has moved from a talking point to something operators want to enforce, and the origin is where you already meter the bytes.

Both parts ship RSL-CAP, a capability-token check that runs at the origin. It reads an `Authorization: License` token, validates the signature, and refuses access unless the token carries the right license and scope. It turns "I can see this bot" into "this bot needs the right credential to proceed."

Be clear-eyed about its status. RSL-CAP is experimental and off by default — an operator access-control layer for teams ready to run the token side, not a product you can buy today and not something that lights up on a stock install. It validates the token and returns a status code (allow, or 401/402); it never meters, charges, or settles money. The mechanism and the failure modes are in [Pay per crawl at the origin](/blog/pay-per-crawl-at-the-origin/), and the optimizer worker's configuration is in the [RSL-CAP docs](/docs/rsl-cap/). For the exact responses a request gets with and without a valid token, read the shipped smoke test rather than trusting a status code quoted from memory.

### 3. What to serve: agents waste tokens on your markup

An AI agent does not want your navigation or your nested `<div>` wrappers. It wants the content. Serve a retrieval bot a full HTML page and it spends tokens parsing markup it will throw away, and it parses HTML poorly enough that it sometimes throws away the content too. The free [AI readability check](/ai-readability/) shows how much more HTML than content one of your pages has.

The fix is to serve agents something they can read: clean markdown, plus an `/llms.txt` index that points them at it. The mod_pagespeed optimizer worker's `agent_optimize` does this through content negotiation. When a client signals it wants the agent-friendly variant, the origin serves markdown synthesized from your existing pages. There is no separate content pipeline to maintain.

This is experimental and off by default; the operator enables it with a worker flag, and it requires browser analysis. Turn it on when your content is the product and your agent traffic is real. The generation rules, the negotiation header, and the shape of the synthesized `/llms.txt` are in [Serve clean Markdown to AI agents](/blog/serve-markdown-to-ai-agents/); the configuration lives in the [agent-optimize docs](/docs/agent-optimize/).

### 4. Provenance: image optimization usually destroys it

C2PA Content Credentials attach signed provenance to an image: where it came from and what edited it, including whether a tool generated it with AI. Newsrooms, stock libraries, and camera makers are adopting them because the agentic web makes "is this image real?" a question buyers ask. Then the image hits your optimizer.

Most image pipelines strip everything that is not pixels. They re-encode to WebP or AVIF, drop the metadata, and hand back a smaller file with its provenance gone. You optimized the image and quietly broke the chain of custody you meant to keep.

By default, both parts carry C2PA / Content Credentials through image optimization rather than strip them: when re-encoding would drop the manifest, they serve the original bytes instead. An opt-in carry-through re-splices the credential onto a recompressed image where it can. This is the one feature in this set that is on by default, because the safe behavior, not silently destroying provenance, should not require a flag. Confirm it on your own assets. What is preserved, and the carry-through option, are in [Preserve Content Credentials and C2PA through image optimization](/blog/preserve-content-credentials-c2pa/).

## Why the origin, and why your servers

You could answer some of these at a third-party proxy. Several CDNs now offer bot verification and pay-per-crawl as managed products, and for some operators the managed path is the right call.

But notice what the proxy model asks of you. To verify a bot's signature, it terminates your TLS and reads your requests. To gate crawl access, it sits between you and your clients and decides who proceeds. To serve agent-friendly content, it rewrites your responses on its own boxes. Every one of these moves a decision about *your* content and *your* traffic onto infrastructure you do not control, governed by a contract you renegotiate.

mod_pagespeed runs these checks at your origin, on your own servers. The module verifies the signature and checks the crawl token before your origin spends a byte. Your worker generates the markdown from your pages. The same optimizer that was already touching the image carries the provenance through rather than stripping it. No request leaves your perimeter to ask a vendor whether it should be allowed. The policy lives where the content lives.

It also means you keep the escape hatch. Each feature is independently configurable with its own default, so you can enable one without the others and test on staging first. The spoke posts and docs carry the exact directive and how to disable it if it does the wrong thing on your inputs. Observe-mode verification and default-on provenance preservation are the low-risk entry points. The gating and the markdown layers are opt-in for a reason.

## What to do this quarter

Do not flip four switches at once. The honest sequence:

1. **Measure.** Turn on the Web Bot Auth verifier in observe mode. Read the classification variable in your logs for a few weeks. Now you know your real agent mix instead of guessing from User-Agent strings.
2. **Protect provenance.** If you serve images that carry Content Credentials, confirm the default-on C2PA preservation is doing what you expect on your assets.
3. **Decide on economics and content.** Pay-per-crawl gating and agent markdown are opt-in layers you turn on deliberately. Whether they are worth it depends on how much agent traffic you have and whether your content is the thing being consumed. The observe-mode data from step one makes that an informed decision rather than a hunch.

The agentic web is not a future you prepare for. It is in your access logs today. The choice is whether you meet it at your own origin, with levers you control, or hand the decision to someone else.

Start with the identity layer: the verifier is experimental, observe-only, and default-off, and the data it gives you informs everything else. See [how the Web Bot Auth verifier classifies each request](/blog/verify-ai-crawlers-web-bot-auth/).

---

> **Status:** the capabilities below sit at different stages. C2PA provenance preservation is available now and on by default in both parts, the module and the optimizer worker. Web Bot Auth verification and pay-per-crawl gating are experimental previews, observe-only or off by default. Agent markdown runs in the optimizer worker but is experimental and off by default. None of the identity, gating, or markdown layers is generally available. Maturity is noted per feature.
