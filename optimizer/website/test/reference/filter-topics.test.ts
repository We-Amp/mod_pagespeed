// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filter-topics.test.ts — the per-filter topic pages (/docs/filters/<name>/).
//
// Source checks (always run):
//   - every name in filters.json has a topic, with a section on its group page,
//     a display name, a title of at most 60 and a description of at most 155
//     characters, and a FAQ of two or three questions;
//   - every group page links each filter's section to its topic page;
//   - llms.txt lists exactly the indexable topic pages.
// Build-output checks (skip themselves without dist/, like anchor-gate):
//   - one page per filter, exactly one <h1>, the title and description the
//     data computed, the visible FAQ equal to the FAQPage JSON-LD;
//   - noindex on exactly the thin pages, and those absent from the sitemap
//     while every indexable one is in it;
//   - the /docs/filters/ table and every /examples/<slug>/ page link to the
//     topic pages.

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { parse } from 'parse5';
import { describe, it, expect } from 'vitest';
import {
  loadFilterTopics,
  plainText,
  THIN_WORDS,
  HUMAN_NAMES,
  PAGE_BY_CATEGORY,
  RELATED,
  topicFacts,
  GOOGLE_PRODUCTS_LINE,
  relatedTopics,
  topicHref,
  renderTopicMarkdown,
  renderFiltersHubMarkdown,
} from '../../src/lib/filter-topics.mjs';
import { EXAMPLES } from '../../src/data/examples';
import { alsoAppliedIn } from '../../src/lib/filter-demos';
import { topicForExample } from '../../src/data/filters';
import { ORGANIZATION_ID } from '../../src/data/organization';

type HtmlNode = {
  nodeName: string;
  value?: string;
  attrs?: { name: string; value: string }[];
  childNodes?: HtmlNode[];
};

// The JSON-LD blocks of a built page, read with an HTML parser.
function jsonLdBlocks(html: string): any[] {
  const out: any[] = [];
  const walk = (node: HtmlNode) => {
    for (const child of node.childNodes ?? []) {
      if (
        child.nodeName === 'script' &&
        child.attrs?.some((a) => a.name === 'type' && a.value === 'application/ld+json')
      ) {
        out.push(JSON.parse((child.childNodes ?? []).map((c) => c.value ?? '').join('')));
      }
      walk(child);
    }
  };
  walk(parse(html) as HtmlNode);
  return out.flat();
}

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const data = JSON.parse(
  readFileSync(resolve(WEBSITE_ROOT, 'src/data/reference/filters.json'), 'utf8'),
);
const allNames: string[] = [
  ...data.filters.map((f: { name: string }) => f.name),
  ...data.aliases.map((a: { name: string }) => a.name),
];
const topics = loadFilterTopics(WEBSITE_ROOT);
const byName = new Map(topics.map((t) => [t.name, t]));
const demosOf = (t: (typeof topics)[number]) => alsoAppliedIn(t.name, t.example);

