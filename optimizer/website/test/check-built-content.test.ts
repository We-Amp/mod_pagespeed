// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Unit tests for the rule functions of scripts/check-built-content.mjs (the
// built-output content lint wired into the Website workflow as
// `npm run check:content`). The fixtures here are synthetic HTML snippets;
// the real build is checked by running the script over dist/, not here.

import { describe, it, expect } from 'vitest';
import { writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  decodeEntities,
  collapseWhitespace,
  tokens,
  titleInfo,
  descriptionInfo,
  isNoindex,
  isRedirectStub,
  visibleText,
  h1Count,
  documentStructureProblems,
  canonicalHrefs,
  canonicalProblem,
  lintPage,
  lintPages,
  loadAllowlist,
  loadPending,
  urlOf,
} from '../scripts/check-built-content.mjs';

describe('decodeEntities', () => {
  it('decodes the named entities titles and descriptions carry', () => {
    expect(decodeEntities('a &amp; b &#39;c&#39; &mdash; d')).toBe("a & b 'c' — d");
  });

  it('decodes decimal and hex numeric references', () => {
    expect(decodeEntities('&#8212; &#x27;')).toBe("— '");
  });

  it('leaves unknown entities alone', () => {
    expect(decodeEntities('&nosuch; 5 &amp; 6')).toBe('&nosuch; 5 & 6');
  });

  it('leaves digit-bearing named entities it does not know as raw text', () => {
    // &frac12; and &sup2; are real HTML entities with digits in their names;
    // they are not in the table, so they must pass through unchanged rather
    // than being half-matched as "&frac" + "12;".
    expect(decodeEntities('&frac12; &sup2;')).toBe('&frac12; &sup2;');
  });
});

describe('collapseWhitespace', () => {
  it('collapses runs and trims', () => {
    expect(collapseWhitespace('  a\n\t b   c ')).toBe('a b c');
  });
});

describe('titleInfo', () => {
  it('reads and decodes the document title from head', () => {
    const html =
      '<html><head><title>Fine &amp; dandy</title></head><body><svg><title>icon</title></svg></body></html>';
    const t = titleInfo(html);
    expect(t.count).toBe(1);
    expect(t.text).toBe('Fine & dandy');
  });

  it('reports a missing title', () => {
    expect(titleInfo('<html><head></head><body></body></html>').count).toBe(0);
  });

  it('reports duplicated titles', () => {
    expect(titleInfo('<head><title>a</title><title>b</title></head>').count).toBe(2);
  });
});

describe('descriptionInfo', () => {
  it('keeps a literal apostrophe inside a double-quoted value', () => {
    const html = '<meta name="description" content="Why the worker won&#39;t and can&#39;t stop">';
    // Astro leaves ' unescaped in double-quoted attributes; the value must
    // not be cut at it (this bug read "PSOL was Google's…" as 15 chars).
    const html2 = `<meta name="description" content="PSOL was Google's shared library">`;
    expect(descriptionInfo(html).text).toBe("Why the worker won't and can't stop");
    expect(descriptionInfo(html2).text).toBe("PSOL was Google's shared library");
  });

  it('accepts reversed attribute order', () => {
    const html = '<meta content="A description long enough to matter here" name="description">';
    expect(descriptionInfo(html).text).toBe('A description long enough to matter here');
  });

  it('reads a value containing markup like <style> in full', () => {
    // The /examples/ pages carry descriptions such as "Moves large inline
    // <style> blocks into external files …". A tag scan that stops at the
    // first '>' cut this value at "<style" and read no description at all.
    const html =
      '<meta name="description" content="Moves large inline <style> blocks into external files with measured savings.">';
    const info = descriptionInfo(html);
    expect(info.count).toBe(1);
    expect(info.text).toBe(
      'Moves large inline <style> blocks into external files with measured savings.',
    );
  });

  it('reports absence', () => {
    expect(descriptionInfo('<head></head>').count).toBe(0);
    expect(descriptionInfo('<head></head>').text).toBe('');
  });
});

