// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * rehype plugin: a "#" permalink on every h2, h3 and h4 of a docs or
 * how-it-works page, rendered into the HTML at build time.
 *
 *   <h2 id="roll-back" aria-labelledby="roll-back--text">
 *     <span id="roll-back--text">Roll back</span>
 *     <a class="heading-anchor" href="#roll-back" aria-label="Link to “Roll back”"></a>
 *   </h2>
 *
 * The link holds no text. The "#" it shows is CSS generated content
 * (`.heading-anchor::after` in global.css), so it is not part of the heading's
 * text: not in textContent or innerText, not in a copy, not in reader mode, a
 * crawler's text or a Markdown copy taken from the page. A permalink that put
 * a "#" text node inside the heading made every one of those read
 * "Roll back#".
 *
 * The heading takes its accessible name from its own text (the span), so the
 * link's label does not join it and a screen reader's headings list reads
 * each heading once; the link keeps its own descriptive name.
 *
 * Rendering it here instead of from a client script means the permalink is in
 * the HTML as served: it works without JavaScript and does not depend on a
 * script running after an optimizer has rewritten the page.
 *
 * Runs after rehypeHeadingIds (listed before this plugin in astro.config), so
 * every heading already has the id the page's table of contents links to.
 */

import { fileURLToPath } from 'node:url';

/** Content folders whose pages get heading permalinks. */
const SCOPES = ['/src/content/docs/', '/src/content/how-it-works/'];

const LEVELS = new Set(['h2', 'h3', 'h4']);

/** @param {import('hast').Nodes} node @returns {string} */
function text(node) {
  if (node.type === 'text') return node.value;
  return 'children' in node ? node.children.map(text).join('') : '';
}

/** @param {import('hast').Nodes} node @param {(el: import('hast').Element) => void} fn */
function eachElement(node, fn) {
  if (node.type === 'element') fn(node);
  if ('children' in node) for (const c of node.children) eachElement(c, fn);
}

/** @param {import('vfile').VFile} file */
function inScope(file) {
  const p = file.path || file.history?.[0] || '';
  const abs = p.startsWith('file:') ? fileURLToPath(p) : p;
  const norm = abs.replace(/\\/g, '/');
  return SCOPES.some((s) => norm.includes(s));
}

/**
 * Add the permalink to one heading element. Exported for the unit test.
 * @param {import('hast').Element} h
 */
export function addAnchor(h) {
  const id = h.properties?.id;
  if (typeof id !== 'string' || id === '') return;
  if (h.children.some((c) => c.type === 'element' && hasClass(c, 'heading-anchor'))) return;
  const label = text(h).replace(/\s+/g, ' ').trim();
  if (label && h.properties.ariaLabelledBy == null && h.properties.ariaLabel == null) {
    const textId = `${id}--text`;
    h.children = [
      { type: 'element', tagName: 'span', properties: { id: textId }, children: h.children },
    ];
    h.properties.ariaLabelledBy = [textId];
  }
  h.children.push({
    type: 'element',
    tagName: 'a',
    properties: {
      className: ['heading-anchor'],
      href: `#${id}`,
      ariaLabel: label ? `Link to “${label}”` : 'Link to this section',
    },
    children: [],
  });
}

/** @param {import('hast').Element} el @param {string} cls */
function hasClass(el, cls) {
  const c = el.properties?.className;
  return Array.isArray(c) ? c.includes(cls) : c === cls;
}

export default function rehypeHeadingAnchors() {
  /** @param {import('hast').Root} tree @param {import('vfile').VFile} file */
  return (tree, file) => {
    if (!inScope(file)) return;
    eachElement(tree, (el) => {
      if (LEVELS.has(el.tagName)) addAnchor(el);
    });
  };
}