describe('filter topics (source)', () => {
  it('cover every filter and compound name in filters.json, once', () => {
    expect(topics.map((t) => t.name).sort()).toEqual([...allNames].sort());
    expect(Object.keys(HUMAN_NAMES).sort()).toEqual([...allNames].sort());
  });

  for (const t of topics) {
    it(`${t.name}: title, description, FAQ and guard`, () => {
      expect(t.path).toBe(`/docs/filters/${t.name}/`);
      expect(t.title.length).toBeLessThanOrEqual(60);
      expect(t.title).toContain(t.name);
      expect(t.description.length).toBeLessThanOrEqual(155);
      expect(t.description.length).toBeGreaterThan(20);
      expect(t.faq.length).toBeGreaterThanOrEqual(2);
      expect(t.faq.length).toBeLessThanOrEqual(3);
      expect(t.section.lede.length).toBeGreaterThan(0);
      expect(t.indexable).toBe(!t.alternateSpellingOf && t.section.words >= THIN_WORDS);
    });
  }

  it('an alternate spelling names its indexable primary as canonical; every other page itself', () => {
    const alternates = topics.filter((t) => t.alternateSpellingOf);
    expect(alternates.length).toBeGreaterThan(0);
    for (const t of topics) {
      const primary = t.alternateSpellingOf ? byName.get(t.alternateSpellingOf) : undefined;
      expect(t.canonicalPath, t.name).toBe(primary?.indexable ? primary.path : t.path);
      // A canonical never points at a page that is itself kept out of search.
      if (t.canonicalPath !== t.path) expect(byName.get(primary!.name)!.indexable).toBe(true);
    }
  });

  it('use one consistent title form per length tier', () => {
    for (const t of topics) {
      const full = `${t.human}: the ${t.name} filter for Apache, nginx and IIS`;
      if (full.length <= 60) expect(t.title).toBe(full);
    }
  });

  it('curated related filters name real filters', () => {
    for (const [name, related] of Object.entries(RELATED)) {
      expect(byName.has(name), name).toBe(true);
      for (const r of related) expect(byName.has(r), `${name} -> ${r}`).toBe(true);
    }
  });

  it('shared sections count only the prose that names the filter', () => {
    for (const t of topics.filter((x) => x.section.shared)) {
      const peers = topics.filter(
        (x) => x.section.shared && x.section.anchor === t.section.anchor && x.name !== t.name,
      );
      for (const p of peers) {
        if (t.indexable && p.indexable) expect(t.section.lede).not.toBe(p.section.lede);
      }
    }
  });

  it('every group page links each filter section to its topic page', () => {
    const pages = new Set(Object.values(PAGE_BY_CATEGORY));
    const text = [...pages]
      .map((slug) => readFileSync(resolve(WEBSITE_ROOT, `src/content/docs/${slug}.md`), 'utf8'))
      .join('\n');
    for (const t of topics) {
      if (t.alternateSpellingOf) continue;
      expect(text, `link to ${t.path}`).toContain(`](${t.path})`);
    }
  });

  it('llms.txt lists exactly the indexable topic pages', () => {
    const llms = readFileSync(resolve(WEBSITE_ROOT, 'public/llms.txt'), 'utf8');
    const listed = [...llms.matchAll(/\(https:\/\/modpagespeed\.com\/docs\/filters\/([^/]+)\/\)/g)]
      .map((m) => m[1])
      .sort();
    expect(listed).toEqual(
      topics
        .filter((t) => t.indexable)
        .map((t) => t.name)
        .sort(),
    );
  });
});

const DIST = resolve(WEBSITE_ROOT, 'dist/client');
const BUILT = existsSync(resolve(DIST, 'docs/filters'));
if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    '[filter-topics] built output not found — skipping the built-HTML checks. ' +
      'Run "npm run build" first to exercise them.',
  );
}

function attr(html: string, re: RegExp): string | null {
  const m = re.exec(html);
  return m ? m[1] : null;
}

function decode(s: string): string {
  return s
    .replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&#x27;/g, "'")
    .replace(/&amp;/g, '&');
}

function textOf(html: string): string {
  return decode(html.replace(/<[^>]+>/g, ''))
    .replace(/\s+/g, ' ')
    .trim();
}

