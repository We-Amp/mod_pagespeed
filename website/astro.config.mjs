// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// @ts-check
import { defineConfig } from 'astro/config';
import tailwindcss from '@tailwindcss/vite';
import sitemap from '@astrojs/sitemap';
import mdx from '@astrojs/mdx';
import node from '@astrojs/node';
import { unified, rehypeHeadingIds } from '@astrojs/markdown-remark';
import remarkCustomHeadingId from 'remark-custom-heading-id';
import remarkDirective from 'remark-directive';
import remarkCallouts from './src/lib/remark-callouts.mjs';
import rehypeDirectiveBlocks from './src/lib/rehype-directive-blocks.mjs';
import rehypeHeadingAnchors from './src/lib/rehype-heading-anchors.mjs';
import remarkLiteralTextDirectives from './src/lib/remark-literal-text-directives.mjs';
import { gitFileHistory, gitLastModified } from './src/lib/git-date';
import { EXAMPLES } from './src/data/examples';
import { exampleImpact } from './src/lib/example-impact';
import {
  exampleLastmods,
  routeFamilyLastmods,
  staticPageLastmods,
} from './src/lib/page-lastmod.mjs';
import { BUILD_ID } from './src/lib/build-id.mjs';
import { PAGEFIND_BUNDLE_PATH } from './src/lib/pagefind-bundle.mjs';
import { loadFilterTopics } from './src/lib/filter-topics.mjs';
import { STATIC_INDEXABLE_PAGES } from './src/lib/static-html-pages.mjs';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const publicDir = resolve(__dirname, 'public');

/**
 * Build a `URL path -> ISO date` map from content-collection frontmatter so the
 * sitemap can emit a real per-entry `lastmod`. A build-time lastmod on every URL
 * trains crawlers to ignore the freshness signal; a per-entry date from
 * `lastUpdated`/`date`/`datePublished` is a true signal. Defensive throughout:
 * any failure returns null, and the serializer then omits lastmod for that URL.
 * @param {string} filePath
 * @returns {string | null}
 */
function extractFrontmatterDate(filePath) {
  try {
    const src = readFileSync(filePath, 'utf8');
    const fm = src.match(/^---\r?\n([\s\S]*?)\r?\n---/);
    if (!fm) return null;
    const block = fm[1];
    const pick = (/** @type {string} */ key) => {
      const m = block.match(new RegExp(`^${key}:\\s*['"]?(\\d{4}-\\d{2}-\\d{2})`, 'm'));
      return m ? m[1] : null;
    };
    const date = pick('lastUpdated') ?? pick('date') ?? pick('datePublished');
    return date ? new Date(date).toISOString() : null;
  } catch {
    return null;
  }
}

/**
 * Docs date themselves from the content file's last commit (the same helper
 * the docs page uses for its visible "Last updated" and JSON-LD dateModified),
 * with the frontmatter as the fallback, so the sitemap agrees with the page.
 * Blog posts are frozen content whose pages date themselves from frontmatter;
 * the sitemap follows the frontmatter for them too.
 * @returns {Map<string, string>}
 */
function buildLastmodMap() {
  /** @type {Map<string, string>} */
  const map = new Map();
  const collections = [
    { dir: 'src/content/blog', prefix: '/blog/', fromGit: false },
    { dir: 'src/content/docs', prefix: '/docs/', fromGit: true },
    // Explainers date their pages (dateModified) from frontmatter, like the blog.
    { dir: 'src/content/how-it-works', prefix: '/how-it-works/', fromGit: false },
  ];
  for (const { dir, prefix, fromGit } of collections) {
    /** @type {string[]} */
    let files = [];
    try {
      files = readdirSync(resolve(__dirname, dir));
    } catch {
      continue;
    }
    for (const file of files) {
      if (!/\.mdx?$/.test(file)) continue;
      const filePath = resolve(__dirname, dir, file);
      const gitDate = fromGit ? gitLastModified(filePath) : null;
      const iso = gitDate ? new Date(gitDate).toISOString() : extractFrontmatterDate(filePath);
      if (iso) map.set(`${prefix}${file.replace(/\.mdx?$/, '')}/`, iso);
    }
  }
  // The .astro pages (home, pricing, download, alternatives, ...) have no
  // content date; they date from the last commit to the page file. With no
  // usable git history (shallow clone, exported tree) they get no entry and
  // the sitemap omits their lastmod.
  for (const [route, iso] of staticPageLastmods(__dirname, gitLastModified)) {
    if (!map.has(route)) map.set(route, iso);
  }
  return map;
}

const lastmodMap = buildLastmodMap();
// Generated route families (/examples/<slug>/, /core-web-vitals/<metric>/)
// date from their template and data files; the family's own index page is a
// static page and keeps its own date.
const familyLastmods = routeFamilyLastmods(__dirname, gitLastModified);
// Each example page dates from its own catalog entry and capture, so a commit
// that re-measures a few examples does not re-date all of them; the family
// date above is the fallback.
for (const [route, iso] of exampleLastmods(__dirname, gitLastModified, gitFileHistory)) {
  if (!lastmodMap.has(route)) lastmodMap.set(route, iso);
}

