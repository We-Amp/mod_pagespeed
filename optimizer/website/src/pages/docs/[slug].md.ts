// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// /docs/<slug>.md — every documentation page as Markdown, for agents and for
// anyone who wants the text without the chrome. The body is the one
// src/lib/docs-markdown.mjs renders for public/llms-full.txt, so the two never
// disagree. Each HTML page advertises its Markdown twin with
// <link rel="alternate" type="text/markdown"> and a "View as Markdown" link.
//
// Prerendered: the pages are static files in dist/client/docs/, served by the
// node adapter like any other asset. Excluded from the sitemap (astro.config)
// and never indexed by Pagefind, which reads HTML only.

import type { APIRoute, GetStaticPaths } from 'astro';
import { loadDocs, loadManifests, renderDocMarkdown } from '../../lib/docs-markdown.mjs';

export const prerender = true;

export const getStaticPaths = (() =>
  loadDocs().map((doc) => ({
    params: { slug: doc.slug },
    props: { doc },
  }))) satisfies GetStaticPaths;

export const GET: APIRoute = ({ props, params }) => {
  const doc = props.doc ?? loadDocs().find((d) => d.slug === params.slug);
  if (!doc) return new Response('Not found', { status: 404 });
  return new Response(renderDocMarkdown(doc, loadManifests()), {
    headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
  });
};
