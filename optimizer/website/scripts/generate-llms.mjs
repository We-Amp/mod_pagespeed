// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-llms.mjs — deterministic generator for the machine-readable surfaces
// that AI agents and crawlers read: public/llms.txt, public/llms-full.txt and
// public/.well-known/ai-plugin.json.
//
//   docs collection (src/content/docs/*.md|mdx, via src/lib/docs-markdown.mjs)
//     + release manifests (src/content/releases-*/release.yaml)
//     + facts (src/data/product-facts.mjs)
//     + the prose and page list in src/data/agent-files.mjs
//     => public/llms.txt       the llms.txt convention: H1, summary, about,
//                              then every docs page grouped as the sidebar
//                              groups them, the tools, the download/pricing/
//                              support pages, the blog posts (newest first) and
//                              the distribution channels
//     => public/llms-full.txt  the same index followed by the Markdown of
//                              every docs page (the text /docs/<slug>.md serves),
//                              the filters hub and the indexable filter topics
//                              (the blog posts are listed in the index only)
//
//   The filter topics list the gallery demos the topic pages list, from
//   src/lib/filter-demos.ts; that module is TypeScript (it reads the examples
//   catalog and its measurements), so it is loaded through Vite's module runner.
//
//   ai-plugin.json.tmpl (scripts/llms-templates/) + LLMS_TOKENS
//     => public/.well-known/ai-plugin.json
//
// Runs at prebuild (see package.json "prebuild") so the published files always
// match the single sources of truth. The byte-equivalence drift guard in
// test/sync/llms-generated.test.ts imports generateContent() from here and
// fails CI if the committed files differ from a fresh render — a docs page or a
// fact changed without regenerating (`npm run gen:llms`), or a hand edit.
//
// Output is deterministic: a stable order (sidebar order; the manifests' own
// field order), no timestamps beyond the manifests' release dates.
//
// Usage:
//   node scripts/generate-llms.mjs            # write public/llms*.txt + the manifest
//   node scripts/generate-llms.mjs --check    # exit 1 if committed files are stale (no write)

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import {
  LLMS_TOKENS,
  PRODUCT_NAME,
  PKG_APACHE_MODULE,
  PKG_NGINX_MODULE,
  PKG_OPTIMIZER,
  PKG_ASPNETCORE,
  PKG_SIDECAR,
  PKG_SIDECAR_NATIVE,
  SIDECAR_VERSION,
  SIDECAR_NGINX_VERSION,
  SIDECAR_RIDS,
  MODULE_PACKAGE_VERSION,
  ASPNETCORE_RIDS,
  ASPNETCORE_TFMS,
  NGINX_APT_MATRIX,
  NGINX_APT_ARCHES,
  STOCK_NGINX_ALMA,
} from '../src/data/product-facts.mjs';
import { SUMMARY, ABOUT, FILTERS_PAGE, SURFACES } from '../src/data/agent-files.mjs';
import {
  SITE,
  loadDocs,
  groupDocs,
  loadManifests,
  renderDocMarkdown,
  dockerTag,
  artifactUrl,
  substituteRelease,
  docUrl,
} from '../src/lib/docs-markdown.mjs';
import {
  loadFilterTopics,
  renderTopicMarkdown,
  renderFiltersHubMarkdown,
} from '../src/lib/filter-topics.mjs';
import { loadPosts, postUrl } from '../src/lib/blog-markdown.mjs';
import { runnerImport } from 'vite';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const WEBSITE_ROOT = resolve(__dirname, '..');
export const TEMPLATE_DIR = resolve(__dirname, 'llms-templates');
export const PUBLIC_DIR = resolve(__dirname, '..', 'public');

/**
 * What gets written. The two text files are built from the collection; the
 * AI-plugin manifest is JSON and still goes through the template+token path
 * (only the drift-prone version/price/variant facts are tokenized; token values
 * never contain double quotes or backslashes — see the PRICING_TIERS note rule
 * in product-facts.mjs — so substitution into a JSON string is safe; the drift
 * test JSON.parses the result, which would catch a violation).
 * @type {Array<{ output: string, build?: (ctx: Context) => string, template?: string }>}
 */
