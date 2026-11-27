// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { visit, SKIP } from 'unist-util-visit';
import { toString } from 'mdast-util-to-string';

/**
 * remark plugin: put inline `:word` text back as it was written.
 *
 * remark-directive is here for the `:::caution` callouts, but it also reads
 * any `:word` inside prose as an inline (text) directive: `host:port`,
 * `CookieName[:RedirectUrl]`, `nginx:stable`. The site defines no inline
 * directives, so such a node would render as an empty `<div>`, drop the word
 * and split the paragraph around it. This plugin turns every text directive
 * back into the literal text it was parsed from.
 *
 * It must run after remark-directive.
 */
export default function remarkLiteralTextDirectives() {
  return (/** @type {import('mdast').Root} */ tree, /** @type {import('vfile').VFile} */ file) => {
    const source = typeof file?.value === 'string' ? file.value : '';
    visit(tree, 'textDirective', (node, index, parent) => {
      if (!parent || index === undefined) return;
      const start = node.position?.start.offset;
      const end = node.position?.end.offset;
      let value = start !== undefined && end !== undefined ? source.slice(start, end) : '';
      if (!value.startsWith(':')) {
        // No source to slice: rebuild the common forms.
        const label = node.children?.length ? `[${toString(node)}]` : '';
        const attributes = Object.entries(node.attributes ?? {})
          .map(([k, v]) => (v ? `${k}="${v}"` : k))
          .join(' ');
        value = `:${node.name}${label}${attributes ? `{${attributes}}` : ''}`;
      }
      parent.children.splice(index, 1, { type: 'text', value });
      return [SKIP, index + 1];
    });
  };
}