describe('isNoindex', () => {
  it('detects noindex in any robots value order', () => {
    expect(isNoindex('<meta name="robots" content="noindex, follow">')).toBe(true);
    expect(isNoindex('<meta name="robots" content="noindex,nofollow">')).toBe(true);
    expect(isNoindex('<meta content="index, follow" name="robots">')).toBe(false);
    expect(isNoindex('<meta name="robots" content="index, follow">')).toBe(false);
    expect(isNoindex('<head></head>')).toBe(false);
  });
});

describe('isRedirectStub', () => {
  it('is a stub only when a meta refresh and noindex coincide', () => {
    const refresh = '<meta http-equiv="refresh" content="0;url=/x">';
    const noindex = '<meta name="robots" content="noindex">';
    expect(isRedirectStub(refresh + noindex)).toBe(true);
    expect(isRedirectStub(refresh)).toBe(false);
    expect(isRedirectStub(noindex)).toBe(false);
    expect(isRedirectStub('<p>a real page</p>')).toBe(false);
  });

  it('does not see a meta refresh that only appears in a script string', () => {
    const html =
      '<meta name="robots" content="noindex"><script>const h = \'http-equiv="refresh"\';</script>';
    expect(isRedirectStub(html)).toBe(false);
  });
});

describe('visible text extraction', () => {
  const page = (body: string) => `<html><head><title>t</title></head><body>${body}</body></html>`;

  it('strips script, style, code, pre and comments', () => {
    const html = page(
      '<script>var daemon = 1;</script><style>.x { content: "daemon" }</style>' +
        '<p>The worker runs.</p><code>daemon flag</code><pre>daemon config</pre><!-- daemon -->',
    );
    expect(visibleText(html)).toBe('The worker runs.');
  });

  it('decodes entities in text', () => {
    expect(visibleText(page('<p>It&#39;s the worker</p>'))).toBe("It's the worker");
  });
});

describe('h1Count', () => {
  it('counts only h1 opening tags outside script and comments', () => {
    const html =
      '<h1>One</h1><script>const s = "<h1>no</h1>";</script><!-- <h1>no</h1> --><h2>Two</h2>';
    expect(h1Count(html)).toBe(1);
  });

  it('does not match h1-prefixed tags like a hypothetical h10', () => {
    expect(h1Count('<h10>not a heading</h10>')).toBe(0);
  });

  it('counts zero and two', () => {
    expect(h1Count('<p>none</p>')).toBe(0);
    expect(h1Count('<h1>a</h1><h1>b</h1>')).toBe(2);
  });
});

describe('tokens', () => {
  it('does not end a tag at a > inside a quoted attribute value', () => {
    const tk = tokens('<img alt="a > b" width=3>');
    expect(tk).toHaveLength(1);
    expect(tk[0]).toMatchObject({ kind: 'open', name: 'img' });
  });

  it('skips the content of raw-text elements', () => {
    const tk = tokens('<script>const s = "</head>"; if (1 < 2) x();</script><p>text</p>');
    expect(tk.map((t) => `${t.kind}:${t.name}`)).toEqual([
      'open:script',
      'close:script',
      'open:p',
      'close:p',
    ]);
  });

  it('reports an unterminated raw-text element', () => {
    const tk = tokens('<style>.x { color: red;');
    expect(tk.some((t) => t.unterminated)).toBe(true);
  });

  it('collects comments and doctypes without tag names', () => {
    const tk = tokens('<!doctype html><!-- note --><p>x</p>');
    expect(tk.map((t) => `${t.kind}:${t.name || '-'}`)).toEqual([
      'doctype:-',
      'comment:-',
      'open:p',
      'close:p',
    ]);
  });
});

