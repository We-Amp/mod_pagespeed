// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filter-topics-helpers.test.ts — the pure helpers of filter-topics.mjs.
//
// filter-topics.test.ts gates the whole-page outputs against the real content;
// this file pins the small text helpers those outputs are built from, on
// hand-written markdown and minimal topic objects, so a content edit never
// breaks it. Nothing here reads src/content/ or src/data/reference/.

import { describe, it, expect } from 'vitest';
import {
  extractSection,
  faqHtml,
  namesGoogleProduct,
  parseBlocks,
  plainText,
  sentences,
  topicConfig,
  topicDescription,
  topicMarkdownUrl,
  topicPath,
  topicTitle,
  wordCount,
  workerEquivalents,
} from '../../src/lib/filter-topics.mjs';

/** parseBlocks + extractSection over a hand-written group-page fragment. */
function cut(md: string, page: string, name: string, names: string[]) {
  return extractSection(parseBlocks(md), page, name, new Set(names));
}

describe('topicPath and topicMarkdownUrl', () => {
  it('build the topic page path and the Markdown twin the page links', () => {
    expect(topicPath('rewrite_css')).toBe('/docs/filters/rewrite_css/');
    expect(topicMarkdownUrl('rewrite_css')).toBe('/docs/filters/rewrite_css.md');
  });
});

describe('parseBlocks', () => {
  it('splits paragraphs on blank lines and collapses the whitespace inside', () => {
    expect(parseBlocks('One  para\n   spans lines.\n\nNext   para.')).toEqual([
      { type: 'para', text: 'One para spans lines.' },
      { type: 'para', text: 'Next para.' },
    ]);
  });

  it('reads a heading with its depth, and an explicit {#id} as the anchor', () => {
    expect(parseBlocks('## Foo bar {#foo_bar}\n### Sub\n# Top')).toEqual([
      { type: 'heading', depth: 2, text: 'Foo bar', id: 'foo_bar' },
      { type: 'heading', depth: 3, text: 'Sub', id: null },
      { type: 'heading', depth: 1, text: 'Top', id: null },
    ]);
  });

  it('reads fenced code with its language, to the closing fence or the end', () => {
    expect(parseBlocks('```apache\nModPagespeed on;\n```')).toEqual([
      { type: 'code', lang: 'apache', text: 'ModPagespeed on;' },
    ]);
    expect(parseBlocks('```\nplain\n```')).toEqual([{ type: 'code', lang: 'text', text: 'plain' }]);
    expect(parseBlocks('~~~nginx\non;\n~~~')).toEqual([
      { type: 'code', lang: 'nginx', text: 'on;' },
    ]);
    expect(parseBlocks('```js\nnever closed')).toEqual([
      { type: 'code', lang: 'js', text: 'never closed' },
    ]);
  });

  it('gathers the consecutive pipe lines of a table into one block', () => {
    expect(parseBlocks('| a | b |\n| --- | --- |\n| 1 | 2 |\nnot a row')).toEqual([
      { type: 'table', text: '| a | b |\n| --- | --- |\n| 1 | 2 |' },
      { type: 'para', text: 'not a row' },
    ]);
  });

  it('reads list items of any marker, joining their indented continuation', () => {
    expect(parseBlocks('- one\n  wrapped\n* star\n1. numbered')).toEqual([
      { type: 'item', text: 'one wrapped' },
      { type: 'item', text: 'star' },
      { type: 'item', text: 'numbered' },
    ]);
  });

  it('keeps a star with no space after it out of the list', () => {
    expect(parseBlocks('*not a list* because no space')).toEqual([
      { type: 'para', text: '*not a list* because no space' },
    ]);
  });

  it('reads a line of anchor tags as the ids it carries', () => {
    expect(parseBlocks('<a id="x"></a><a name="y"></a>')).toEqual([
      { type: 'anchors', ids: ['x', 'y'] },
    ]);
  });

  it('keeps a ::: callout whole, its fences included', () => {
    expect(parseBlocks(':::note\ninside\n:::\nafter')).toEqual([
      { type: 'other', text: ':::note\ninside\n:::' },
      { type: 'para', text: 'after' },
    ]);
  });

  it('labels the per-server labels, and the Full guide lines by prefix', () => {
    const labels = ['**Apache:**', '**nginx:**', '**IIS:**', 'Enable:', 'Configuration for nginx:'];
    for (const line of labels) expect(parseBlocks(line)).toEqual([{ type: 'label', text: line }]);
    // The label forms match the whole line; anything longer is prose.
    expect(parseBlocks('Enable: with more text')).toEqual([
      { type: 'para', text: 'Enable: with more text' },
    ]);
    // The guide line is a prefix match, linked or plain.
    expect(parseBlocks('[Full guide →](/docs/filters/convert_to_webp/)')).toEqual([
      { type: 'guide', text: '[Full guide →](/docs/filters/convert_to_webp/)' },
    ]);
    expect(parseBlocks('Full guides: elsewhere')).toEqual([
      { type: 'guide', text: 'Full guides: elsewhere' },
    ]);
  });
});

