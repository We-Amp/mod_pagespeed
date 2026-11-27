// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// noscript-forms.test.ts — the no-JavaScript inquiry forms on the two scan
// pages and the contact page, checked against the built output. Skips itself
// before `npm run build`, like scan-landing-seo.test.ts.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { parse, parseFragment } from 'parse5';
import { describe, it, expect } from 'vitest';

const DIST = path.join(fileURLToPath(new URL('..', import.meta.url)), 'dist/client');
const BUILT = existsSync(DIST);

interface Node {
  nodeName: string;
  value?: string;
  attrs?: { name: string; value: string }[];
  childNodes?: Node[];
  content?: Node;
}

const html = (route: string) => readFileSync(path.join(DIST, route, 'index.html'), 'utf8');

function all(node: Node, tag: string, out: Node[] = []): Node[] {
  if (node.nodeName === tag) out.push(node);
  (node.childNodes ?? []).forEach((c) => all(c, tag, out));
  if (node.content) all(node.content, tag, out);
  return out;
}

const attr = (n: Node, name: string) => n.attrs?.find((a) => a.name === name)?.value;

// With scripting enabled (the parse5 default) <noscript> content is a raw text
// node, so re-parse it as a fragment to inspect the form inside.
function noscriptForms(route: string): Node[] {
  const doc = parse(html(route)) as unknown as Node;
  const forms: Node[] = [];
  for (const ns of all(doc, 'noscript')) {
    const raw = (ns.childNodes ?? []).map((c) => c.value ?? '').join('');
    forms.push(...all(parseFragment(raw) as unknown as Node, 'form'));
  }
  return forms;
}

const hidden = (form: Node, name: string) =>
  all(form, 'input').find((i) => attr(i, 'type') === 'hidden' && attr(i, 'name') === name);

const FIELDS = ['email', 'name', 'url', 'message', 'company'];

function fieldNames(form: Node): string[] {
  return [...all(form, 'input'), ...all(form, 'textarea')]
    .filter((n) => attr(n, 'type') !== 'hidden' && attr(n, 'type') !== 'submit')
    .map((n) => attr(n, 'name')!)
    .sort();
}

describe.skipIf(!BUILT)('no-JavaScript inquiry forms (built output)', () => {
  for (const [route, topic] of [
    ['/analyze/', 'consulting'],
    ['/ai-readability/', 'agents'],
  ] as const) {
    it(`${route} has exactly one noscript form posting to the contact endpoint`, () => {
      const forms = noscriptForms(route);
      expect(forms).toHaveLength(1);
      const form = forms[0];
      expect(attr(form, 'action')).toBe('/ai-readability/api/contact');
      expect(attr(form, 'method')).toBe('post');
      expect(attr(hidden(form, 'topic')!, 'value')).toBe(topic);
      expect(attr(hidden(form, 'redirect')!, 'value')).toBe('/contact/thanks/');
      expect(fieldNames(form)).toEqual([...FIELDS].sort());
    });
  }

  it('/contact/ submits natively to the contact endpoint without JavaScript', () => {
    const doc = parse(html('/contact/')) as unknown as Node;
    const form = all(doc, 'form').find((f) => attr(f, 'id') === 'contact-form')!;
    expect(attr(form, 'action')).toBe('/ai-readability/api/contact');
    expect(attr(form, 'method')).toBe('post');
    expect(attr(form, 'enctype')).toBeUndefined();
    expect(attr(hidden(form, 'redirect')!, 'value')).toBe('/contact/thanks/');
    const names = [...all(form, 'input'), ...all(form, 'select'), ...all(form, 'textarea')].map(
      (n) => attr(n, 'name'),
    );
    for (const n of ['email', 'name', 'topic', 'message', 'company']) expect(names).toContain(n);
    // The page-load timestamp is filled by script, so it is empty in the HTML.
    expect(attr(hidden(form, 't')!, 'value')).toBe('');
  });
});
