// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { visit } from 'unist-util-visit';
import { toString } from 'mdast-util-to-string';

/**
 * Container-directive names we render as styled callouts. Anything else is
 * left untouched so a stray `:::foo` never silently disappears.
 * @type {Record<string, string>}
 */
const CALLOUT_TYPES = {
  note: 'Note',
  tip: 'Tip',
  caution: 'Caution',
  warning: 'Warning',
  danger: 'Danger',
  important: 'Important',
  // Behaviour that is merged but not in a release yet. Written
  // `:::next-release{after="X.Y.Z"}`, where X.Y.Z is the last released version
  // when the block was written; test/next-release-markers.test.ts fails once a
  // newer version is released, so the marker is removed in that release's
  // site update. See website/CLAUDE.md, "Unreleased behaviour".
  'next-release': 'From the next release',
};

/**
 * remark plugin: turn `remark-directive` container directives into styled
 * callout asides.
 *
 *   :::caution[Experimental]
 *   Body markdown…
 *   :::
 *
 * becomes `<aside class="callout callout-caution"><p class="callout-title">
 * Experimental</p>…</aside>`. The optional `[Label]` overrides the default
 * title (the capitalized directive name). Body content keeps rendering as
 * normal markdown, so inline code / links / emphasis inside a callout work.
 *
 * Without this plugin the `:::caution …:::` lines render as raw literal text —
 * which is exactly the bug this fixes on the docs pages that use them.
 */
export default function remarkCallouts() {
  return (/** @type {import('mdast').Root} */ tree) => {
    visit(tree, (node) => {
      if (node.type !== 'containerDirective') return;
      const defaultTitle = CALLOUT_TYPES[node.name];
      if (!defaultTitle) return;

      node.data ??= {};
      const children = node.children ?? [];

      // remark-directive parses an optional `[Label]` into the first child,
      // a paragraph flagged with `data.directiveLabel`. Lift its text into the
      // title and drop it from the body.
      let title = defaultTitle;
      const labelIndex = children.findIndex((c) => c.data && c.data.directiveLabel);
      if (labelIndex !== -1) {
        const labelText = toString(children[labelIndex]).trim();
        if (labelText) title = labelText;
        children.splice(labelIndex, 1);
      }

      children.unshift({
        type: 'paragraph',
        data: { hName: 'p', hProperties: { className: ['callout-title'] } },
        children: [{ type: 'text', value: title }],
      });

      node.data.hName = 'aside';
      node.data.hProperties = {
        className: ['callout', `callout-${node.name}`],
      };
      const after = node.attributes?.after;
      if (node.name === 'next-release' && after) {
        node.data.hProperties['data-released-after'] = after;
      }
    });
  };
}
