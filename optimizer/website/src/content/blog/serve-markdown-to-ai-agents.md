---
title: 'Serve markdown and llms.txt to AI agents from your origin'
description: 'The mod_pagespeed worker serves a markdown variant on Accept: text/markdown and builds /llms.txt from your sitemap. Experimental, off by default.'
date: 2026-06-22
author: 'Otto van der Schaaf'
tags: ['ai-agents', 'llms-txt', 'agent-optimize', 'content']
product: '2.0'
lastUpdated: 2026-10-22
draft: false
---

> **Status: experimental.** `agent_optimize` is an optimizer worker feature, off by default and enabled by an operator flag. Its flags and behavior may change in a future release.

An AI agent requests your page and wants the article. What it gets is your full HTML document: the nav, the cookie banner markup, the script tags, the footer, and the content buried in the middle. It parses all of it and burns tokens on all of it. The signal it wanted is maybe 10% of the bytes you sent.

That is a worse fit than it looks. HTML is a layout format. Agents and LLM crawlers read it to extract meaning, not to render it, so the markup that helps a browser paint a page is noise to a model reading prose. A clean markdown rendering of the main content is cheaper to ingest and easier to parse. The agent gets the heading structure, the lists, the links, and the text, without the chrome.

The usual answer is to maintain that markdown yourself. You stand up a headless CMS, or write a build step that mirrors every page into a markdown file and keeps the mirror in sync forever. That is a second content pipeline running next to your first one, and it drifts the moment someone edits a page and forgets the mirror.

The optimizer worker takes a different route. It generates the markdown variant at the origin, from the site you already have.

## What `agent_optimize` does

`agent_optimize` adds content negotiation for machine readers. When a request arrives with `Accept: text/markdown`, the worker serves a markdown rendering of that page's main content with `Content-Type: text/markdown`. The same URL still serves HTML to browsers. The agent asks for markdown and gets markdown. Nobody maintains a second copy.

The markdown comes from an extra headless render at the origin. The worker renders the page, extracts the main content and structure, and synthesizes markdown from it. Because it derives from the live page, it tracks the live page. Edit the article, and the next markdown request reflects the edit once the cache entry expires.

It can also build a site index. `--agent-optimize-llms-txt` synthesizes an `/llms.txt` from your sitemap plus per-page summaries, then caches and serves it. That gives an agent a single entry point describing what your site contains and where the readable content lives, instead of forcing it to crawl HTML to find out.

The generation runs at your origin. There is no external service rendering your pages, no markdown-as-a-service vendor, no copy of your content on someone else's infrastructure. The render runs on the same worker that already optimizes the page, and the markdown lives in your cache, on your servers. The sovereignty argument that applies to the rest of the pipeline applies here too.

## It ships off by default, as an experimental feature

`agent_optimize` runs in the optimizer worker; the module alone does not produce it. It ships **off by default** and is experimental, so evaluate it in staging before you enable it in production. Operator flags turn it on. On the module alone, with `AgentOptimize on`, the request is recognized and HTML responses carry `Vary: Accept`, but no markdown is ever rendered or served; the render depends on a headless browser only the optimizer worker runs. Turning it on takes two things:

1. **Enable browser analysis.** `agent_optimize` requires `--enable-browser-analysis`, because the markdown variant comes from a headless render.
2. **Set the flag.** `--agent-optimize` (or `PAGESPEED_AGENT_OPTIMIZE=true`). Default: `false`.

For the site index, `--agent-optimize-llms-txt` (env `PAGESPEED_AGENT_OPTIMIZE_LLMS_TXT=true`, default `false`) requires `--agent-optimize`, so enable both.

This is not a "just works out of the box" feature, and it would be dishonest to sell it as one. It is default-off and dependent on browser analysis being on.

### The cost you are signing up for

The markdown variant comes from an additional headless render, and that render is not free. It uses CPU and memory at your origin, on top of whatever the page already costs to optimize. The result is cached, so you pay the render once per cache entry rather than once per request, but the first request for an uncached path does real work.

The trade-off in plain terms: you spend origin compute to hand agents a cheaper, cleaner input. Whether that math works depends on how much agent traffic you get and how much you care about how those agents read you. Decide it with your own numbers, not ours.

## The control levers

A few of the knobs worth knowing. These are not all of them, but they shape behavior the most:

- `--agent-optimize-paths` — restrict markdown generation to specific path prefixes. Point it at your content paths so the worker is not rendering markdown for routes with no prose worth extracting.
- `--agent-optimize-sitemap-url` — the sitemap the `/llms.txt` index is built from. Default `/sitemap.xml`, read from your own origin.
- `--agent-optimize-cache-ttl` — how long a generated markdown variant stays cached. Default `86400` seconds (one day). Lower it if your content changes faster than that and you want the markdown to track more closely.
- `--agent-optimize-respect-ai-directives` — on by default. The worker honors AI-directed crawl signals when deciding what to expose.

Because everything is cached, the usual escape hatches apply. If a generated variant is wrong, PURGE the path and let the next request regenerate it. If you want the feature gone, the flag is one line to remove.

## How to verify it

Do not take the behavior on faith. Test it on staging.

1. Enable `--enable-browser-analysis` and `--agent-optimize`.
2. Request an HTML URL with the header `Accept: text/markdown`. Confirm the response comes back as `Content-Type: text/markdown` with the page's main content rendered as markdown.
3. Enable `--agent-optimize-llms-txt` and `GET /llms.txt`. You should see the synthesized index built from your sitemap.
4. Check the worker stats endpoint, which exposes build and serve counters for the feature, so you can confirm it is generating and serving rather than silently no-opping.

Exact per-case responses, such as what happens on paths with no extractable content or how edge cases negotiate, are best read from the shipped smoke test rather than inferred from this post. Run it against your own site and trust the verdict it gives over any general claim here.

## Where this fits

Serving markdown is one piece of treating AI agents as a first-class audience at the origin. The broader argument for why the agentic web is better served from your own infrastructure than bolted on as a separate stack is the [agentic web at the origin](/blog/agentic-web-at-the-origin/) pillar. For the implementation detail behind the render, the [deep dive on the markdown and llms.txt code path](/blog/serve-markdown-to-ai-crawlers-llms-txt/) walks the shipped filter. If you first want the evidence that AI crawlers miss JavaScript-rendered content, [can AI read your website?](/blog/can-ai-read-your-website/) has the data. To see how much of one of your own pages a crawler can read without JavaScript, run the free [AI readability check](/ai-readability/). If you also want to know which agents are reaching you and which are impersonating real crawlers, the companion piece on [verifying AI crawlers with Web Bot Auth](/blog/verify-ai-crawlers-web-bot-auth/) covers the identity side. If your stack is .NET rather than nginx, the same 2.0 pipeline runs as [ASP.NET Core middleware](/blog/aspnet-core-middleware/).

To turn it on, start with `--enable-browser-analysis` and `--agent-optimize`, then read the [agent_optimize configuration reference](/docs/agent-optimize/) for the full flag list and defaults.