// Filter topic pages (/docs/filters/<name>/): the thin ones are noindex and
// stay out of the sitemap until their group-page section is enriched (the guard
// lives in src/lib/filter-topics.mjs); the rest date themselves from the group
// page their prose comes from.
const filterTopics = loadFilterTopics(__dirname);
const indexableTopics = new Set(filterTopics.filter((t) => t.indexable).map((t) => t.name));
for (const t of filterTopics) {
  if (!t.indexable) continue;
  const iso = lastmodMap.get(`/docs/${t.section.page}/`);
  if (iso) lastmodMap.set(t.path, iso);
}

// Slugs of /examples/<slug>/ pages search engines may index: settled
// before/after data from a filter that acted. The rest are noindex (see
// examples/[slug].astro) and are kept out of the sitemap until the generator
// captures them applied — self-healing on next build.
const examplesWithData = new Set(
  EXAMPLES.filter((ex) => exampleImpact(ex.slug)?.indexable).map((ex) => ex.slug),
);

/** True when the last path segment has no file extension. */
function isExtensionless(/** @type {string} */ url) {
  return !basename(url).includes('.');
}

/**
 * Rewrite extensionless /1.0/ request URLs to their .html counterparts.
 * @param {import('http').IncomingMessage} req
 * @param {import('http').ServerResponse} _res
 * @param {() => void} next
 */
function rewriteLegacyUrl(req, _res, next) {
  const url = req.url || '';
  if (url.startsWith('/1.0/') && !url.endsWith('/') && isExtensionless(url)) {
    const htmlPath = join(publicDir, url + '.html');
    if (existsSync(htmlPath)) {
      req.url = url + '.html';
    }
  }
  next();
}

/**
 * Vite plugin: rewrite extensionless /1.0/ URLs to .html.
 * The legacy docs use extensionless hrefs but the files have .html extensions.
 * This runs in the dev/preview server; production uses Astro middleware.
 * @returns {import('vite').Plugin}
 */
function legacyDocsRewrite() {
  return {
    name: 'legacy-docs-rewrite',
    enforce: 'pre',
    configureServer(server) {
      server.middlewares.use(rewriteLegacyUrl);
    },
    configurePreviewServer(server) {
      server.middlewares.use(rewriteLegacyUrl);
    },
  };
}

