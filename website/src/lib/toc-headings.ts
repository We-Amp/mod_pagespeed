// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export interface Heading {
  depth: number;
  slug: string;
  text: string;
}

/**
 * The h2/h3 headings for "On this page", without the headings inside a
 * `:::next-release` marker, whose ids rehype-next-release-headings.mjs records
 * as `nextReleaseHeadingIds` in the rendered frontmatter. Those headings keep
 * their anchors on the page; they are only left out of the navigation.
 */
export function tocHeadings(
  headings: Heading[],
  frontmatter: Record<string, unknown> | undefined,
): Heading[] {
  const ids = frontmatter?.nextReleaseHeadingIds;
  const unreleased = new Set(Array.isArray(ids) ? ids : []);
  return headings.filter((h) => h.depth >= 2 && h.depth <= 3 && !unreleased.has(h.slug));
}