export const TARGETS = [
  { output: 'llms.txt', build: (ctx) => withNotice(decodeCharacterReferences(buildIndex(ctx))) },
  {
    output: 'llms-full.txt',
    build: (ctx) => withNotice(decodeCharacterReferences(buildFull(ctx))),
  },
  { template: 'ai-plugin.json.tmpl', output: '.well-known/ai-plugin.json' },
];

const UNRESOLVED_TOKEN = /\{\{\s*[A-Za-z0-9_]+\s*\}\}/;

/**
 * Substitute every {{TOKEN}} in `template` with its value from LLMS_TOKENS.
 * Throws if any {{TOKEN}} remains unresolved (typo or missing fact) so a bad
 * token can never silently ship into a public file.
 * @param {string} template
 * @param {string} name  source name, for error messages
 * @returns {string}
 */
export function render(template, name) {
  let out = template;
  for (const [token, value] of Object.entries(LLMS_TOKENS)) {
    out = out.replaceAll(`{{${token}}}`, value);
  }
  const leftover = out.match(UNRESOLVED_TOKEN);
  if (leftover) {
    throw new Error(
      `${name}: unresolved template token ${leftover[0]} — add it to LLMS_TOKENS ` +
        `in src/data/product-facts.mjs (or fix the typo in the source).`,
    );
  }
  return out;
}

/**
 * @typedef {{
 *   docs: import('../src/lib/docs-markdown.mjs').Doc[],
 *   groups: import('../src/lib/docs-markdown.mjs').DocGroup[],
 *   manifests: import('../src/lib/docs-markdown.mjs').Manifests,
 *   topics: import('../src/lib/filter-topics.mjs').Topic[],
 *   allTopics: import('../src/lib/filter-topics.mjs').Topic[],
 *   demos: Record<string, import('../src/lib/filter-demos.ts').DemoLink[]>,
 *   posts: import('../src/lib/blog-markdown.mjs').Post[],
 * }} Context
 */

/**
 * The gallery demos each indexable filter topic lists, from the module the
 * topic page itself uses.
 * @param {string} root
 * @param {import('../src/lib/filter-topics.mjs').Topic[]} topics
 */
async function loadDemos(root, topics) {
  /** @type {{ module: typeof import('../src/lib/filter-demos.ts') }} */
  const { module } = await runnerImport(resolve(root, 'src/lib/filter-demos.ts'), {
    configFile: false,
    root,
    logLevel: 'silent',
  });
  return Object.fromEntries(topics.map((t) => [t.name, module.alsoAppliedIn(t.name, t.example)]));
}

/**
 * Everything the builders read, loaded once.
 * @param {string} [root]
 * @returns {Promise<Context>}
 */
export async function loadContext(root = WEBSITE_ROOT) {
  const docs = loadDocs(root);
  // Only the indexable filter topic pages are listed; the thin ones are noindex
  // until their group-page section is enriched (src/lib/filter-topics.mjs).
  const allTopics = loadFilterTopics(root);
  const topics = allTopics.filter((t) => t.indexable);
  return {
    docs,
    groups: groupDocs(docs),
    manifests: loadManifests(root),
    topics,
    allTopics,
    demos: await loadDemos(root, topics),
    posts: loadPosts(root),
  };
}

/**
 * One index entry, the llms.txt line form.
 * @param {string} title
 * @param {string} url
 * @param {string} description
 */
function entry(title, url, description) {
  return `- [${title}](${url}): ${description}`;
}

/**
 * The distribution channels and packages, every name and version taken from
 * the release manifests and product-facts.mjs.
 * @param {Context} ctx
 * @returns {string[]}
 */