describe.skipIf(!BUILT)('filter topics (built html)', () => {
  if (!BUILT) return;
  const sitemap = readdirSync(DIST)
    .filter((f) => /^sitemap-\d+\.xml$/.test(f))
    .map((f) => readFileSync(resolve(DIST, f), 'utf8'))
    .join('\n');

  it('has a page for every filter and nothing else under /docs/filters/', () => {
    const dirs = readdirSync(resolve(DIST, 'docs/filters'), { withFileTypes: true })
      .filter((d) => d.isDirectory())
      .map((d) => d.name)
      .sort();
    expect(dirs).toEqual([...allNames].sort());
  });

  for (const t of topics) {
    describe(t.path, () => {
      const html = readFileSync(resolve(DIST, `docs/filters/${t.name}/index.html`), 'utf8');

      it('has exactly one h1, the data title and description', () => {
        expect(html.match(/<h1[\s>]/g)?.length).toBe(1);
        expect(decode(attr(html, /<title>([^<]*)<\/title>/) ?? '')).toBe(t.title);
        expect(decode(attr(html, /<meta name="description" content="([^"]*)"/) ?? '')).toBe(
          t.description,
        );
      });

      it('shows the FAQ its FAQPage JSON-LD carries', () => {
        const blocks = [
          ...html.matchAll(/<script type="application\/ld\+json"[^>]*>([\s\S]*?)<\/script>/g),
        ].map((m) => JSON.parse(m[1]));
        const faqLd = blocks.flat().find((b) => b['@type'] === 'FAQPage');
        expect(faqLd).toBeTruthy();
        const ld = faqLd.mainEntity.map((q: any) => ({ q: q.name, a: q.acceptedAnswer.text }));
        const dl = /<dl[^>]*data-topic-faq[^>]*>([\s\S]*?)<\/dl>/.exec(html)?.[1] ?? '';
        const visible = [
          ...dl.matchAll(/<dt[^>]*>([\s\S]*?)<\/dt>\s*<dd[^>]*>([\s\S]*?)<\/dd>/g),
        ].map((m) => ({ q: textOf(m[1]), a: textOf(m[2]) }));
        expect(visible).toEqual(ld);
        expect(ld).toEqual(t.faq.map((f) => ({ q: f.q, a: plainText(f.a) })));
      });

      const canonicalElsewhere = t.canonicalPath !== t.path;
      it(
        t.indexable
          ? 'is indexable and in the sitemap'
          : canonicalElsewhere
            ? 'names its primary spelling as canonical, without noindex, and is not in the sitemap'
            : 'is noindex and not in the sitemap',
        () => {
          const noindex = /<meta name="robots" content="noindex,follow"/.test(html);
          expect(noindex).toBe(!t.indexable && !canonicalElsewhere);
          expect(sitemap.includes(`https://modpagespeed.com${t.path}<`)).toBe(t.indexable);
          expect(attr(html, /<link rel="canonical" href="([^"]*)"/)).toBe(
            `https://modpagespeed.com${t.canonicalPath}`,
          );
        },
      );

      it('names the site-wide organization as author and publisher', () => {
        const article = jsonLdBlocks(html).find((b) => b['@type'] === 'TechArticle');
        expect(article.author['@id']).toBe(ORGANIZATION_ID);
        expect(article.publisher['@id']).toBe(ORGANIZATION_ID);
        expect(article.publisher.logo?.url).toBeTruthy();
      });
    });
  }

  it('the /docs/filters/ table keeps the section link on rows whose topic page is noindex', () => {
    const html = readFileSync(resolve(DIST, 'docs/filters/index.html'), 'utf8');
    for (const t of topics) {
      const row = new RegExp(`<tr[^>]*id="${t.name}"[^>]*>([\\s\\S]*?)</tr>`).exec(html)?.[1] ?? '';
      expect(row.includes(`href="${t.groupPath}"`), t.name).toBe(!t.indexable);
    }
  });

  it('every topic page is in site search, indexable or not', () => {
    for (const t of topics) {
      const html = readFileSync(resolve(DIST, `docs/filters/${t.name}/index.html`), 'utf8');
      expect(html, t.name).toMatch(/<main[^>]*data-pagefind-body/);
    }
  });

  it('the /docs/filters/ table links every row to its topic page', () => {
    const html = readFileSync(resolve(DIST, 'docs/filters/index.html'), 'utf8');
    for (const name of allNames) {
      expect(html, name).toMatch(
        new RegExp(`<tr[^>]*id="${name}"[^>]*>[\\s\\S]*?href="/docs/filters/${name}/"`),
      );
    }
  });

  it('every example page that isolates a filter links to its full guide', () => {
    for (const ex of EXAMPLES) {
      const html = readFileSync(resolve(DIST, `examples/${ex.slug}/index.html`), 'utf8');
      const href = attr(html, /<a[^>]*href="([^"]+)"[^>]*data-full-guide/);
      const guide = topicForExample(ex.slug, ex.filters);
      if (!guide) {
        expect(href, ex.slug).toBeNull();
        continue;
      }
      expect(href, ex.slug).toBe(guide.topic);
      expect(href, ex.slug).toMatch(/^\/docs\/filters\/[a-z0-9_]+\/$/);
      expect(byName.has(href!.split('/')[3]), ex.slug).toBe(true);
    }
  });
});

