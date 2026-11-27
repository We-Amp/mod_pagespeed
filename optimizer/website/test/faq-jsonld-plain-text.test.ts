// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// faq-jsonld-plain-text.test.ts — a docs page renders its `faq` frontmatter
// twice: as the visible accordion (Markdown rendered to HTML) and as FAQPage
// JSON-LD. The structured data must carry the answer a reader sees, not the
// Markdown source. Runs after `npm run build` and skips itself otherwise (like
// anchor-gate.test.ts): on every built docs page with an FAQ section, each
// JSON-LD answer has no Markdown syntax (backticks, `**`, link syntax, or an
// em dash glued to a word, the trace of a `--flag` outside a code span) and
// equals the plain text of the visible answer for the same question.

import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse, serialize, serializeOuter } from 'parse5';
import { describe, it, expect } from 'vitest';
import { htmlToPlainText } from '../src/lib/jsonld';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DOCS = path.join(WEBSITE_ROOT, 'dist/client/docs');

const BUILT = existsSync(DOCS);
if (!BUILT) {
  console.warn(
    `[faq-jsonld-plain-text] built output not found at ${DOCS} — skipping. Run \`npm run build\` first.`,
  );
}

function pages(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const full = path.join(dir, name);
    if (statSync(full).isDirectory()) out.push(...pages(full));
    else if (name === 'index.html') out.push(full);
  }
  return out;
}

type Answer = { q: string; a: string };

type HtmlNode = {
  nodeName: string;
  value?: string;
  attrs?: { name: string; value: string }[];
  childNodes?: HtmlNode[];
};

function* elements(node: HtmlNode, tag: string): Generator<HtmlNode> {
  for (const child of node.childNodes ?? []) {
    if (child.nodeName === tag) yield child;
    yield* elements(child, tag);
  }
}

const attr = (node: HtmlNode, name: string) => node.attrs?.find((a) => a.name === name)?.value;

function jsonLdAnswers(doc: HtmlNode): Answer[] {
  const out: Answer[] = [];
  for (const script of elements(doc, 'script')) {
    if (attr(script, 'type') !== 'application/ld+json') continue;
    const text = (script.childNodes ?? []).map((c) => c.value ?? '').join('');
    const node = JSON.parse(text) as { '@type'?: string; mainEntity?: unknown };
    if (node['@type'] !== 'FAQPage' || !Array.isArray(node.mainEntity)) continue;
    for (const q of node.mainEntity as { name: string; acceptedAnswer: { text: string } }[]) {
      out.push({ q: q.name, a: q.acceptedAnswer.text });
    }
  }
  return out;
}

// The visible answer: the question is the <summary> text (its icon left out),
// the answer the <p> that follows it, each read through the same plain-text
// helper the structured data uses.
function visibleAnswers(doc: HtmlNode): Answer[] {
  const section = [...elements(doc, 'section')].find((s) => attr(s, 'id') === 'faq');
  if (!section) return [];
  return [...elements(section, 'details')].map((details) => {
    const children = details.childNodes ?? [];
    const summary = children.find((c) => c.nodeName === 'summary');
    const answer = children.find((c) => c.nodeName === 'p');
    const question = {
      ...summary,
      nodeName: 'summary',
      childNodes: (summary?.childNodes ?? []).filter((c) => c.nodeName !== 'svg'),
    };
    return {
      q: htmlToPlainText(serializeOuter(question as never)),
      a: htmlToPlainText(answer ? serialize(answer as never) : ''),
    };
  });
}

describe.skipIf(!BUILT)('docs FAQPage answers match the visible answers', () => {
  const built = BUILT
    ? pages(DOCS).map((file) => ({ file, doc: parse(readFileSync(file, 'utf8')) as HtmlNode }))
    : [];
  const withFaq = built.filter(({ doc }) =>
    [...elements(doc, 'section')].some((s) => attr(s, 'id') === 'faq'),
  );

  it('finds docs pages with an FAQ section', () => {
    expect(withFaq.length).toBeGreaterThan(0);
  });

  it('no FAQPage answer on a docs page carries Markdown syntax', () => {
    const offenders = built.flatMap(({ file, doc }) =>
      jsonLdAnswers(doc)
        // A dash glued to a word is a `--flag` outside a code span that the
        // Markdown typographer turned into an em dash.
        .filter(({ a }) => /`|\*\*|\]\(|—[a-z]/.test(a))
        .map(({ q }) => `${path.relative(DOCS, file)}: ${q}`),
    );
    expect(offenders).toEqual([]);
  });

  for (const { file, doc } of withFaq) {
    it(`${path.relative(DOCS, file)}: the structured answers equal the visible ones`, () => {
      expect(jsonLdAnswers(doc)).toEqual(visibleAnswers(doc));
    });
  }
});