export function distributionLines(ctx) {
  const rel21 = ctx.manifests['2.1'];
  const rel20 = ctx.manifests['2.0'];
  const repo = rel21.urls.archive_base;
  const tag = dockerTag(rel21);
  const aspnetPkg = rel20.artifacts.nuget?.package ?? PKG_ASPNETCORE;
  const current = `${PRODUCT_NAME} ${rel21.release.semver} (${rel21.release.released_on})`;
  return [
    `- Signed apt and yum repository at ${repo} (GPG key ${rel21.urls.gpg_key}; one-time setup \`curl -fsSL ${repo}install.sh | sudo sh\`): \`${PKG_APACHE_MODULE}\` (the Apache module), \`${PKG_NGINX_MODULE}\` (the nginx module, prebuilt and pinned to each distribution's stock nginx: ${NGINX_APT_MATRIX}, ${NGINX_APT_ARCHES}; AlmaLinux 9 via yum against stock nginx ${STOCK_NGINX_ALMA}) and \`${PKG_OPTIMIZER}\` (the optimizer worker). Current release: ${current}${MODULE_PACKAGE_VERSION === rel21.release.semver ? '' : `; the module packages are ${MODULE_PACKAGE_VERSION}, a module-only update, and \`${PKG_OPTIMIZER}\` stays ${rel21.release.semver}`}. See [the apt/yum repository](${SITE}/download/apt-yum/) and [Install the module on Apache and nginx](${docUrl('installation-module')}).`,
    `- Container images on GHCR: \`${rel21.artifacts.docker.image}:${tag}\` (nginx with the module) and \`${rel21.artifacts.docker.worker_image}:${tag}\` (the optimizer worker), on ${rel21.compat.docker.base_images.join(', ')}. See [Install with Docker](${docUrl('installation-docker')}).`,
    `- Helm chart \`${rel21.artifacts.helm.chart}\` from the repository \`${rel21.artifacts.helm.repository}\` (\`helm repo add weamp ${rel21.artifacts.helm.repository}\`; index at ${rel21.artifacts.helm.repository}/index.yaml; application version ${substituteRelease(rel21.artifacts.helm.app_version, rel21)}; Kubernetes ${rel21.compat.kubernetes.versions.join(', ')}; Helm ${rel21.compat.kubernetes.helm_versions.join(', ')}). See [Deploy with Helm](${docUrl('helm-deployment')}).`,
    `- IIS: the signed Windows installer, ${MODULE_PACKAGE_VERSION}, on the module's GitHub release: ${artifactUrl(rel21, 'msi', 'win_x64', MODULE_PACKAGE_VERSION)} (${rel21.compat.iis.os.join(', ')}; IIS ${rel21.compat.iis.iis_versions.join(', ')}; x64). See [Install on IIS](${docUrl('install-iis')}).`,
    `- NuGet \`${aspnetPkg}\` ${rel21.release.semver}, the ASP.NET Core middleware (${ASPNETCORE_TFMS.join(', ')}; ${ASPNETCORE_RIDS.join(', ')}): \`dotnet add package ${aspnetPkg}\`. See [Install ASP.NET Core middleware](${docUrl('aspnet-getting-started')}).`,
    `- NuGet \`${PKG_SIDECAR}\` with \`${PKG_SIDECAR_NATIVE}\`, ${SIDECAR_VERSION}, the module as an ASP.NET Core sidecar (${SIDECAR_RIDS.join(', ')}; bundles nginx ${SIDECAR_NGINX_VERSION}): \`dotnet add package ${PKG_SIDECAR}\`.`,
    `- Worker packages (deb, rpm and Windows zip, with a checksum list) of every release: the [release listing](https://github.com/We-Amp/pagespeed-optimizer/releases/latest). The module packages install from the signed repositories above; see [Download](${SITE}/download/).`,
  ];
}

/**
 * The index: H1, summary, about, then the link sections.
 * @param {Context} ctx
 * @returns {string}
 */
