// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Build the Pagefind search index over the production build output.
 *
 * Runs as the `postbuild` npm script, so `npm run build` always produces the
 * index. Astro's node adapter writes the prerendered pages and the static
 * assets to dist/client/ and serves them from there, so the index is written
 * to dist/client/pagefind/v<version>/ and is reachable at /pagefind/v<version>/
 * on the live site (see src/lib/pagefind-bundle.mjs for why the version is in
 * the path).
 * The dev server has no build output and therefore no index; the search
 * dialog says so when the bundle fails to load.
 *
 * Page selection is declared in the HTML, not here: BaseLayout marks the
 * searchable region with `data-pagefind-body` and leaves it off noindex pages
 * and the utility routes (/api/, /go/, /buy/, /error/, 404, 500). Pagefind
 * skips every page without that attribute, which also covers the legacy
 * /1.0/ archive (static HTML that never carries it). `data-pagefind-ignore`
 * on navigation chrome keeps sidebar titles out of the excerpts.
 *
 * The written output is checked before the script reports success: a missing
 * or 0-byte file under the bundle directory fails the build (see
 * lib/search-index-output.mjs), because the dialog would load it without
 * complaint. Rebuilding is the remedy.
 */
import * as pagefind from 'pagefind';
import { existsSync, readFileSync } from 'node:fs';
import { relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { findBadSearchOutput } from './lib/search-index-output.mjs';
import { PAGEFIND_BUNDLE_PATH } from '../src/lib/pagefind-bundle.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));
const site = resolve(root, 'dist/client');
const outputPath = resolve(site, '.' + PAGEFIND_BUNDLE_PATH);

function fail(message, errors = []) {
  console.error(`build-search-index: ${message}`);
  for (const e of errors) console.error(`  ${e}`);
  process.exit(1);
}

if (!existsSync(site)) fail(`${site} does not exist; run \`astro build\` first.`);

const { index, errors: createErrors } = await pagefind.createIndex({
  // One language, one index: the whole site is <html lang="en">.
  forceLanguage: 'en',
});
if (!index) fail('could not create the index.', createErrors);

const { page_count, errors: addErrors } = await index.addDirectory({ path: site });
if (addErrors.length) fail('indexing reported errors.', addErrors);
if (page_count === 0) fail(`no HTML files found under ${site}.`);

const { errors: writeErrors } = await index.writeFiles({ outputPath });
if (writeErrors.length) fail('writing the index failed.', writeErrors);
await pagefind.close();

const bad = findBadSearchOutput(outputPath);
if (bad.length) fail('Pagefind wrote missing or empty files; rebuild.', bad);

// `page_count` above counts the HTML files Pagefind looked at; the entry file
// records how many it actually indexed (the ones carrying data-pagefind-body).
const entry = JSON.parse(readFileSync(resolve(outputPath, 'pagefind-entry.json'), 'utf8'));
const indexed = Object.values(entry.languages ?? {}).reduce((n, l) => n + (l.page_count ?? 0), 0);
if (indexed === 0) {
  fail(
    'no pages were indexed. Did `data-pagefind-body` disappear from BaseLayout.astro? ' +
      'Pagefind indexes only the pages that carry it.',
  );
}
console.log(
  `build-search-index: scanned ${page_count} files, indexed ${indexed} pages into ${relative(root, outputPath)}/`,
);