describe('plainText and faqHtml', () => {
  it('plainText unwraps code spans, links and emphasis, and collapses whitespace', () => {
    expect(plainText('Use `rewrite_css` and [the guide](/docs/filters/) — **bold** *em*')).toBe(
      'Use rewrite_css and the guide — bold em',
    );
    expect(plainText('one_two _three_ four')).toBe('one_two three four');
    expect(plainText('  spans\n  lines  ')).toBe('spans lines');
  });

  it('plainText decodes the entities the FAQ answers use, and only those', () => {
    expect(plainText('a &mdash; b &rarr; c &amp; d')).toBe('a — b → c & d');
    expect(plainText('a &lt; b')).toBe('a &lt; b');
  });

  it('faqHtml wraps code spans in <code>, escaped, and escapes the rest', () => {
    expect(faqHtml('Add `ModPagespeedEnableFilters foo<b>` & [see](/x)')).toBe(
      'Add <code>ModPagespeedEnableFilters foo&lt;b&gt;</code> &amp; see',
    );
    expect(faqHtml('  spans\n  lines  ')).toBe('spans lines');
  });

  it('gives the FAQ answer the same text plainText does', () => {
    const md = 'Add `foo` & [see](/x) — *em* done. And `bar<b>` too.';
    // Decode in one pass so a decoded "&" is never decoded again.
    const entities: Record<string, string> = { '&amp;': '&', '&lt;': '<', '&gt;': '>' };
    const text = faqHtml(md)
      .replace(/<\/?code>/g, '')
      .replace(/&amp;|&lt;|&gt;/g, (entity) => entities[entity]);
    expect(text).toBe(plainText(md));
  });
});

describe('wordCount and sentences', () => {
  it('wordCount counts the tokens that carry a letter or a digit', () => {
    expect(wordCount('one two three')).toBe(3);
    expect(wordCount('`foo` — bar 3.5')).toBe(3);
    expect(wordCount('')).toBe(0);
  });

  it('sentences splits after . ! ? before a capital, code, link or emphasis', () => {
    expect(sentences('One two. Three four! Five? Six.')).toEqual([
      'One two.',
      'Three four!',
      'Five?',
      'Six.',
    ]);
    expect(sentences('It rewrites. `code` follows.')).toEqual(['It rewrites.', '`code` follows.']);
    expect(sentences('Stop. next one')).toEqual(['Stop. next one']);
  });

  it('sentences keeps abbreviations, decimals and URLs whole', () => {
    expect(sentences('Like e.g., `document.write` here. Next one.')).toEqual([
      'Like e.g., `document.write` here.',
      'Next one.',
    ]);
    expect(sentences('Raised to 3.5 times. Next.')).toEqual(['Raised to 3.5 times.', 'Next.']);
    expect(sentences('See https://example.com/a.b. Then this.')).toEqual([
      'See https://example.com/a.b.',
      'Then this.',
    ]);
  });

  it('sentences returns a trailing sentence with or without its full stop', () => {
    expect(sentences('No stop at the end')).toEqual(['No stop at the end']);
    expect(sentences('One. Two')).toEqual(['One.', 'Two']);
  });
});

