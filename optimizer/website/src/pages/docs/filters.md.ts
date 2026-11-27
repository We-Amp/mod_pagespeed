// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// /docs/filters.md: the Markdown twin of the filters hub (/docs/filters/): one
// table per category, every filter and compound name. The same text goes into
// public/llms-full.txt. No docs slug is `filters`, so this does not collide
// with /docs/<slug>.md.

import type { APIRoute } from 'astro';
import { loadFilterTopics, renderFiltersHubMarkdown } from '../../lib/filter-topics.mjs';

export const prerender = true;

export const GET: APIRoute = () =>
  new Response(renderFiltersHubMarkdown(loadFilterTopics()), {
    headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
  });
