// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Typed view of src/data/reference/filters.json, the filter reference that
// scripts/generate-references.mjs derives from the module source (the gperf
// filter-name table, the enum/id/label vector, the CoreFilters,
// OptimizeForBandwidth and dangerous sets, the compound-name expansions) and
// from src/data/reference/filters-overlay.json (category, summary, risk, note)
// and src/data/examples.ts (the demo per filter).
//
// Consumers: /docs/filters/ (src/pages/docs/filters.astro) renders the
// complete table and its ItemList JSON-LD from FILTERS, each row linking to the
// filter's topic page (/docs/filters/<name>/); the drift gates in
// test/reference/ assert that every name here has a section on exactly one
// group page and that the JSON still matches the pinned source.
//
// Do not hand-edit filters.json: regenerate it (`npm run gen:references`).

import data from './reference/filters.json';
import { topicPath, indexableTopicNames } from '../lib/filter-topics.mjs';

export type FilterCategory = 'Image' | 'CSS' | 'JavaScript' | 'HTML' | 'Caching';

export interface Filter {
  /** Filter name, exactly as passed to EnableFilters/DisableFilters. */
  name: string;
  /** Content type the filter transforms; decides the group page. */
  category: FilterCategory;
  /** One-line "what it does", plain text. */
  description: string;
  /** Deep link to the filter's own anchor on its group page. */
  href: string;
  /** The filter's topic page, /docs/filters/<name>/. */
  topic: string;
  /** The topic page is indexable (not held back by the thin-content guard). */
  topicIndexable: boolean;
  /** In the CoreFilters set (true), partly (an alias whose members differ) or not. */
  core: boolean | 'partial';
  /** In the OptimizeForBandwidth set. */
  optimizeForBandwidth: boolean | 'partial';
  /** Risk rating: from the source (Dangerous, Deprecated) or these docs; null when neither states one. */
  risk: string | null;
  /** A compound name that expands to member filters (rewrite_images, extend_cache, ...). */
  alias: boolean;
  /** Member filters of an alias. */
  members: string[];
  /** Canonical spelling when this name is an accepted alternate (left_trim_urls -> trim_urls). */
  alternateSpellingOf: string | null;
  /** Accepted but ignored by the module. */
  deprecated: boolean;
  /** In the dangerous set: never enabled by AllFilters, for testing only. */
  dangerous: boolean;
  /** /examples/<slug>/ demo that isolates this filter, when the gallery has one. */
  example: string | null;
  /** Short platform or status note from the overlay. */
  note: string | null;
}

/** Display order for grouping the table and JSON-LD. */
export const CATEGORY_ORDER: FilterCategory[] = ['Image', 'CSS', 'JavaScript', 'HTML', 'Caching'];

/** The docs page that documents each category's filters. */
export const PAGE_BY_CATEGORY: Record<FilterCategory, string> = {
  Image: 'image-filters',
  CSS: 'css-filters',
  JavaScript: 'javascript-filters',
  HTML: 'html-filters',
  Caching: 'cache-control',
};

/** The pinned module source the data was generated from. */
export const FILTERS_SOURCE = data.source;

/** CoreFilters, OptimizeForBandwidth and dangerous set membership, by name. */
export const FILTER_SETS = data.sets;

function asCategory(name: string, category: string | null): FilterCategory {
  if (category && (CATEGORY_ORDER as string[]).includes(category))
    return category as FilterCategory;
  throw new Error(`filters.json: ${name} has no category (add it to filters-overlay.json)`);
}

function hrefFor(category: FilterCategory, name: string): string {
  return `/docs/${PAGE_BY_CATEGORY[category]}/#${name}`;
}

const INDEXABLE = indexableTopicNames();

const single: Filter[] = data.filters.map((f) => {
  const category = asCategory(f.name, f.category);
  return {
    name: f.name,
    category,
    description: f.summary ?? f.label ?? f.name,
    href: hrefFor(category, f.name),
    topic: topicPath(f.name),
    topicIndexable: INDEXABLE.has(f.name),
    core: f.core,
    optimizeForBandwidth: f.optimizeForBandwidth,
    risk: f.risk,
    alias: false,
    members: [],
    alternateSpellingOf: f.alternateSpellingOf,
    deprecated: f.deprecated,
    dangerous: f.dangerous,
    example: f.example,
    note: f.note,
  };
});

const aliases: Filter[] = data.aliases.map((a) => {
  const category = asCategory(a.name, a.category);
  return {
    name: a.name,
    category,
    description: a.summary ?? a.name,
    href: hrefFor(category, a.name),
    topic: topicPath(a.name),
    topicIndexable: INDEXABLE.has(a.name),
    core: a.core as boolean | 'partial',
    optimizeForBandwidth: a.optimizeForBandwidth as boolean | 'partial',
    risk: a.risk,
    alias: true,
    members: a.members,
    alternateSpellingOf: null,
    deprecated: false,
    dangerous: false,
    example: a.example,
    note: a.note,
  };
});

/** Every filter name the module accepts, plus the compound names, alphabetical. */
export const FILTERS: Filter[] = [...single, ...aliases].sort((a, b) =>
  a.name.localeCompare(b.name, 'en'),
);

/** Filters grouped by category in display order, for the table and JSON-LD. */
export function filtersByCategory(): { category: FilterCategory; filters: Filter[] }[] {
  return CATEGORY_ORDER.map((category) => ({
    category,
    filters: FILTERS.filter((f) => f.category === category),
  }));
}

/**
 * The filter whose topic page is the full guide to an /examples/<slug>/ demo:
 * the filter of the same name, else the filter that lists this demo as its
 * example, else the first filter in the demo's PageSpeedFilters value.
 */
export function topicForExample(slug: string, filters: string): Filter | undefined {
  const byName = new Map(FILTERS.map((f) => [f.name, f]));
  return (
    byName.get(slug) ??
    FILTERS.find((f) => f.example === `/examples/${slug}/`) ??
    filters
      .split(',')
      // A +/- modifier list adjusts a configuration demo; it names no single
      // filter the demo isolates, so such a demo gets no guide link.
      .map((n) => (/^[+-]/.test(n.trim()) ? undefined : byName.get(n.trim())))
      .find((f): f is Filter => !!f)
  );
}