describe('filter topic Markdown twins (source)', () => {
  const md = (name: string) => {
    const t = byName.get(name)!;
    return renderTopicMarkdown(t, topics, demosOf(t));
  };

  it('follows the page order', () => {
    const body = md('rewrite_images');
    const order = [
      '# Optimize images (rewrite_images)\n',
      '\nCanonical URL: https://modpagespeed.com/docs/filters/rewrite_images/\n',
      '\n## What it switches on\n',
      '\n## How it works\n',
      '\n## When to use it\n',
      '\n## Risks\n',
      '\n## Configuration\n',
      '\n## Live example\n',
      '\n## Frequently asked questions\n',
      '\n## Related filters\n',
    ];
    const at = order.map((h) => body.indexOf(h));
    expect(at.every((n) => n >= 0)).toBe(true);
    expect(at).toEqual([...at].sort((a, b) => a - b));
  });

  it('leaves out the chrome, the call to action and the impact tiles', () => {
    expect(md('rewrite_images')).not.toMatch(
      /Download (&|&amp;) run|Breadcrumb|Measured on the demo/,
    );
  });

  it('shares its facts and its related list with the page', () => {
    for (const t of topics.filter((x) => x.indexable)) {
      const body = renderTopicMarkdown(t, topics, demosOf(t));
      for (const fact of topicFacts(t)) {
        const absolute = fact.replace(/\]\((\/[^)\s]*)\)/g, '](https://modpagespeed.com$1)');
        expect(body, t.name).toContain(`- ${absolute}`);
      }
      const related = relatedTopics(t, topics);
      expect(related.length, t.name).toBeLessThanOrEqual(8);
      expect(
        related.map((r) => r.name),
        t.name,
      ).not.toContain(t.name);
      for (const r of related) {
        expect(body, t.name).toContain(`(https://modpagespeed.com${topicHref(r)})`);
      }
    }
  });

  it('links a related filter to a page search engines index', () => {
    let toGroup = 0;
    for (const t of topics) {
      for (const r of relatedTopics(t, topics)) {
        const href = topicHref(r);
        if (r.indexable) expect(href, `${t.name} -> ${r.name}`).toBe(r.path);
        else {
          expect(href, `${t.name} -> ${r.name}`).toBe(r.groupPath);
          expect(href, `${t.name} -> ${r.name}`).toMatch(/^\/docs\/[^/]+\/#/);
          toGroup++;
        }
      }
      if (!t.indexable) continue;
      const related = renderTopicMarkdown(t, topics, demosOf(t)).match(
        /\n## Related filters\n([\s\S]*?)\nThis page is drawn/,
      )?.[1];
      for (const r of relatedTopics(t, topics).filter((x) => !x.indexable)) {
        expect(related, `${t.name} twin -> ${r.name}`).not.toContain(
          `(https://modpagespeed.com${r.path})`,
        );
      }
    }
    // Some related filters are thin today; the check must exercise that path.
    expect(toGroup).toBeGreaterThan(0);
  });

  it('lists the same gallery demos as the topic page', () => {
    // The example links in the twin's live example section, in order: the demo
    // that isolates the filter (if any), then the others that apply it.
    const linked = (body: string) =>
      [
        ...(body.match(/\n## Live examples?\n[\s\S]*?(?=\n## )/)?.[0] ?? '').matchAll(
          /\]\(https:\/\/modpagespeed\.com\/examples\/([^/)]+)\/\)/g,
        ),
      ].map((m) => m[1]);
    let withList = 0;
    for (const t of topics.filter((x) => x.indexable)) {
      const demos = demosOf(t);
      if (demos.length) withList++;
      const isolating = t.example ? [t.example.split('/')[2]] : [];
      expect(linked(renderTopicMarkdown(t, topics, demos)), t.name).toEqual([
        ...isolating,
        ...demos.map((d) => d.slug),
      ]);
    }
    expect(withList).toBeGreaterThan(0);
  });

  it('refuses to render without the demo list', () => {
    const t = byName.get('rewrite_images')!;
    // @ts-expect-error the list is required
    expect(() => renderTopicMarkdown(t, topics)).toThrow(/no demo list/);
  });

  it('every live example link points at a gallery page', () => {
    const slugs = new Set(EXAMPLES.map((e) => e.slug));
    for (const t of topics.filter((x) => x.indexable && x.example)) {
      expect(slugs.has(t.example!.split('/')[2]), t.name).toBe(true);
    }
  });

  it('the hub links indexable topics to their page and thin ones to the group page', () => {
    const hub = renderFiltersHubMarkdown(topics);
    for (const t of topics) {
      const href = t.indexable ? t.path : t.groupPath;
      expect(hub, t.name).toContain(`[\`${t.name}\`](https://modpagespeed.com${href})`);
    }
  });

  it('escapes backslashes before pipes in hub table cells', () => {
    const t = { ...byName.get('rewrite_images')!, summary: 'a\\|b | c\\' };
    const hub = renderFiltersHubMarkdown([t]);
    expect(hub).toContain('a\\\\\\|b \\| c\\\\ |');
  });

  it('absolutizes relative links in FAQ answers', () => {
    const t = {
      ...byName.get('rewrite_images')!,
      faq: [{ q: 'Where next?', a: 'See [the docs](/docs/filters/) and [below](#risks).' }],
    };
    const body = renderTopicMarkdown(t, topics, demosOf(t));
    expect(body).toContain('See [the docs](https://modpagespeed.com/docs/filters/) and [below](');
    expect(body).not.toMatch(/\]\((\/|#)/);
  });

  it('the topic page spells the same non-affiliation sentence as the twin', () => {
    const page = readFileSync(resolve(WEBSITE_ROOT, 'src/pages/docs/filters/[name].astro'), 'utf8');
    expect(page.replace(/\s+/g, ' ')).toContain(GOOGLE_PRODUCTS_LINE);
    const t = byName.get('insert_ga')!;
    expect(renderTopicMarkdown(t, topics, demosOf(t))).toContain(`\n${GOOGLE_PRODUCTS_LINE}\n`);
  });
});
