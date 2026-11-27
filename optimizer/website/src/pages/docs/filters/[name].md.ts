// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// /docs/filters/<name>.md: the Markdown twin of each indexable filter topic
// page (/docs/filters/<name>/), for agents and for anyone who wants the text
// without the chrome. The body is the one src/lib/filter-topics.mjs renders
// for public/llms-full.txt, so the two never disagree; the demo list is the
// one the HTML page shows (src/lib/filter-demos.ts). The thin topics are
// noindex and have no twin (their .md is a 404); the HTML page advertises the
// twin with <link rel="alternate" type="text/markdown"> only when it exists.
//
// Prerendered like /docs/<slug>.md, excluded from the sitemap by astro.config
// and never indexed by Pagefind.

import type { APIRoute, GetStaticPaths } from 'astro';
import { loadFilterTopics, renderTopicMarkdown } from '../../../lib/filter-topics.mjs';
import { alsoAppliedIn } from '../../../lib/filter-demos';

export const prerender = true;

export const getStaticPaths = (() => {
  const topics = loadFilterTopics();
  return topics
    .filter((t) => t.indexable)
    .map((topic) => ({ params: { name: topic.name }, props: { topic, topics } }));
}) satisfies GetStaticPaths;

export const GET: APIRoute = ({ props }) =>
  new Response(
    renderTopicMarkdown(
      props.topic,
      props.topics,
      alsoAppliedIn(props.topic.name, props.topic.example),
    ),
    {
      headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
    },
  );
