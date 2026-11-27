// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from 'vitest';
import { htmlToPlainText, safeJsonLd } from './jsonld';

describe('safeJsonLd', () => {
  it('serializes like JSON.stringify when nothing needs escaping', () => {
    const obj = { '@type': 'WebSite', name: 'modpagespeed.com' };
    expect(safeJsonLd(obj)).toBe(JSON.stringify(obj));
  });

  it('cannot be closed by a FAQ answer containing </script>', () => {
    // The shape BaseLayout inlines: a FAQPage whose acceptedAnswer text is
    // content-authored frontmatter. An answer that names the closing tag must
    // not be able to end the script element it lives in.
    const faq = {
      '@context': 'https://schema.org',
      '@type': 'FAQPage',
      mainEntity: [
        {
          '@type': 'Question',
          name: 'Is it safe to write </script> in an answer?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'Yes — the string </script> stays inside the JSON block.',
          },
        },
      ],
    };
    const out = safeJsonLd(faq);
    expect(out).not.toContain('</script');
    expect(out).not.toContain('<');
    // Still one JSON value, and the same one: the answer round-trips intact.
    expect(JSON.parse(out)).toEqual(faq);
  });

  it('escapes < everywhere it appears, in keys and nested values alike', () => {
    const out = safeJsonLd({ 'a<b': ['<img', { c: '1<2' }] });
    expect(out).not.toContain('<');
    expect(JSON.parse(out)).toEqual({ 'a<b': ['<img', { c: '1<2' }] });
  });
});

describe('htmlToPlainText', () => {
  it('keeps the words of code spans, links and emphasis and drops the markup', () => {
    const html =
      'Scope the switch: <code>ModPagespeed off</code> inside a <code>&lt;Location&gt;</code> block; see <a href="/docs/configuration/">the <strong>configuration</strong> page</a>.';
    expect(htmlToPlainText(html)).toBe(
      'Scope the switch: ModPagespeed off inside a &lt;Location&gt; block; see the configuration page.',
    );
  });

  it('decodes numeric and named character references', () => {
    expect(htmlToPlainText('It&#39;s &quot;on&quot; &amp; ready &#x2014; yes')).toBe(
      'It\'s "on" & ready \u2014 yes',
    );
  });

  it('collapses the line breaks a soft-wrapped answer renders with', () => {
    expect(htmlToPlainText('one\ntwo  three')).toBe('one two three');
  });

  it('keeps angle brackets encoded so the text cannot read as a tag', () => {
    expect(htmlToPlainText('<code>&lt;img&gt;</code> and &#60;b&#62;')).toBe(
      '&lt;img&gt; and &lt;b&gt;',
    );
  });

  it('leaves an unknown reference as written', () => {
    expect(htmlToPlainText('a &bogus; b')).toBe('a &bogus; b');
  });
});
