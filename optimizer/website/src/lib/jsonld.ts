// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { parseFragment } from 'parse5';

/**
 * Structured data that cannot close its own script tag.
 *
 * Every JSON-LD block the site emits is inlined into the page as
 * `<script type="application/ld+json" set:html={...}>`, and several of them
 * carry content-authored strings: FAQ answers come from frontmatter, filter
 * summaries from the docs collection. A string containing `</script>` would
 * otherwise end the script element early and inject the rest of the string as
 * markup — the classic script-context injection.
 *
 * `safeJsonLd(value)` serializes like `JSON.stringify` and then replaces every
 * `<` with its JSON unicode escape `\u003c`. The escape is valid JSON anywhere
 * a character can appear, so `JSON.parse` of the output yields exactly the
 * same object, while the bytes `</script` can never occur in the page. Only
 * `<` is escaped: JSON syntax has no other character that HTML tokenization
 * treats specially inside a script element.
 */
export function safeJsonLd(value: unknown): string {
  return JSON.stringify(value).replace(/</g, '\\u003c');
}

type FragmentNode = { nodeName: string; value?: string; childNodes?: FragmentNode[] };

function textOf(node: FragmentNode): string {
  if (node.nodeName === '#text') return node.value ?? '';
  return (node.childNodes ?? []).map(textOf).join('');
}

/**
 * The text a reader sees in a rendered HTML fragment: tags dropped, character
 * references decoded, whitespace runs collapsed. Structured data that mirrors
 * a visible answer (a FAQPage `acceptedAnswer`) is built from the same rendered
 * HTML as the page, so the markup carries the visible words rather than the
 * Markdown source (no literal backticks, asterisks or link syntax).
 *
 * The fragment is read with an HTML parser, not pattern-matched. `<` and `>`
 * in the text come back encoded (`&lt;`, `&gt;`): consumers may read an
 * answer's text as HTML, where a literal `<Location>` would parse as a tag and
 * drop out.
 */
export function htmlToPlainText(html: string): string {
  return textOf(parseFragment(html) as FragmentNode)
    .replace(/[<>]/g, (c) => (c === '<' ? '&lt;' : '&gt;'))
    .replace(/\s+/g, ' ')
    .trim();
}