export function buildIndex(ctx) {
  /** @type {string[]} */
  const lines = [
    `# ${PRODUCT_NAME}`,
    '',
    `> ${render(SUMMARY, 'agent-files.mjs SUMMARY')}`,
    '',
    render(ABOUT, 'agent-files.mjs ABOUT'),
    '',
  ];
  for (const group of ctx.groups) {
    lines.push(`## ${group.label}`, '');
    for (const doc of group.docs) {
      lines.push(entry(doc.data.title, docUrl(doc.slug), doc.data.description));
    }
    if (group.label === 'Filters') {
      for (const t of ctx.topics) {
        lines.push(
          entry(`${t.human} (${t.name})`, `${SITE}${t.path}`, t.summary.replace(/\.?$/, '.')),
        );
      }
    }
    if (group.label === 'Reference') {
      lines.push(
        entry(FILTERS_PAGE.title, `${SITE}${FILTERS_PAGE.path}`, FILTERS_PAGE.description),
      );
    }
    lines.push('');
  }
  for (const { section, items } of SURFACES) {
    lines.push(`## ${section}`, '');
    for (const item of items) {
      lines.push(
        entry(
          render(item.title, 'agent-files.mjs SURFACES'),
          /^https:\/\//.test(item.path) ? item.path : `${SITE}${item.path}`,
          render(item.description, 'agent-files.mjs SURFACES'),
        ),
      );
    }
    lines.push('');
  }
  lines.push('## Blog', '');
  for (const post of ctx.posts) {
    lines.push(
      entry(post.data.title, postUrl(post.slug), `${post.date}. ${post.data.description}`),
    );
  }
  lines.push('');
  lines.push('## Distribution', '', ...distributionLines(ctx), '');
  return lines.join('\n');
}

/**
 * The index followed by every page's Markdown, in sidebar order, then the
 * filters hub and the indexable filter topics in index order (the text
 * /docs/filters.md and /docs/filters/<name>.md serve).
 * @param {Context} ctx
 * @returns {string}
 */
export function buildFull(ctx) {
  const parts = [buildIndex(ctx).trimEnd(), ''];
  for (const doc of ctx.docs) {
    parts.push('---', '', renderDocMarkdown(doc, ctx.manifests).trimEnd(), '');
  }
  parts.push('---', '', renderFiltersHubMarkdown(ctx.allTopics).trimEnd(), '');
  for (const t of ctx.topics) {
    parts.push('---', '', renderTopicMarkdown(t, ctx.allTopics, ctx.demos[t.name]).trimEnd(), '');
  }
  return parts.join('\n');
}

/**
 * The named character references the sources use, plus the common ones near
 * them. A name not listed here is left as written: the output then shows the
 * reference rather than a wrong character.
 * @type {Record<string, string>}
 */
const NAMED_REFERENCES = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  nbsp: ' ',
  ndash: '–',
  mdash: '—',
  hellip: '…',
  lsquo: '‘',
  rsquo: '’',
  ldquo: '“',
  rdquo: '”',
  larr: '←',
  rarr: '→',
  times: '×',
  middot: '·',
  bull: '•',
  deg: '°',
  copy: '©',
  reg: '®',
  trade: '™',
};

