---
title: 'Serving markdown to AI crawlers, and synthesizing /llms.txt'
description: 'How to serve markdown to AI crawlers on the same URL via Accept: text/markdown, synthesize /llms.txt from the sitemap, and honor AI opt-out directives.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['architecture', 'deep-dive', 'operations']
draft: false
product: '2.0'
---

A crawler sends `GET /docs/configuration/` with `Accept: text/markdown`. Instead of a few hundred kilobytes of HTML wrapped in nav bars, script tags, and a cookie banner, it gets back a markdown document: headings, paragraphs, a code fence, the canonical link targets, and nothing else. The response carries `Content-Type: text/markdown`, `Vary: Accept`, `X-Robots-Tag: noindex`, and `Cache-Control: private`. The same origin also publishes a synthesized `/llms.txt` site-index built from its own sitemap.

This is the feature in the mod_pagespeed optimizer worker that **serves markdown to AI crawlers** on the same URL a browser uses. It ships in the worker but is **experimental and off by default**; it is content negotiation, not a separate endpoint or a separate build. Two things up front. The markdown variant is **operator-gated**: it is selected only when you have enabled agent optimization and the request asks for it. And the whole pipeline **honors AI opt-out signals**: `robots.txt` AI blocks, `X-Robots-Tag: noai`, `Google-Extended`, and known AI user agents. Both are in the code below.

## How to serve markdown to AI crawlers: SAX HTML to markdown, with a `<main>`/`<article>` guard

The rendering itself is a SAX filter, `MarkdownExtractorFilter` (`lib/html/markdown_extractor_filter.cc`). It consumes the event stream of the rendered outerHTML and accumulates markdown while maintaining its own explicit block, list, table, and skip stacks. It never queries a tree. `ExtractAgentMarkdown(html, url)` is the pure entry point.

The interesting decisions are about what to strip and how to fail safe:

