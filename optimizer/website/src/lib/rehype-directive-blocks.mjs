// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * rehype plugin: give the generated directive reference the hooks a script
 * and a stylesheet can work with.
 *
 * The reference region of the configuration page is flat Markdown: an `h3` per
 * area, an `h4` per directive, and the directive's list and paragraphs as
 * siblings. This plugin keeps that flat run and only marks it at render time:
 *
 *   <div data-directive-reference>
 *     …the region's introduction…
 *     <div data-directive-finder>…label, input, status…</div>
 *     <h3 id data-directive-area>…</h3>
 *     <p data-directive-index>…links to the area's directives…</p>
 *     <h4 id data-directive="Name">…</h4>
 *     …the directive's text, up to the next h4 or h3…
 *     …
 *   </div>
 *
 * No element is added per directive or per area: on a page with hundreds of
 * directives, a wrapper for each makes the page slower to lay out. A
 * directive's text is everything after its heading up to the next heading;
 * the finder's script hides that run element by element.
 *
 * Only the rendered HTML changes. The Markdown source, the generated region
 * in it, the `.md` twin of the page and `llms-full.txt` are untouched, and
 * every heading keeps its id. A page without the region is left alone.
 *
 * The script can only hide elements, so nothing in the region may end up as
 * loose text once the browser has parsed the page. A paragraph that holds a
 * block element (the browser closes the paragraph there and leaves the rest
 * of its text loose) is split the way the browser splits it, with each loose
 * run of text in a `span`; the page lays out exactly as before.
 *
 * It runs before rehype-raw, so HTML written in the Markdown (comments, the
 * `<a id>` anchors) is still `raw` nodes here.
 */

export const REGION = 'module-directives';

/** The finder's visible strings. */
export const FINDER_LABEL = 'Find a directive';

/** @param {import('hast').RootContent} node */
const isHeading = (node, tag) => node.type === 'element' && node.tagName === tag;

/** @param {import('hast').RootContent} node */
const isMarker = (node, edge) =>
  node.type === 'raw' && node.value.trim() === `<!-- generated:${edge} ${REGION} -->`;

/** @param {import('hast').Nodes} node @returns {string} */
function text(node) {
  if (node.type === 'text') return node.value;
  return 'children' in node ? node.children.map(text).join('') : '';
}

/**
 * @param {string} tagName
 * @param {Record<string, unknown>} properties
 * @param {import('hast').ElementContent[]} children
 * @returns {import('hast').Element}
 */
const el = (tagName, properties, children) => ({ type: 'element', tagName, properties, children });

const newline = () => /** @type {import('hast').Text} */ ({ type: 'text', value: '\n' });

/** Elements whose start tag closes an open paragraph in an HTML parser. */
const CLOSES_P = new Set(
  'address article aside blockquote details dialog div dl fieldset figcaption figure footer form h1 h2 h3 h4 h5 h6 header hgroup hr main menu nav ol p pre search section table ul'.split(
    ' ',
  ),
);

/** @param {import('hast').Nodes} node */
const isLoose = (node) => node.type === 'text' && node.value.trim() !== '';

/**
 * Top-level nodes of the region as the browser will see them: a paragraph
 * with a block element inside is split where the parser closes it, and any
 * loose text run is wrapped in a `span` so it can be hidden.
 *
 * @param {import('hast').ElementContent[]} nodes
 * @returns {import('hast').ElementContent[]}
 */
function noLooseText(nodes) {
  /** @type {import('hast').ElementContent[]} */
  const out = [];
  /** @param {import('hast').ElementContent[]} run */
  const flush = (run) => {
    if (run.some(isLoose)) out.push(el('span', {}, run));
    else out.push(...run);
  };
  for (const node of nodes) {
    if (isLoose(node)) {
      out.push(el('span', {}, [node]));
      continue;
    }
    const split =
      node.type === 'element' &&
      node.tagName === 'p' &&
      node.children.some((c) => c.type === 'element' && CLOSES_P.has(c.tagName));
    if (!split) {
      out.push(node);
      continue;
    }
    // `<p>a<div></div>b</p>` parses as `<p>a</p><div></div>b<p></p>`.
    let first = true;
    /** @type {import('hast').ElementContent[]} */
    let run = [];
    for (const child of node.children) {
      if (child.type === 'element' && CLOSES_P.has(child.tagName)) {
        if (first) out.push({ ...node, children: run });
        else flush(run);
        first = false;
        run = [];
        out.push(child);
      } else {
        run.push(child);
      }
    }
    flush(run);
    out.push(el('p', {}, []));
  }
  return out;
}

/** The finder: a labelled input and the line that reports the result. */
function finder() {
  return el('div', { className: ['directive-finder'], dataDirectiveFinder: '' }, [
    el('label', { htmlFor: 'directive-filter' }, [{ type: 'text', value: FINDER_LABEL }]),
    el(
      'input',
      {
        id: 'directive-filter',
        type: 'search',
        autoComplete: 'off',
        autoCapitalize: 'off',
        spellCheck: 'false',
        ariaDescribedBy: 'directive-filter-status',
      },
      [],
    ),
    el('p', { id: 'directive-filter-status', role: 'status' }, []),
  ]);
}

export default function rehypeDirectiveBlocks() {
  return (/** @type {import('hast').Root} */ tree) => {
    const begin = tree.children.findIndex((n) => isMarker(n, 'begin'));
    if (begin === -1) return;
    const end = tree.children.findIndex((n, i) => i > begin && isMarker(n, 'end'));
    if (end === -1) return;

    /** @type {import('hast').ElementContent[]} */
    const intro = [];
    /** @type {import('hast').ElementContent[]} */
    const body = [];
    let areas = 0;
    /** @type {'intro' | 'area' | 'directive'} */
    let where = 'intro';

    for (const node of noLooseText(
      /** @type {import('hast').ElementContent[]} */ (tree.children.slice(begin + 1, end)),
    )) {
      if (isHeading(node, 'h3')) {
        where = 'area';
        areas += 1;
        node.properties = { ...node.properties, dataDirectiveArea: '' };
      } else if (where !== 'intro' && isHeading(node, 'h4')) {
        where = 'directive';
        node.properties = { ...node.properties, dataDirective: text(node).trim() };
      } else if (where === 'area' && isHeading(node, 'p')) {
        // Between the area heading and its first directive: the links to the
        // area's directives.
        node.properties = { ...node.properties, dataDirectiveIndex: '' };
      }
      (where === 'intro' ? intro : body).push(node);
    }
    if (areas === 0) return;

    const reference = el('div', { dataDirectiveReference: '' }, [
      ...intro,
      finder(),
      newline(),
      ...body,
    ]);
    tree.children.splice(begin + 1, end - begin - 1, newline(), reference, newline());
  };
}