describe('documentStructureProblems / document-structure rule', () => {
  // A head that is complete for every other rule, so the structure rule is
  // the only thing under test.
  const goodHead =
    '<meta charset="utf-8">' +
    '<title>A title of usable length</title>' +
    '<meta name="description" content="A description that is comfortably longer than fifty characters for the lint.">' +
    '<link rel="canonical" href="https://modpagespeed.com/x/">';
  const page = (head: string, body: string, afterHtml = '') =>
    `<!doctype html><html lang="en"><head>${head}</head><body>${body}</body></html>${afterHtml}`;

  it('accepts a well-formed page', () => {
    expect(
      documentStructureProblems(page(goodHead, '<h1>H</h1><p>The worker serves.</p>')),
    ).toEqual([]);
    expect(lintPage(page(goodHead, '<h1>H</h1><p>The worker serves.</p>'))).toEqual([]);
  });

  // Regression variant A: </head> moved above the title. The title is
  // present and must be reported as misplaced, not as missing.
  it('reports a <title> below </head> as outside <head>, and still counts it', () => {
    const head = goodHead.replace('<title>A title of usable length</title>', '');
    const html = page(head, '<h1>H</h1><title>A title of usable length</title>');
    expect(titleInfo(html).count).toBe(1);
    const structure = lintPage(html).filter((f) => f.rule === 'document-structure');
    expect(structure).toHaveLength(1);
    expect(structure[0].message).toMatch(/<title> is outside <head> at offset \d+/);
  });

  // Regression variant B: </head> right after the title, so the JSON-LD
  // block ends up in the body. Everything else passes; only this rule sees it.
  it('reports a JSON-LD script below </head>', () => {
    const html = page(
      goodHead,
      '<h1>H</h1><script type="application/ld+json">{"@context":"https://schema.org"}</script>',
    );
    const structure = lintPage(html).filter((f) => f.rule === 'document-structure');
    expect(structure).toHaveLength(1);
    expect(structure[0].message).toMatch(
      /application\/ld\+json <script> outside <head> at offset \d+/,
    );
  });

  it('reports content after </html>', () => {
    const html = page(goodHead, '<h1>H</h1>', '\n<script>tail();</script>\n');
    expect(documentStructureProblems(html)).toEqual([
      expect.stringMatching(/^content after <\/html>/),
    ]);
  });

  it('reports a second <body>', () => {
    const html = `<!doctype html><html><head>${goodHead}</head><body><h1>H</h1></body><body>extra</body></html>`;
    const problems = documentStructureProblems(html).join('\n');
    expect(problems).toMatch(/2 <body> start tags \(want 1\)/);
    expect(problems).toMatch(/2 <\/body> end tags \(want 1\)/);
  });

  it('reports a stylesheet link in the body', () => {
    const html = page(goodHead, '<h1>H</h1><link rel="stylesheet" href="/late.css">');
    expect(documentStructureProblems(html)).toEqual([
      expect.stringMatching(/<link rel="stylesheet"> outside <head> at offset \d+/),
    ]);
  });

  it('reports content between </head> and <body>', () => {
    const html = `<!doctype html><html><head>${goodHead}</head><div>stray</div><body><h1>H</h1></body></html>`;
    expect(documentStructureProblems(html)).toEqual([
      expect.stringMatching(/^content between <\/head> and <body>: "<div>stray<\/div>"/),
    ]);
  });

  it('ignores an SVG <title> in the body', () => {
    const html = page(goodHead, '<h1>H</h1><svg><title>icon</title></svg>');
    expect(documentStructureProblems(html)).toEqual([]);
    expect(titleInfo(html).count).toBe(1);
  });

  it('ignores a </head> that only appears inside a script string', () => {
    const html = page(goodHead, '<h1>H</h1><script>const s = "</head>";</script>');
    expect(documentStructureProblems(html)).toEqual([]);
  });

  it('does not tokenize markup inside an attribute value', () => {
    const html = page(goodHead, '<h1>H</h1><p data-note="5 > 3 and <b>bold</b>">ok</p>');
    expect(documentStructureProblems(html)).toEqual([]);
  });

  it('reports head-only metas placed in the body', () => {
    const html = page(goodHead, '<h1>H</h1><meta name="viewport" content="width=device-width">');
    expect(documentStructureProblems(html)).toEqual([
      expect.stringMatching(/<meta name="viewport"> outside <head> at offset \d+/),
    ]);
  });
});