- **Hard strip** — `script`, `style`, `noscript`, `template`, `head`, `svg`, and inline-hidden elements (`display:none`, `visibility:hidden`, `hidden`, `aria-hidden="true"` on the element's own attributes) are always dropped. There is no computed style in a SAX pass, so class-driven hiding is a documented limit, not a bug it pretends to handle.
- **Soft strip** — `nav`, `header`, and `footer` are dropped as boilerplate by default. But this is where the guard lives. If a `<main>`, `<article>`, or `role="main"`/`role="article"` content root is found *inside* a soft-chrome subtree that the strip would discard, the filter sets `dropped_content_root()` and the extractor fails open: it re-runs with `strip_soft_chrome=false`, keeping the chrome so the content is not silently lost. A page that lays its whole article inside a `<header>` still yields content-complete markdown.

The output is treated as attacker-controlled, because it is — the input is a hostile page's rendered DOM and the consumer is an AI agent. So the filter is self-bounded and escapes structure rather than trusting the page. URLs in `href`/`src` are percent-encoded for the markdown-structural bytes (`(`, `)`, `<`, `>`, `"`, `\`, space) so a crafted href cannot break out of `(...)` and forge a link; control bytes are dropped (`SanitizeUrl`). Link/alt text gets `[`, `]`, `(`, `)` backslash-escaped and CR/LF folded to a space (`EscapeLinkText`). A leading `#`/`>`/`|`/`-`/`+` in paragraph text is escaped so it cannot forge a heading, quote, table, or list (`EscapeLeadingMarker`). Inline `<code>` and `<pre>` get an adaptive backtick fence longer than any run inside the content, so a backtick in the payload cannot close the span early.

The bounds are constants in the header, not magic in the body: an 8 MiB output cap (`kMaxMarkdownBytes`), a 16-level list-indent emission clamp (`kMaxListIndentLevels`), and an 8-deep blockquote prefix clamp (`kMaxBlockquoteDepth`). The cap is enforced on the staging buffers too (`cur_`, `cell_`, `code_buf_`, `table_`), so a single never-flushed block can't leak past it. The filter does not trust the caller's input cap.

One contract, from the header: the extracted markdown is *not* guaranteed equal to human-visible content in either direction, because stylesheet-driven hiding is invisible to a SAX pass. Downstream consumers must treat it as untrusted, attacker-influenced input. We don't claim the agent sees exactly what a human sees.

## The serve-side gate, and why an ungated request still gets a page

Asking for markdown is two independent signals, ANDed together at serve time.

The request side is `WantsAgentMarkdown(accept)` (`lib/classify/capability_mask.cc`). It is presence-only: the literal `text/markdown` token must appear in `Accept`. It is *not* q-ranked, and `Accept: */*` does **not** opt into markdown — a generic wildcard agent gets the normal response. The intent is kept separate from the 32-bit `CapabilityMask` (which has no free bit) as its own one-bit signal on the request context.

The serve side is the operator's `--agent-optimize` toggle as published by the worker, `g_shared_config.agent_optimize_entitled`. In the nginx interceptor (`src/nginx/ngx_pagespeed_module.cc`), the AND is computed explicitly before the cache read:

```cpp
bool agent_request_entitled =
    ctx->agent_wants_markdown && g_shared_config.agent_optimize_entitled;

auto read_result = cache->ReadBestAlternateAgentByKey(cache_key, ctx->mask,
                                                      agent_request_entitled);
```

Only when both are true can the selector return the `kAgentMarkdown` sentinel variant — alternate id `0x7C` (`lib/classify/alternate_id.h`). The gating applies to that one sentinel and nothing else. A request on a worker where the flag is off, or one that does not name `text/markdown`, transparently gets the normal best variant. There is no hard decline, no 406, no degraded HTML path for the ordinary visitor. The markdown variant is an additional, gated alternate alongside the WebP/AVIF/mobile/desktop variants stored under the same key.

When the markdown variant is served, the module emits the headers an AI-consumption, non-indexed, content-negotiated response should carry: `Vary: Accept`, `X-Robots-Tag: noindex`, and `Cache-Control: private`. The `Content-Type` is the origin content type stored in the variant's metadata, which the worker stamps as `text/markdown` (`browser_analysis_manager.cc`), copied through verbatim on the cache-hit path. (The `; charset=utf-8` you do see on the synthesized `/llms.txt` is hardcoded on that path; the per-URL variant carries the bare type.) The normal freshness `Cache-Control` and `Content-Encoding` blocks are skipped for it: `0x7C` decodes to gzip bits in the mask, so emitting them would produce a spurious `Content-Encoding` and a conflicting cache directive.

So: operator-gated, served as `text/markdown`, private and non-indexed. Not on by default. The `--agent-optimize` flag on the worker is what enables the `0x7C` selection; everything else the worker does is unaffected by it.

## Synthesizing /llms.txt from the sitemap

`/llms.txt` is a site-index for agents — a short, link-first map of what the site is. The optimizer worker synthesizes one from the customer's own sitemap rather than asking anyone to hand-author it. The builder is `LlmsTxtBuilder::Build` (`src/worker/llms_txt_builder.cc`), and it is pure of process effects: the two blocking operations (an own-origin HTTP GET, and a read of an existing agent-markdown variant from cache) are injected functions, so the whole orchestration is unit-testable without a network or a render.

The flow, in order:

1. **AI directive check first.** If `respect_ai_directives` is set (the default), the builder fetches `/robots.txt` and runs `AnalyzeRobotsForAi`. If the site blocks AI crawling at the root, synthesis is abandoned with `kAiBlocked` — `/llms.txt` is itself an AI-consumption artifact, so a site-wide AI block suppresses it. If `robots.txt` is unreachable, the builder leans permissive and continues.
2. **Fetch and hash the sitemap.** It GETs `/sitemap.xml` (configurable), records a SHA-256 of the bytes for freshness, parses out page URLs, and does a bounded one-level fan-out into nested same-origin sitemaps (`kLlmsTxtMaxNestedSitemaps = 10`). If no sitemap is reachable, it falls back to the concrete (non-wildcard) allow-path entries as the page set.
3. **Filter to own-origin, allow-listed, deduped, capped.** Pages are matched against the default-port-normalized own host, filtered through the `agent_optimize_paths` allow-list, deduplicated, and capped at `kLlmsTxtMaxEntries = 5000`.
4. **Derive a title and summary per page.** A free summary comes from an already-rendered agent-markdown variant when one exists in cache (the first `# ` heading and the first following line). Otherwise a cheap own-origin GET (no Chrome render) provides both the page's title/meta-description *and* the per-page AI-directive evaluation. Pages are grouped into sections by their first path segment, with top-level pages under "Pages".
5. **Assemble** with `FormatLlmsTxt` and return `kOk`.

The own-origin GET in step 4 is doing double duty on purpose: when directives are respected, every indexed page must be evaluated, so the builder fetches even when a cached variant already gave it a summary (the rendered variant carries no response headers). It is bounded by `summary_fetch_cap` (default 200 cheap fetches per build).

## Honoring the opt-out: noai, Google-Extended, and known AI agents

The opt-out handling lives in `lib/html/robots_ai_directives.cc` and is applied at two scopes.

Site-wide, `AnalyzeRobotsForAi` parses `robots.txt` into user-agent groups and reports `ai_blocked_site_wide` when the wildcard `User-agent: *` group disallows `/`, **or** when any recognized AI crawler's own group disallows `/`. The recognized set (`KnownAiUserAgents`) is auditable in one place and currently includes `gptbot`, `google-extended`, `ccbot`, `claudebot`, `anthropic-ai`, `perplexitybot`, `bytespider`, `amazonbot`, `applebot-extended`, `meta-externalagent`, `cohere-ai`, `diffbot`, `omgilibot`, `facebookbot`, `imagesiftbot`, `claude-web`, and `google-cloudvertexbot`. An exact UA match beats the wildcard group, and `Allow: /` beats `Disallow: /`.

Per page, `PageExcludedByAiDirectives` drops a page from the index when any of these say no:

- an `X-Robots-Tag` response header carrying `noai`, `noimageai`, or `none`;
- a `<meta name="robots">` content carrying `noai`, `noimageai`, or `none`;
- a `Google-Extended` response header set to `none`.

Token matching is case-insensitive and comma/space/semicolon delimited, so `noindex, noai` is caught. And the builder is **fail-closed** on evaluation: if directives are respected but a page could not be evaluated — for instance, it fell beyond the per-build fetch cap — it is omitted rather than indexed on a guess. A failed fetch (network error, 4xx, 5xx) is *not* treated as an opt-out, so a transient error doesn't quietly drop a page that never said no. The safe default in each direction is the conservative one.

## Related

- [/blog/serve-markdown-to-ai-agents/](/blog/serve-markdown-to-ai-agents/) — the flags-and-gates overview of the same feature; this post is the code-level deep dive.
- [/blog/agentic-web-at-the-origin/](/blog/agentic-web-at-the-origin/) — the pillar on origin-resident agent features: verification, access control, markdown, and provenance.
- [/blog/can-ai-read-your-website/](/blog/can-ai-read-your-website/) — the data behind why client-side-rendered pages go blind to crawlers; this post is the shipped server-side answer.
- [/blog/sentinel-cache-keys-and-103-early-hints/](/blog/sentinel-cache-keys-and-103-early-hints/) — the reserved alternate-id space the `0x7C` markdown variant lives in.
- [/blog/search-console-ai-performance-report-vs-page-speed/](/blog/search-console-ai-performance-report-vs-page-speed/) — measuring AI/answer-engine traffic against page speed.
- [/blog/why-i-rebuilt-mod-pagespeed/](/blog/why-i-rebuilt-mod-pagespeed/) — why 2.0 is an independent rebuild, not a fork.
- [/blog/remove-unused-javascript-chrome-coverage/](/blog/remove-unused-javascript-chrome-coverage/) — another use of the rendered-DOM pipeline this feature shares.
- [/how-it-works/metadata-cache/](/how-it-works/metadata-cache/) — how variants and sentinels are keyed and stored.

If you run a docs site or a content property and you want crawlers to read it as markdown instead of guessing at your DOM, the extractor, the `/llms.txt` builder, and the opt-out handling are all in the shipped 2.0 worker. See [/docs/agent-optimize/](/docs/agent-optimize/) for the flags and the `--agent-optimize-paths` allow-list. To see what a crawler reads on one of your pages today, and whether the scan finds an `/llms.txt` on your site, use the free [AI readability check](/ai-readability/).

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