describe('extractSection', () => {
  const names = ['x_filter', 'other_filter'];

  it('cuts on the heading id and ends at the next heading of the same depth', () => {
    const md = [
      '## A {#x_filter}',
      '',
      'Intro of a.',
      '',
      '### Sub of a',
      '',
      'In sub.',
      '',
      '## B {#other_filter}',
      '',
      'In b.',
    ].join('\n');
    expect(cut(md, 'css-filters', 'x_filter', names)).toEqual({
      page: 'css-filters',
      anchor: 'x_filter',
      shared: false,
      heading: 'A',
      lede: 'Intro of a.',
      body: [{ heading: 'Sub of a', md: 'In sub.' }],
      risks: [],
      config: [],
      words: 5,
      siblings: [],
    });
    // A deeper heading inside belongs to the section; a shallower one ends it.
    expect(cut(md, 'css-filters', 'other_filter', names)!.lede).toBe('In b.');
  });

  it('takes three sentences for the lede, after the status sentences', () => {
    const md = [
      '## A {#x_filter}',
      '',
      '**Core filter.** One. Two. Three. Four. Five.',
      '',
      '### Risks',
      '',
      'Can break.',
      '',
      '### Configuration',
      '',
      '```apache',
      'ModPagespeedEnableFilters x_filter',
      '```',
    ].join('\n');
    expect(cut(md, 'css-filters', 'x_filter', names)).toMatchObject({
      lede: 'One. Two. Three.',
      body: [{ heading: null, md: 'Four. Five.' }],
      risks: ['Can break.'],
      config: [{ lang: 'apache', code: 'ModPagespeedEnableFilters x_filter' }],
    });
  });

  it('rewrites fragment links to the group page, in prose and in tables', () => {
    const md = [
      '## A {#x_filter}',
      '',
      'One. Two. Three. See [the risks](#risks) too.',
      '',
      '| Directive | Effect |',
      '| --- | --- |',
      '| [x](#x_filter) | does it |',
    ].join('\n');
    const s = cut(md, 'css-filters', 'x_filter', names)!;
    expect(s.body[0].md).toBe('See [the risks](/docs/css-filters/#risks) too.');
    expect(s.config).toEqual([
      {
        md: '| Directive | Effect |\n| --- | --- |\n| [x](/docs/css-filters/#x_filter) | does it |',
      },
    ]);
  });

  it('returns null when no heading or anchor carries the filter name', () => {
    expect(cut('## Other {#other_filter}\n\nProse.', 'css-filters', 'x_filter', names)).toBeNull();
  });

  it('on a shared section keeps the prose that names the filter, or neither', () => {
    const shared = ['convert_a', 'convert_b'];
    const md = [
      '## Format conversion {#convert_a}',
      '',
      '`convert_a` makes PNG.',
      '',
      '<a id="convert_b"></a>',
      '',
      '`convert_b` serves WebP only.',
      '',
      'General prose for both.',
      '',
      '`convert_a` alone detail.',
      '',
      '## Next',
    ].join('\n');
    // The heading's own id makes this section convert_a's own: nothing drops.
    expect(cut(md, 'image-filters', 'convert_a', shared)).toMatchObject({
      shared: false,
      anchor: 'convert_a',
      siblings: ['convert_b'],
      words: 14,
    });
    expect(cut(md, 'image-filters', 'convert_b', shared)).toMatchObject({
      shared: true,
      anchor: 'convert_a',
      heading: 'Format conversion',
      siblings: ['convert_a'],
      lede: '`convert_b` serves WebP only.',
      body: [{ heading: null, md: 'General prose for both.' }],
      // Only the prose that names the filter counts towards the thin guard.
      words: 4,
    });
  });

  it('drops a colon that introduces nothing, keeps one that introduces a list', () => {
    const alone = [
      '## A {#x_filter}',
      '',
      'One. Two. Three. It works overall. Enable it like this:',
      '',
      '```apache',
      'ModPagespeedEnableFilters x_filter',
      '```',
    ].join('\n');
    expect(cut(alone, 'css-filters', 'x_filter', names)).toMatchObject({
      lede: 'One. Two. Three.',
      body: [{ heading: null, md: 'It works overall.' }],
    });
    const withList = [
      '## A {#x_filter}',
      '',
      'One. Two. Three. It converts:',
      '',
      '- the first thing',
      '- the second thing',
    ].join('\n');
    expect(cut(withList, 'css-filters', 'x_filter', names)).toMatchObject({
      body: [{ heading: null, md: 'It converts:\n\n- the first thing\n\n- the second thing' }],
    });
  });

  it('resets the subheading on "What it does" and drops risks-mode code and tables', () => {
    const md = [
      '## T {#x_filter}',
      '',
      'One. Two. Three.',
      '',
      '### What it does',
      '',
      'Detail one.',
      '',
      '### Risks',
      '',
      '```apache',
      'code under risks',
      '```',
      '',
      'Table too:',
      '',
      '| c | d |',
      '| --- | --- |',
    ].join('\n');
    expect(cut(md, 'css-filters', 'x_filter', names)).toMatchObject({
      body: [{ heading: null, md: 'Detail one.' }],
      risks: ['Table too:'],
      config: [],
    });
  });
});