const CHARACTER_REFERENCE =
  /&(?:#([0-9]{1,7})|#[xX]([0-9a-fA-F]{1,6})|([A-Za-z][A-Za-z0-9]{1,31}));/g;

/**
 * Decode the character references in a run of prose, in one pass, so
 * `&amp;lt;` becomes `&lt;` and not `<`. A numeric reference to NUL, a
 * surrogate or a code point past U+10FFFF, and an unknown name, stay as written.
 * @param {string} text
 * @returns {string}
 */
export function decodeReferences(text) {
  return text.replace(CHARACTER_REFERENCE, (ref, dec, hex, name) => {
    if (name !== undefined) return NAMED_REFERENCES[name] ?? ref;
    const cp = dec !== undefined ? Number.parseInt(dec, 10) : Number.parseInt(hex, 16);
    if (cp === 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return ref;
    return String.fromCodePoint(cp);
  });
}

/**
 * Decode the references in one paragraph, keeping inline code spans as
 * written: a backtick run opens a span that the next run of the same length
 * closes; a run with no partner is plain text.
 * @param {string} par
 * @returns {string}
 */
function decodeOutsideCodeSpans(par) {
  let out = '';
  let i = 0;
  while (i < par.length) {
    const open = par.indexOf('`', i);
    if (open < 0) {
      out += decodeReferences(par.slice(i));
      break;
    }
    out += decodeReferences(par.slice(i, open));
    let end = open;
    while (par[end] === '`') end++;
    const run = end - open;
    let close = -1;
    for (let j = end; j < par.length;) {
      if (par[j] !== '`') {
        j++;
        continue;
      }
      let k = j;
      while (par[k] === '`') k++;
      if (k - j === run) {
        close = j;
        break;
      }
      j = k;
    }
    if (close < 0) {
      out += par.slice(open, end);
      i = end;
    } else {
      out += par.slice(open, close + run);
      i = close + run;
    }
  }
  return out;
}

/**
 * The llms files are plain text, so the HTML character references the
 * Markdown sources carry (`&lt;`, `&#39;`, `&nbsp;`) are decoded to their
 * characters. Fenced code blocks and inline code spans are kept as written,
 * because there the author meant the literal text. Code spans are matched
 * within a paragraph (lines up to a blank line or a fence), as Markdown does.
 * @param {string} md
 * @returns {string}
 */
export function decodeCharacterReferences(md) {
  /** @type {string[]} */
  const out = [];
  /** @type {string[]} */
  let para = [];
  const flush = () => {
    if (para.length) out.push(decodeOutsideCodeSpans(para.join('\n')));
    para = [];
  };
  /** @type {string | null} */
  let fence = null;
  for (const line of md.split('\n')) {
    const f = line.match(/^\s*(`{3,}|~{3,})(.*)$/);
    if (fence) {
      out.push(line);
      if (f && f[1][0] === fence[0] && f[1].length >= fence.length && f[2].trim() === '') {
        fence = null;
      }
      continue;
    }
    if (f && !(f[1][0] === '`' && f[2].includes('`'))) {
      flush();
      fence = f[1];
      out.push(line);
      continue;
    }
    if (line.trim() === '') {
      flush();
      out.push(line);
      continue;
    }
    para.push(line);
  }
  flush();
  return out.join('\n');
}

/** @param {string} text */
function withNotice(text) {
  return (
    text.trimEnd() +
    '\n\n<!-- Generated by scripts/generate-llms.mjs from the docs collection, the release manifests and src/data/product-facts.mjs; regenerate with `npm run gen:llms`, never edit by hand. -->\n'
  );
}

/**
 * Render every target. Returns { outputFilename: content }.
 * This is the single render path shared by the CLI and the drift-guard test.
 * @param {string} [root]
 * @returns {Promise<Record<string, string>>}
 */
export async function generateContent(root = WEBSITE_ROOT) {
  const ctx = await loadContext(root);
  /** @type {Record<string, string>} */
  const result = {};
  for (const target of TARGETS) {
    if (target.build) {
      result[target.output] = target.build(ctx);
    } else if (target.template) {
      const tmpl = readFileSync(resolve(TEMPLATE_DIR, target.template), 'utf8');
      result[target.output] = render(tmpl, target.template);
    }
  }
  return result;
}

async function main() {
  const checkOnly = process.argv.includes('--check');
  const content = await generateContent();
  let stale = 0;

  for (const { output } of TARGETS) {
    const outPath = resolve(PUBLIC_DIR, output);
    const rendered = content[output];

    if (checkOnly) {
      let current = '';
      try {
        current = readFileSync(outPath, 'utf8');
      } catch {
        /* missing file counts as stale */
      }
      if (current !== rendered) {
        stale++;
        console.error(
          `STALE: public/${output} differs from generated output. Run \`npm run gen:llms\`.`,
        );
      } else {
        console.log(`OK: public/${output} is up to date.`);
      }
    } else {
      writeFileSync(outPath, rendered);
      console.log(`Generated public/${output} (${rendered.length} bytes).`);
    }
  }

  if (checkOnly && stale > 0) {
    process.exit(1);
  }
}

// Run the CLI only when invoked directly (`node scripts/generate-llms.mjs`),
// not when imported by the drift-guard test.
if (import.meta.url === pathToFileURL(process.argv[1] || '').href) {
  await main();
}
