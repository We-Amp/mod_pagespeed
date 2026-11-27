// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Which gallery demos a filter topic page links to, beyond the one that
// isolates the filter. One answer for every surface that lists them: the topic
// page (src/pages/docs/filters/[name].astro), its Markdown twin
// (src/pages/docs/filters/[name].md.ts) and public/llms-full.txt
// (scripts/generate-llms.mjs, which loads this module through Vite), so the
// HTML and the Markdown cannot list different demos.

import { examplesApplying } from '../data/examples';
import { demonstrates } from './example-impact';

export interface DemoLink {
  slug: string;
  title: string;
}

/** The gallery slug of a topic's `example` path (/examples/<slug>/). */
export function exampleSlugOf(example: string | null | undefined): string | undefined {
  return example?.match(/^\/examples\/([^/]+)\/$/)?.[1];
}

/**
 * The other gallery demos that apply `name` alongside the one that isolates it
 * (or instead of it, when no demo isolates the filter). A demo whose latest
 * capture shows this filter did not act is left out until it does. Catalog
 * order.
 */
export function alsoAppliedIn(name: string, example: string | null | undefined): DemoLink[] {
  const exampleSlug = exampleSlugOf(example);
  return examplesApplying(name)
    .filter((ex) => ex.slug !== exampleSlug && demonstrates(ex.slug, name))
    .map((ex) => ({ slug: ex.slug, title: ex.title }));
}
