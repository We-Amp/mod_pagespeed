// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// console-facts-sync.test.ts — LOCKSTEP GUARD for the console-facing product
// facts. shared/product-facts.mjs (repo root) is the file the admin console
// copies verbatim and byte-verifies; src/data/product-facts.mjs re-exports the
// identity constants from it so the website reads the same single definition.
// This fails CI if:
//   1. the shared module's export set changes (the console's copy declares
//      exactly these names — an accidental addition or removal must be a
//      deliberate edit here too);
//   2. the shared values are not what the console expects (identity facts,
//      the one product statement, the support-terms URL);
//   3. the website re-exports drift from the shared definitions, or the
//      website file re-declares one of the names locally;
//   4. the shared file stops being a standalone, dependency-free ESM module
//      (the console consumes the copy without the website tree), or it grows
//      content that has no business in front of a console user — pricing,
//      plans, or any leftover from the retired licensing model.

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';

import * as shared from '../../../shared/product-facts.mjs';
import * as website from '../../src/data/product-facts.mjs';

const SHARED_PATH = resolve(__dirname, '../../../shared/product-facts.mjs');
const WEBSITE_PATH = resolve(__dirname, '../../src/data/product-facts.mjs');
const sharedSource = readFileSync(SHARED_PATH, 'utf8');
const websiteSource = readFileSync(WEBSITE_PATH, 'utf8');

// The identity facts the console's type declarations consume.
const IDENTITY_EXPORTS = [
  'VENDOR',
  'VENDOR_URL',
  'WEBSITE',
  'PRODUCT_NAME',
  'PRODUCT_DISPLAY_NAME',
  'PRIVACY_URL',
  'TERMS_URL',
  'SUPPORT_URL',
] as const;
// Plus the support pointer the console renders.
const POINTER_EXPORTS = ['PRODUCT_STATEMENT', 'SUPPORT_TERMS_URL'] as const;

type SharedExports = Record<string, unknown>;
const sharedRecord = shared as SharedExports;
const websiteRecord = website as SharedExports;

describe('console-facing product facts (shared/product-facts.mjs)', () => {
  it('1. exports exactly the names the console consumes', () => {
    expect(Object.keys(sharedRecord).sort()).toEqual(
      [...IDENTITY_EXPORTS, ...POINTER_EXPORTS].sort(),
    );
    for (const name of [...IDENTITY_EXPORTS, ...POINTER_EXPORTS]) {
      expect(typeof sharedRecord[name], name).toBe('string');
      expect((sharedRecord[name] as string).length, name).toBeGreaterThan(0);
    }
  });

  it('2a. the identity facts are internally consistent', () => {
    expect(shared.PRIVACY_URL).toBe(`${shared.WEBSITE}/privacy/`);
    expect(shared.TERMS_URL).toBe(`${shared.WEBSITE}/terms/`);
    expect(shared.SUPPORT_URL).toBe(`${shared.WEBSITE}/pricing/`);
    // Trailing slash is canonical on every URL that has a path; the bare site
    // origin has none (it is composed into paths).
    expect(shared.WEBSITE).not.toMatch(/\/$/);
    for (const name of ['VENDOR_URL', 'PRIVACY_URL', 'TERMS_URL', 'SUPPORT_URL'] as const) {
      expect(shared[name], name).toMatch(/^https:\/\/.+\/$/);
    }
  });

  it('2b. the product statement and the support-terms URL are the agreed values', () => {
    expect(shared.PRODUCT_STATEMENT).toBe('mod_pagespeed is open source (Apache-2.0).');
    expect(shared.SUPPORT_TERMS_URL).toBe('https://we-amp.com/licensing/');
  });

  it('3a. the website re-exports every identity fact unchanged', () => {
    for (const name of IDENTITY_EXPORTS) {
      expect(websiteRecord[name], name).toBe(sharedRecord[name]);
    }
  });

  it('3b. the website file re-exports from the shared module and never re-declares a name', () => {
    const reexport = websiteSource.match(
      /export \{([^}]*)\} from '\.\.\/\.\.\/\.\.\/shared\/product-facts\.mjs';/,
    );
    expect(reexport, 'export { … } from the shared module').not.toBeNull();
    const reexported = reexport![1]
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean)
      .sort();
    expect(reexported).toEqual([...IDENTITY_EXPORTS].sort());
    for (const name of IDENTITY_EXPORTS) {
      expect(websiteSource, `local re-declaration of ${name}`).not.toMatch(
        new RegExp(`^export (const|let|var) ${name}\\b`, 'm'),
      );
    }
  });

  it('4a. the shared file is a standalone, dependency-free ESM module', () => {
    // The console copies the file verbatim and builds it without this tree, so
    // it may not import anything (relative or bare) or reach for CommonJS.
    expect(sharedSource).not.toMatch(/^\s*import\b/m);
    expect(sharedSource).not.toMatch(/\bfrom\s+['"]/);
    expect(sharedSource).not.toMatch(/\brequire\s*\(/);
    expect(sharedSource).not.toMatch(/\bprocess\.|\bimport\.meta\b/);
    expect(sharedSource).toMatch(/^export const VENDOR = /m);
  });

  it('4b. the shared file carries no pricing, plan or retired-licensing content', () => {
    const forbidden: RegExp[] = [
      /licen[cs]e[- ]key/i,
      /activat/i,
      /evaluat/i,
      /\btrial\b/i,
      /unlicensed/i,
      /x-pagespeed-warn/i,
      /warn(ing)? header/i,
      /\btiers?\b/i,
      /\bladder\b/i,
      /[$€£]\s*\d/,
      /\d\s*\/\s*(month|year|site|host)\b/i,
      /\b(USD|EUR)\b/,
      /\bper[- ]site\b/i,
      /PRICING|PRICE_|TIERS/,
    ];
    for (const re of forbidden) {
      expect(sharedSource, String(re)).not.toMatch(re);
    }
  });
});