describe('canonicalHrefs / canonicalProblem', () => {
  it('finds canonical links regardless of attribute order', () => {
    expect(canonicalHrefs('<link rel="canonical" href="https://modpagespeed.com/a/">')).toEqual([
      'https://modpagespeed.com/a/',
    ]);
    expect(canonicalHrefs('<link href="https://modpagespeed.com/a/" rel="canonical">')).toEqual([
      'https://modpagespeed.com/a/',
    ]);
    expect(canonicalHrefs('<link rel="stylesheet" href="/x.css">')).toEqual([]);
  });

  it('accepts absolute https URLs ending in / or an extension', () => {
    expect(canonicalProblem('https://modpagespeed.com/docs/')).toBeNull();
    expect(canonicalProblem('https://modpagespeed.com/1.0/doc/system.html')).toBeNull();
  });

  it('rejects relative, off-site and bare paths', () => {
    expect(canonicalProblem('/docs/')).toMatch(/not an absolute URL/);
    expect(canonicalProblem('http://modpagespeed.com/docs/')).toMatch(/not on/);
    expect(canonicalProblem('https://example.com/docs/')).toMatch(/not on/);
    expect(canonicalProblem('https://modpagespeed.com/docs')).toMatch(/does not end/);
  });
});

describe('lintPage', () => {
  const goodHead =
    '<title>A title of usable length</title>' +
    '<meta name="description" content="A description that is comfortably longer than fifty characters for the lint.">' +
    '<link rel="canonical" href="https://modpagespeed.com/x/">';

  it('passes a well-formed indexable page', () => {
    const html = `<html><head>${goodHead}</head><body><h1>Heading</h1><p>The worker serves.</p></body></html>`;
    expect(lintPage(html)).toEqual([]);
  });

  it('flags a short title and a short description separately', () => {
    const html =
      '<head><title>Short</title>' +
      '<meta name="description" content="too short">' +
      '<link rel="canonical" href="https://modpagespeed.com/x/"></head>' +
      '<body><h1>H</h1></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).toContain('title-length');
    expect(rules).toContain('description-length');
  });

  it('skips length, description, daemon and canonical rules on noindex pages', () => {
    const html =
      '<head><title>Short</title>' +
      '<meta name="robots" content="noindex, follow">' +
      '<meta name="description" content="too short"></head>' +
      '<body><h1>H</h1><p>The daemon serves.</p></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).not.toContain('title-length');
    expect(rules).not.toContain('description-present');
    expect(rules).not.toContain('term-drift-daemon');
    expect(rules).not.toContain('canonical');
  });

  it('still demands one h1 and the product name on noindex pages', () => {
    const html =
      '<head><title>t</title><meta name="robots" content="noindex"></head>' +
      '<body><p>mod_pagespeed 2.0 is old</p></body>';
    const rules = lintPage(html).map((f) => f.rule);
    expect(rules).toContain('h1-count');
    expect(rules).toContain('product-naming');
  });

  it('skips h1-count and document-structure on redirect stubs', () => {
    // Astro's 404/500 stubs: meta refresh, noindex, no html/head/body at all.
    const stub =
      '<!doctype html><title>Redirecting to: /404</title>' +
      '<meta http-equiv="refresh" content="0;url=/404">' +
      '<meta name="robots" content="noindex">' +
      '<body><a href="/404">Redirecting</a></body>';
    expect(isRedirectStub(stub)).toBe(true);
    const rules = lintPage(stub).map((f) => f.rule);
    expect(rules).not.toContain('h1-count');
    expect(rules).not.toContain('document-structure');
  });

  it('flags daemon in visible text but not in code samples', () => {
    const visible = `<html><head>${goodHead}</head><body><h1>H</h1><p>Restart the daemon.</p></body></html>`;
    const coded = `<html><head>${goodHead}</head><body><h1>H</h1><code>systemctl restart daemon</code></body></html>`;
    expect(lintPage(visible).map((f) => f.rule)).toContain('term-drift-daemon');
    expect(lintPage(coded).map((f) => f.rule)).not.toContain('term-drift-daemon');
  });

  it('matches daemons (plural) too', () => {
    const html = `<html><head>${goodHead}</head><body><h1>H</h1><p>Two daemons were considered.</p></body></html>`;
    expect(lintPage(html).map((f) => f.rule)).toContain('term-drift-daemon');
  });

  it('flags daemon in the title, description and og surfaces a search result shows', () => {
    const og = `<html><head>${goodHead}<meta property="og:description" content="Restart the daemon for new settings."></head><body><h1>H</h1></body></html>`;
    const titled = `<html><head>${goodHead.replace('A title of usable length', 'The daemon settings explained well')}</head><body><h1>H</h1></body></html>`;
    const ogHit = lintPage(og).find((f) => f.rule === 'term-drift-daemon');
    expect(ogHit?.message).toMatch(/^og:description: /);
    expect(
      lintPage(titled).some(
        (f) => f.rule === 'term-drift-daemon' && f.message.startsWith('<title>: '),
      ),
    ).toBe(true);
  });

  it('flags never-valid product names in text, title and description', () => {
    const page = (body: string, title = 'A title of usable length', description?: string) =>
      `<html><head><title>${title}</title>${
        description ? `<meta name="description" content="${description}">` : ''
      }<link rel="canonical" href="https://modpagespeed.com/x/"></head><body><h1>H</h1><p>${body}</p></body></html>`;

    // Never valid, in any case, with underscore or space.
    expect(lintPage(page('Runs mod_pagespeed 2.0 today')).map((f) => f.rule)).toContain(
      'product-naming',
    );
    expect(lintPage(page('Runs MOD_PAGESPEED 2.0 today')).map((f) => f.rule)).toContain(
      'product-naming',
    );
    expect(lintPage(page('Runs mod pagespeed 2.0 today')).map((f) => f.rule)).toContain(
      'product-naming',
    );
    // The current line under its old CamelCase name.
    expect(lintPage(page('Runs ModPageSpeed 2.1 today')).map((f) => f.rule)).toContain(
      'product-naming',
    );
    // In the title and description, not only in body text.
    expect(
      lintPage(page('All fine', 'Upgrading from mod_pagespeed 2.0 explained')).map((f) => f.rule),
    ).toContain('product-naming');
    expect(
      lintPage(
        page('All fine', undefined, 'How mod_pagespeed 2.0 installs differ from this release here'),
      ).map((f) => f.rule),
    ).toContain('product-naming');

    // The predecessor line under its real CamelCase name is history, and
    // the current product name is fine. (The retired "2.1" label is a
    // separate rule, below; it is not this one.)
    expect(
      lintPage(page('Migrating from ModPageSpeed 2.0 today')).map((f) => f.rule),
    ).not.toContain('product-naming');
    expect(lintPage(page('Runs mod_pagespeed today')).map((f) => f.rule)).not.toContain(
      'product-naming',
    );
    expect(lintPage(page('Runs mod_pagespeed 2.1 today')).map((f) => f.rule)).not.toContain(
      'product-naming',
    );
    expect(lintPage(page('Runs mod_pagespeed 1.15 today')).map((f) => f.rule)).not.toContain(
      'product-naming',
    );

    // The message quotes the name that actually matched.
    const hit = lintPage(page('Runs mod pagespeed 2.0 today')).find(
      (f) => f.rule === 'product-naming',
    );
    expect(hit?.message).toMatch(/"mod pagespeed 2\.0" never named a product line/);
  });

  it('flags the retired "2.1" product label, and only as a label', () => {
    const page = (body: string, title = 'A title of usable length', description?: string) =>
      `<html><head><title>${title}</title>${
        description ? `<meta name="description" content="${description}">` : ''
      }<link rel="canonical" href="https://modpagespeed.com/x/"></head><body><h1>H</h1><p>${body}</p></body></html>`;
    const rules = (html: string) => lintPage(html).map((f) => f.rule);

    // The label, in any case, with underscore or space, at the end of a
    // sentence too.
    for (const body of [
      'Runs mod_pagespeed 2.1 today',
      'Runs MOD_PAGESPEED 2.1 today',
      'Runs mod pagespeed 2.1 today',
      'Install mod_pagespeed 2.1.',
      'The mod_pagespeed 2.1 worker',
    ]) {
      expect(rules(page(body)), body).toContain('retired-product-label');
    }
    // In the title and description, not only in body text.
    expect(rules(page('All fine', 'Security hardening guide — mod_pagespeed 2.1'))).toContain(
      'retired-product-label',
    );
    expect(
      rules(page('All fine', undefined, 'Install mod_pagespeed 2.1 on Apache and nginx from apt')),
    ).toContain('retired-product-label');

    // Allowed: the plain name, the major line, exact versions, version-scoped
    // phrases that do not follow the name, and code.
    for (const body of [
      'Runs mod_pagespeed today',
      'mod_pagespeed 2 is a drop-in for 1.15 configurations',
      'mod_pagespeed 2.1.0 converged the two lines',
      'mod_pagespeed 2.5.0 is the current release',
      'Since 2.1.0 the worker drops privileges',
      'Volumes created before 2.1 are migrated',
      'See <a href="/docs/release-notes-2-1/">the 2.1.0 notes</a>',
      'Set <code>Version: mod_pagespeed 2.1</code> in old configs',
    ]) {
      expect(rules(page(body)), body).not.toContain('retired-product-label');
    }

    // The message names the match and the replacement.
    const hit = lintPage(page('Runs mod_pagespeed 2.1 today')).find(
      (f) => f.rule === 'retired-product-label',
    );
    expect(hit?.message).toMatch(/retired "mod_pagespeed 2\.1" label/);

    // Every use counts, on every surface: social metas, alt, aria-label and
    // the JSON-LD strings, besides the text, title and description.
    const head = (extra: string) =>
      `<html><head><title>A title of usable length</title>${extra}<link rel="canonical" href="https://modpagespeed.com/x/"></head><body><h1>H</h1><p>All fine</p></body></html>`;
    const count = (html: string) =>
      lintPage(html).find((f) => f.rule === 'retired-product-label')?.count ?? 0;
    expect(count(head('<meta property="og:title" content="Install mod_pagespeed 2.1">'))).toBe(1);
    expect(count(head('<meta name="twitter:description" content="mod_pagespeed 2.1 rocks">'))).toBe(
      1,
    );
    expect(
      count(head('<script type="application/ld+json">{"name":"mod_pagespeed 2.1"}</script>')),
    ).toBe(1);
    expect(
      count(
        head('<script type="application/ld+json">{"url":"https://x/release-notes-2-1/"}</script>'),
      ),
    ).toBe(0);
    expect(
      count(
        `<html><head><title>A title of usable length</title></head><body><h1>H</h1><img alt="mod_pagespeed 2.1 logo" src="/x.png"><a aria-label="mod_pagespeed 2.1 home" href="/">x</a><p>mod_pagespeed 2.1 and mod_pagespeed 2.1</p></body></html>`,
      ),
    ).toBe(4);
  });

  it('flags more than one description meta under description-present', () => {
    const html =
      `<html><head>${goodHead}` +
      '<meta name="description" content="A second description that is also long enough to matter here">' +
      '</head><body><h1>H</h1></body></html>';
    const hit = lintPage(html).find((f) => f.rule === 'description-present');
    expect(hit?.message).toBe('2 <meta name="description"> elements');
  });

  it('flags missing, multiple and malformed canonicals', () => {
    const none = `<head>${goodHead.replace(/<link[^>]*>/, '')}</head>`;
    const two = `<head>${goodHead}<link rel="canonical" href="https://modpagespeed.com/y/"></head>`;
    const relative = `<head>${goodHead.replace('https://modpagespeed.com/x/', '/x/')}</head>`;
    for (const head of [none, two, relative]) {
      const rules = lintPage(`${head}<body><h1>H</h1></body>`).map((f) => f.rule);
      expect(rules).toContain('canonical');
    }
  });
});

