// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// /blog/<slug>.md: every blog post as Markdown, for agents and for anyone who
// wants the text without the chrome. The body is the one
// src/lib/blog-markdown.mjs renders; the posts are not part of
// public/llms-full.txt (they are dated commentary, each with its own twin), and
// public/llms.txt lists them. Each HTML page advertises its twin with
// <link rel="alternate" type="text/markdown">.
//
// Prerendered: the files are static assets in dist/client/blog/, served by the
// node adapter like any other asset. Excluded from the sitemap (astro.config)
// and never indexed by Pagefind, which reads HTML only.

import type { APIRoute, GetStaticPaths } from 'astro';
import { loadPosts, renderPostMarkdown } from '../../lib/blog-markdown.mjs';

export const prerender = true;

export const getStaticPaths = (() =>
  loadPosts().map((post) => ({
    params: { slug: post.slug },
    props: { post },
  }))) satisfies GetStaticPaths;

export const GET: APIRoute = ({ props, params }) => {
  const post = props.post ?? loadPosts().find((p) => p.slug === params.slug);
  if (!post) return new Response('Not found', { status: 404 });
  return new Response(renderPostMarkdown(post), {
    headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
  });
};