describe('workerEquivalents', () => {
  const body = [
    '## Flags',
    '',
    'Intro before the transforms.',
    '',
    '## Transforms',
    '',
    '### Minify CSS {#minify_css}',
    '',
    'It minifies. (module equivalent: `rewrite_css`) Toggleable via --css-minify, see [the flag](#--css-minify). Irrelevant.',
    '',
    '- a list item is not a para',
    '',
    '### No id heading',
    '',
    '(module equivalent: `outline_css`) Toggleable via --x.',
    '',
    '### Inline small CSS {#inline_css}',
    '',
    '(module equivalent: `inline_css` and `rewrite_css`) Always-on.',
    '',
    '## After',
    '',
    '### Out of section {#out}',
    '',
    '(module equivalent: `strip_scripts`) Toggleable via --z.',
  ].join('\n');

  it('maps each named module filter to its transform, with the control sentences', () => {
    const eq = workerEquivalents(body);
    expect([...eq.keys()]).toEqual(['rewrite_css', 'inline_css']);
    expect(eq.get('rewrite_css')).toEqual({
      id: 'minify_css',
      title: 'Minify CSS',
      control:
        'Toggleable via --css-minify, see [the flag](/docs/worker-configuration/#--css-minify).',
    });
    expect(eq.get('inline_css')).toEqual({
      id: 'inline_css',
      title: 'Inline small CSS',
      control: 'Always-on.',
    });
  });

  it('keeps the first transform for a repeated name, and only the Transforms section', () => {
    const eq = workerEquivalents(body);
    // `rewrite_css` stays with Minify CSS, its first transform.
    expect(eq.get('rewrite_css')!.id).toBe('minify_css');
    // A heading without an id is skipped; the section ends at the next h2.
    expect(eq.get('outline_css')).toBeUndefined();
    expect(eq.get('strip_scripts')).toBeUndefined();
    const none = workerEquivalents('## Nothing here\n\n### X {#x}\n\n(module equivalent: `a`)');
    expect(none.size).toBe(0);
  });

  it('keeps a control sentence that says the transform has no flag of its own', () => {
    const md = [
      '## Transforms',
      '',
      '### Script deferral {#script-deferral}',
      '',
      'Defers safe scripts. (module equivalent: `defer_javascript`) It runs with a script profile; it has no flag of its own.',
    ].join('\n');
    const eq = workerEquivalents(md);
    expect(eq.get('defer_javascript')).toEqual({
      id: 'script-deferral',
      title: 'Script deferral',
      control: 'It runs with a script profile; it has no flag of its own.',
    });
  });
});

describe('topicTitle and topicDescription', () => {
  const H = (n: number) => 'H'.repeat(n);
  const F = (n: number) => 'F'.repeat(n);

  it('topicTitle picks the longest form that fits 60 characters', () => {
    expect(topicTitle('Minify CSS', 'rewrite_css')).toBe(
      'Minify CSS: the rewrite_css filter for Apache, nginx and IIS',
    );
    expect(topicTitle(H(30), 'f')).toBe(`${H(30)}: the f filter`);
    expect(topicTitle(H(50), 'f')).toBe(`${H(50)} (f)`);
    expect(topicTitle(H(57), 'x_filter')).toBe('The x_filter filter');
    expect(topicTitle('H', F(57))).toBe(F(57));
    // No form fits: the name alone, cut to 60.
    expect(topicTitle('H', F(70))).toBe(F(60));
  });

  it('topicDescription picks the longest form that fits 155 characters', () => {
    expect(topicDescription('It minifies CSS', 'rewrite_css')).toBe(
      'It minifies CSS. How the mod_pagespeed rewrite_css filter works, when to use it, its risks and the Apache, nginx and IIS directives.',
    );
    expect(topicDescription('y'.repeat(45), 'rewrite_css')).toBe(
      `${'y'.repeat(45)}. The rewrite_css filter: what it does, its risks and the Apache, nginx and IIS directives.`,
    );
    expect(topicDescription('y'.repeat(100), 'rewrite_css')).toBe(
      `${'y'.repeat(100)}. The rewrite_css filter: what it does and its risks.`,
    );
  });

  it('topicDescription trims the summary, without doubling the full stop', () => {
    expect(topicDescription('It works.', 'f')).toBe(
      'It works. How the mod_pagespeed f filter works, when to use it, its risks and the Apache, nginx and IIS directives.',
    );
    const angled = topicDescription('Converts <img> and <script>', 'f');
    expect(angled.startsWith('Converts img and script.')).toBe(true);
  });

  it('topicDescription falls back to an ellipsis cut at a word boundary', () => {
    expect(topicDescription('w'.repeat(200), 'f')).toHaveLength(155);
    expect(topicDescription('w'.repeat(200), 'f').endsWith('…')).toBe(true);
    // The cut drops the partial word, then adds the ellipsis.
    expect(topicDescription('x'.repeat(150) + ' tail', 'f')).toBe(`${'x'.repeat(150)}…`);
  });
});