describe('lintPages', () => {
  const page = (url: string, description: string) => ({
    url,
    html:
      '<html><head><title>A title of usable length</title>' +
      `<meta name="description" content="${description}">` +
      '<link rel="canonical" href="https://modpagespeed.com/x/"></head>' +
      '<body><h1>H</h1></body></html>',
  });

  it('groups identical descriptions on indexable pages', () => {
    const { failures } = lintPages([
      page('/a/', 'The very same description repeated on two pages exactly'),
      page('/b/', 'The very same description repeated on two pages exactly'),
      page('/c/', 'A different description that is long enough to be valid'),
    ]);
    const dup = failures.find((f) => f.rule === 'description-duplicate');
    expect(dup?.message).toMatch(/\/a\//);
    expect(dup?.message).toMatch(/\/b\//);
  });

  it('exempts only the named rule on an allowlisted page', () => {
    // 'short' fails description-length and nothing else.
    const exempt = lintPages([page('/a/', 'short')], {
      '/a/': { 'description-length': 'generator gap, documented' },
    });
    expect(exempt.failures.filter((f) => f.url === '/a/')).toEqual([]);
    expect(
      exempt.warnings.some(
        (w) =>
          w.url === '/a/' &&
          w.rule === 'allowlist' &&
          /exempted description-length: generator gap/.test(w.message),
      ),
    ).toBe(true);

    // An entry naming a rule that passes drops nothing: the real failure
    // still fails the build.
    const wrongRule = lintPages([page('/a/', 'short')], { '/a/': { canonical: 'reason' } });
    expect(wrongRule.failures.some((f) => f.url === '/a/' && f.rule === 'description-length')).toBe(
      true,
    );
  });

  it('errors on allowlist entries that match no built page', () => {
    const { failures } = lintPages(
      [page('/a/', 'A different description that is long enough to be valid')],
      {
        '/gone/': { canonical: 'stale entry' },
      },
    );
    const stale = failures.find((f) => f.rule === 'allowlist');
    expect(stale?.url).toBe('/gone/');
    expect(stale?.message).toMatch(/matches no built page/);
  });

  it('routes the never-valid product name to an error-level failure', () => {
    const naming = {
      url: '/x/',
      html:
        '<html><head><title>A title of usable length</title>' +
        '<meta name="robots" content="noindex"></head>' +
        '<body><h1>H</h1><p>mod_pagespeed 2.0 mentioned</p></body></html>',
    };
    const { failures, warnings } = lintPages([naming]);
    const hit = failures.find((f) => f.rule === 'product-naming');
    expect(hit?.level).toBe('error');
    expect(hit?.message).toMatch(/mod_pagespeed 2\.0/);
    expect(warnings).toEqual([]);
  });
});

describe('retired-label pending list (the rename ratchet)', () => {
  const labelled = (url: string, text = 'mod_pagespeed 2.1 runs here') => ({
    url,
    html:
      '<html><head><title>A title of usable length</title>' +
      '<meta name="description" content="A description that is long enough to be a valid one here">' +
      '<link rel="canonical" href="https://modpagespeed.com/x/"></head>' +
      `<body><h1>H</h1><p>${text}</p></body></html>`,
  });

  it('fails a page that is not pending', () => {
    const { failures } = lintPages([labelled('/a/')]);
    const hit = failures.find((f) => f.rule === 'retired-product-label');
    expect(hit?.url).toBe('/a/');
    expect(hit?.level).toBe('error');
  });

  it('reports a pending page as a warning', () => {
    const { failures, warnings } = lintPages([labelled('/a/')], {}, { '/a/': 1 });
    expect(failures).toEqual([]);
    expect(
      warnings.some(
        (w) => w.url === '/a/' && w.rule === 'retired-product-label' && /^pending/.test(w.message),
      ),
    ).toBe(true);
  });

  it('fails a pending entry whose page no longer carries the label', () => {
    const { failures } = lintPages([labelled('/a/', 'mod_pagespeed runs here')], {}, { '/a/': 1 });
    const stale = failures.find((f) => f.rule === 'retired-product-label');
    expect(stale?.url).toBe('/a/');
    expect(stale?.message).toMatch(/no longer carries the retired label/);
  });

  it('fails a pending entry that matches no built page', () => {
    const { failures } = lintPages([labelled('/a/')], {}, { '/a/': 1, '/gone/': 1 });
    const stale = failures.find((f) => f.url === '/gone/');
    expect(stale?.rule).toBe('retired-product-label');
    expect(stale?.message).toMatch(/matches no built page/);
  });

  it('fails a pending page whose count went up (a new use), however it got there', () => {
    const twice = labelled('/a/', 'mod_pagespeed 2.1 runs here, and mod_pagespeed 2.1 again');
    const { failures } = lintPages([twice], {}, { '/a/': 1 });
    const hit = failures.find((f) => f.rule === 'retired-product-label');
    expect(hit?.message).toMatch(/^2 uses, 1 pending: new text/);
  });

  it('fails a pending page whose count went down until the entry is lowered', () => {
    const { failures } = lintPages([labelled('/a/')], {}, { '/a/': 2 });
    const hit = failures.find((f) => f.rule === 'retired-product-label');
    expect(hit?.message).toMatch(/lower its count .* to 1/);
  });

  it('the shipped pending list loads as URL paths with positive counts', () => {
    const pending = loadPending(join(__dirname, '../scripts/retired-label-pending.json'));
    for (const [url, n] of Object.entries(pending)) {
      expect(url.startsWith('/') && url.endsWith('/'), url).toBe(true);
      expect(Number.isInteger(n) && n > 0, url).toBe(true);
    }
  });
});

describe('urlOf', () => {
  it('maps dist files to site URLs', () => {
    expect(urlOf('/dist', '/dist/client/index.html')).toBe('/');
    expect(urlOf('/dist', '/dist/client/docs/a/index.html')).toBe('/docs/a/');
    expect(urlOf('/dist', '/dist/client/404.html')).toBe('/404.html');
    expect(urlOf('/dist', '/dist/client/docs/deep/nested/index.html')).toBe('/docs/deep/nested/');
  });
});

describe('loadAllowlist', () => {
  const tmpFile = () => join(tmpdir(), `content-lint-allow-${process.pid}-${Date.now()}.json`);

  it('rejects an entry naming an unknown rule', () => {
    const file = tmpFile();
    writeFileSync(file, JSON.stringify({ '/x/': { 'no-such-rule': 'reason' } }));
    try {
      expect(() => loadAllowlist(file)).toThrow(/unknown rule "no-such-rule"/);
    } finally {
      rmSync(file);
    }
  });

  it('rejects the old whole-page format and empty reasons', () => {
    const file = tmpFile();
    writeFileSync(file, JSON.stringify({ '/x/': 'a reason' }));
    try {
      expect(() => loadAllowlist(file)).toThrow(/expected \{rule: reason\}/);
    } finally {
      rmSync(file);
    }

    const file2 = tmpFile();
    writeFileSync(file2, JSON.stringify({ '/x/': { canonical: '   ' } }));
    try {
      expect(() => loadAllowlist(file2)).toThrow(/reason must be a non-empty string/);
    } finally {
      rmSync(file2);
    }
  });

  it('returns an empty allowlist when the file is absent', () => {
    expect(loadAllowlist(join(tmpdir(), 'content-lint-allow-absent.json'))).toEqual({});
  });
});
