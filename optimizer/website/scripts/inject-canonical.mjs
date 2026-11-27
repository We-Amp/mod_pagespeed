#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Inject canonical link tags into archived /1.0/doc/*.html files.
 *
 * Pages with new 2.0 equivalents get a canonical pointing to the new URL.
 * Archive-only pages get a self-referencing canonical (/1.0/doc/...).
 *
 * Run once: node scripts/inject-canonical.mjs
 */

import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, basename } from 'node:path';

const SITE = 'https://modpagespeed.com';
const docDir = join(import.meta.dirname, '..', 'public', '1.0', 'doc');

// Old doc slug → new 2.0 canonical path (without trailing slash the tag adds it)
const NEW_EQUIVALENTS = {
  configuration: '/docs/configuration/',
  build_ngx_pagespeed_from_source: '/docs/installation-module/',
  build_from_source: '/docs/installation-module/',
  build_mod_pagespeed_from_source: '/docs/installation-module/',
  download: '/docs/getting-started/',
  release_notes: '/docs/release-notes/',
  'filter-image-optimize': '/docs/image-filters/',
  'reference-image-optimize': '/docs/image-filters/',
  system: '/docs/deployment/',
  faq: '/docs/troubleshooting/',
  admin: '/docs/api-reference/',
  console: '/docs/api-reference/',
  index: '/docs/',
};

const files = readdirSync(docDir).filter((f) => f.endsWith('.html'));
let modified = 0;

for (const file of files) {
  const filePath = join(docDir, file);
  let html = readFileSync(filePath, 'utf-8');

  // Skip if canonical already present
  if (html.includes('rel="canonical"')) {
    console.log(`  SKIP (already has canonical): ${file}`);
    continue;
  }

  const slug = basename(file, '.html');
  const newPath = NEW_EQUIVALENTS[slug];
  const canonicalUrl = newPath ? `${SITE}${newPath}` : `${SITE}/1.0/doc/${slug}`;

  const tag = `    <link rel="canonical" href="${canonicalUrl}" />`;

  // Insert after <head> line
  html = html.replace(/^(\s*<head>)$/m, `$1\n${tag}`);

  writeFileSync(filePath, html, 'utf-8');
  modified++;
  const label = newPath ? `→ ${newPath}` : '(self-referencing)';
  console.log(`  OK: ${file} ${label}`);
}

console.log(`\nDone. Modified ${modified} of ${files.length} files.`);