// The Topic typedef in filter-topics.mjs has many fields; each fixture below
// carries only the ones the helper reads, so it stands in for the rest.
describe('namesGoogleProduct', () => {
  const topic = (human: string, lede: string, body: string[], note?: string) =>
    ({ human, section: { lede, body: body.map((md) => ({ heading: null, md })) }, note }) as any;

  it('sees Google products in the display name, the section prose and the note', () => {
    expect(namesGoogleProduct(topic('Insert Google Analytics', 'Adds the script.', []))).toBe(true);
    expect(namesGoogleProduct(topic('Async AdSense', '', ['plain']))).toBe(true);
    expect(namesGoogleProduct(topic('Plain', 'mentions google', []))).toBe(true);
    expect(namesGoogleProduct(topic('Plain', 'nothing', ['nothing'], 'Needs Google Fonts.'))).toBe(
      true,
    );
  });

  it('is false when none of them names one', () => {
    expect(namesGoogleProduct(topic('Plain', 'lede', ['body']))).toBe(false);
    expect(namesGoogleProduct(topic('Plain', '', [], 'no products'))).toBe(false);
  });
});

describe('topicConfig', () => {
  const enable = {
    apache: 'ModPagespeedEnableFilters foo',
    nginx: 'pagespeed_enable_filters foo;',
    iis: 'pagespeed_enable_filters foo',
  };
  const disable = {
    apache: 'ModPagespeedDisableFilters foo',
    nginx: 'pagespeed_disable_filters foo;',
    iis: 'pagespeed_disable_filters foo',
  };
  const topic = (over: Record<string, unknown>) =>
    ({ enable, disable, core: false, section: { config: [] }, ...over }) as any;

  it('spells the enable line per server, and the disable block only for a CoreFilter', () => {
    expect(topicConfig(topic({})).servers).toEqual([
      { label: 'Apache', lang: 'apache', code: 'ModPagespeedEnableFilters foo' },
      { label: 'nginx', lang: 'nginx', code: 'pagespeed_enable_filters foo;' },
      { label: 'IIS (pagespeed.config)', lang: 'text', code: 'pagespeed_enable_filters foo' },
    ]);
    expect(topicConfig(topic({})).disable).toBe('');
    expect(topicConfig(topic({ core: true })).disable).toBe(
      [
        'ModPagespeedDisableFilters foo   # Apache',
        'pagespeed_disable_filters foo;   # nginx',
        'pagespeed_disable_filters foo   # IIS',
      ].join('\n'),
    );
  });

  it('leaves a deprecated filter with no server lines and no disable block', () => {
    const off = topicConfig(topic({ enable: null, disable: null, core: true }));
    expect(off.servers).toEqual([]);
    expect(off.disable).toBe('');
  });

  it('drops a section code block that only repeats the enable line', () => {
    const config = [
      { lang: 'apache', code: 'ModPagespeedEnableFilters foo' },
      { lang: 'apache', code: '# a comment\nModPagespeedEnableFilters foo' },
      { lang: 'apache', code: 'ModPagespeedEnableFilters foo\nModPagespeedEnableFilters bar' },
      { md: 'Prose entry' },
      { lang: 'nginx', code: 'pagespeed_enable_filters foo;' },
    ];
    expect(topicConfig(topic({ section: { config } })).section).toEqual([
      { lang: 'apache', code: 'ModPagespeedEnableFilters foo\nModPagespeedEnableFilters bar' },
      { md: 'Prose entry' },
    ]);
    // With no enable line of its own, nothing is a repeat of one.
    expect(topicConfig(topic({ enable: null, section: { config } })).section).toHaveLength(
      config.length,
    );
  });
});