// https://astro.build/config
export default defineConfig({
  // Astro 7 trims leading/trailing whitespace inside element text content,
  // which collides words wherever an inline tag sits on its own line
  // ("contributor to" + <a>link</a> renders as "contributor tolink"). This
  // site is served through ModPageSpeed, which optimizes HTML at the edge,
  // so build-time compression is redundant here. Keep this off unless the
  // whitespace handling is fixed upstream and the rendered text is rechecked.
  compressHTML: false,
  adapter: node({ mode: 'standalone' }),
  site: 'https://modpagespeed.com',
  security: {
    checkOrigin: true,
  },
  // 301 the old default-template slug to the descriptive origin-story slug.
  // The post was renamed hello-world.md -> why-i-rebuilt-mod-pagespeed.md;
  // this preserves any accrued links/bookmarks to the original URL.
  redirects: {
    '/blog/hello-world/': '/blog/why-i-rebuilt-mod-pagespeed/',
    // /vs/getpagespeed/ retired 2026-06-20 (reverses the D6 single-named-
    // page exception): we win the maintainer/alternative SERP on facts-about-us,
    // and a leader-names-challenger comparison page only lent the competitor
    // visibility. Forward the evaluation intent to the no-competitor-named page.
    '/vs/getpagespeed/': '/alternatives/ngx-pagespeed/',
    // /vs/ merged into /alternatives/ (one comparison hub): every comparison
    // page moved 1:1 by slug, the hub index folded into /alternatives/. Production
    // gets the same 301s from the server's redirect map; these keep dev and
    // preview in step. One hop each: no target is itself a redirect.
    '/vs/': '/alternatives/',
    '/vs/cloudflare-apo/': '/alternatives/cloudflare-apo/',
    '/vs/cloudinary/': '/alternatives/cloudinary/',
    '/vs/imgix/': '/alternatives/imgix/',
    '/vs/imgproxy/': '/alternatives/imgproxy/',
    '/vs/nitropack/': '/alternatives/nitropack/',
    '/vs/shortpixel/': '/alternatives/shortpixel/',
    '/vs/thumbor/': '/alternatives/thumbor/',
    '/vs/wp-rocket/': '/alternatives/wp-rocket/',
    // The activation guide became the software-license page when the product
    // stopped needing activation; keep inbound links and bookmarks working.
    '/docs/license-activation/': '/docs/license/',
    // The per-site claim and Community attestation flows retired with the
    // converged line; receipts and portal mails still link here, so both
    // forward to the software-license page.
    '/claim-license/': '/license/',
    '/community-license/': '/license/',
    // The release notes are the product's changelog; the conventional URL
    // forwards to the index page that lists every line.
    '/changelog/': '/docs/release-notes/',
    // The checkout placeholder retired with the support tiers: support is
    // quoted on /pricing/. Production gets the /buy/ 301 from the server's
    // redirect map; this entry keeps dev and preview in step.
    '/buy/': '/pricing/',
    // Reference pages folded into the generated references (2026-10): the
    // alphabetical directive index became the configuration reference (every
    // directive, generated from the module source), and the two filter hubs
    // merged into the one filters table at /docs/filters/.
    '/docs/directive-index/': '/docs/configuration/',
    '/docs/filter-reference/': '/docs/filters/',
    '/docs/filters-overview/': '/docs/filters/',
    // Docs structure: retired pages forward to their successors (one block,
    // kept contiguous so parallel docs branches merge cleanly).
    '/docs/documentation-plan/': '/docs/',
  },
  markdown: {
    // remarkDirective parses `:::caution[…]:::` container syntax; remarkCallouts
    // then turns those nodes into styled callout asides. Order matters — the
    // directive parser must run before the transform that consumes its nodes.
    // remarkLiteralTextDirectives puts inline `:word` text (host:port) back as
    // written; the site has no inline directives.
    // The plugins go to the remark/rehype processor itself; .mdx inherits it.
    // rehypeDirectiveBlocks marks the generated directive reference (an
    // attribute on every directive and area heading) and adds the directive
    // finder; it
    // leaves every page without that region untouched.
    // rehypeHeadingAnchors renders the "#" permalink into docs and
    // how-it-works headings at build time; it needs the heading ids, so
    // rehypeHeadingIds runs first (Astro runs it again at the end, which keeps
    // the ids already set).
    processor: unified({
      remarkPlugins: [
        remarkCustomHeadingId,
        remarkDirective,
        remarkLiteralTextDirectives,
        remarkCallouts,
      ],
      rehypePlugins: [rehypeDirectiveBlocks, rehypeHeadingIds, rehypeHeadingAnchors],
    }),
    // Dual-theme Shiki. `defaultColor: false` emits CSS variables instead of
    // baking one theme's colors into the HTML — the styles in global.css then
    // switch between --shiki-light and --shiki-dark based on `.dark` on <html>.
    // Without this, light-mode pages render code blocks in github-dark, which
    // produces a jarring high-contrast block against light prose.
    shikiConfig: {
      themes: {
        light: 'github-light',
        dark: 'github-dark',
      },
      defaultColor: false,
    },
  },
  integrations: [
    // MDX for docs pages that interpolate release manifest values via the
    // release-aware components in src/components/release/. Plain .md docs
    // continue to work; only the version-coupled docs use .mdx (§4).
    mdx(),
    sitemap({
      // Hand-written indexable pages in public/ are not Astro routes; list them.
      customPages: STATIC_INDEXABLE_PAGES.map(
        (path) => new URL(path, 'https://modpagespeed.com').href,
      ),
      // Exclude legacy 1.0 docs, the noindex checkout, and the noindex
      // /go/ redirect shims. Also drop /examples/<slug>/ detail pages that have
      // no generated data yet (they're noindex until the generator populates
      // them); the /examples/ index hub always stays in. The archived 1.15
      // release notes page is retired (folded into /docs/release-notes/), so
      // there is no /1.1/docs/ route left to exclude here.
      filter: (page) => {
        if (
          page.includes('/1.0/') ||
          page.includes('/buy/') ||
          page.includes('/go/') ||
          page.includes('/error/') ||
          page.endsWith('/contact/thanks/') ||
          // /privacy/ points at the one privacy notice on we-amp.com (production
          // answers it with a 301); the canonical page is there.
          page.endsWith('/privacy/')
        )
          return false;
        // The Markdown twins of the docs pages (/docs/<slug>.md) are for
        // agents; the HTML page is the one search engines should index.
        if (page.endsWith('.md')) return false;
        const m = new URL(page).pathname.match(/^\/examples\/([^/]+)\/$/);
        if (m) return examplesWithData.has(m[1]);
        const topic = new URL(page).pathname.match(/^\/docs\/filters\/([^/]+)\/$/);
        if (topic) return indexableTopics.has(topic[1]);
        return true;
      },
      // Per-entry lastmod from content dates or git history where available.
      // A URL with no real date (no usable git history in this checkout)
      // omits lastmod entirely — an always-changing build-time "now" makes
      // search engines distrust the freshness signal sitewide.
      serialize(item) {
        let path;
        try {
          path = new URL(item.url).pathname;
        } catch {
          path = '';
        }
        const iso =
          lastmodMap.get(path) ??
          familyLastmods.find((f) => path.startsWith(f.prefix) && path !== f.prefix)?.iso;
        if (iso) item.lastmod = iso;
        return item;
      },
    }),
  ],
  vite: {
    plugins: [legacyDocsRewrite(), tailwindcss()],
    define: {
      'import.meta.env.SITE_BUILD_ID': JSON.stringify(BUILD_ID),
      'import.meta.env.SITE_BUILD_ID_SET': JSON.stringify(Boolean(process.env.SITE_BUILD_ID)),
      'import.meta.env.PAGEFIND_BUNDLE_PATH': JSON.stringify(PAGEFIND_BUNDLE_PATH),
    },
  },
});
