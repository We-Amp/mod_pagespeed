// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The docs navigation tree: the groups and links the docs sidebar and the
// phone "Docs menu" show, built from the docs collection.
//
// It is the collection's own grouping (frontmatter `group`, pages by `order`)
// with two differences:
//   - the per-line release-notes pages are not listed. "Release notes" is one
//     entry; that page links every line.
//   - the filter reference (/docs/filters/) is not a collection page, so it
//     is added by hand under "Filters".
//
// Reading order (previous / next) still follows the whole collection.

export interface DocsNavEntry {
  id: string;
  data: { title: string; order: number; group?: string };
}

export interface DocsNavItem {
  /** What a page passes as `current`: the docs slug, or FILTER_REFERENCE. */
  key: string;
  href: string;
  label: string;
}

export interface DocsNavGroup {
  label: string;
  items: DocsNavItem[];
}

export interface DocsNavLocation {
  /** Label of the group that is open on this page. */
  group: string | null;
  /** Key of the entry that is this page, when the page has its own entry. */
  item: string | null;
}

/** `current` for /docs/filters/; a filter page passes `filters/<name>`. */
export const FILTER_REFERENCE = 'filters';

const FILTERS_GROUP = 'Filters';
const RELEASE_LINES_GROUP = 'Release notes by line';
const RELEASE_NOTES = 'release-notes';

export function docsNavGroups(docs: DocsNavEntry[]): DocsNavGroup[] {
  const groups: DocsNavGroup[] = [];
  for (const d of [...docs].sort((a, b) => a.data.order - b.data.order)) {
    const label = d.data.group ?? 'Other';
    if (label === RELEASE_LINES_GROUP) continue;
    if (groups.at(-1)?.label !== label) groups.push({ label, items: [] });
    groups.at(-1)!.items.push({ key: d.id, href: `/docs/${d.id}/`, label: d.data.title });
  }
  const filters = groups.find((g) => g.label === FILTERS_GROUP);
  if (filters) {
    // After "Filter selection" (how to choose), before the category pages.
    const after = filters.items.findIndex((i) => i.key === 'filter-selection');
    filters.items.splice(after + 1, 0, {
      key: FILTER_REFERENCE,
      href: '/docs/filters/',
      label: 'Filter reference',
    });
  }
  return groups;
}

/**
 * Where `current` sits in the tree. A page without an entry of its own opens
 * the group of the entry that leads to it: a filter page the group holding the
 * filter reference, a per-line release-notes page the group holding "Release
 * notes". No entry is marked as the current page for those.
 */
export function docsNavLocation(
  groups: DocsNavGroup[],
  docs: DocsNavEntry[],
  current: string,
): DocsNavLocation {
  const groupOf = (key: string) =>
    groups.find((g) => g.items.some((i) => i.key === key))?.label ?? null;
  const own = groupOf(current);
  if (own) return { group: own, item: current };
  if (current.startsWith(`${FILTER_REFERENCE}/`)) {
    return { group: groupOf(FILTER_REFERENCE), item: null };
  }
  const doc = docs.find((d) => d.id === current);
  if (doc?.data.group === RELEASE_LINES_GROUP) return { group: groupOf(RELEASE_NOTES), item: null };
  return { group: null, item: null };
}
