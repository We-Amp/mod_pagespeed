// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { visit } from 'unist-util-visit';

/**
 * rehype plugin: list the ids of the headings inside next-release markers.
 *
 * A heading inside a `:::next-release` marker (the `aside.callout-next-release`
 * remarkCallouts renders) belongs to behaviour that is not released yet, so the
 * docs page leaves it out of "On this page". Astro collects every heading into
 * `headings` again at the end of the pipeline, so the list cannot be pruned
 * here; this plugin records the ids in the frontmatter instead
 * (`nextReleaseHeadingIds`, read through `remarkPluginFrontmatter`), and
 * src/pages/docs/[slug].astro filters on them. The headings themselves keep
 * their ids and anchors.
 *
 * It runs after rehypeHeadingIds, so every heading already has its id.
 */
export default function rehypeNextReleaseHeadings() {
  return (/** @type {import('hast').Root} */ tree, /** @type {any} */ file) => {
    /** @type {string[]} */
    const ids = [];
    visit(tree, 'element', (node) => {
      const cls = node.properties?.className;
      if (
        node.tagName !== 'aside' ||
        !Array.isArray(cls) ||
        !cls.includes('callout-next-release')
      ) {
        return;
      }
      visit(node, 'element', (child) => {
        const id = child.properties?.id;
        if (/^h[1-6]$/.test(child.tagName) && typeof id === 'string') ids.push(id);
      });
    });
    file.data.astro ??= {};
    file.data.astro.frontmatter ??= {};
    file.data.astro.frontmatter.nextReleaseHeadingIds = ids;
  };
}
