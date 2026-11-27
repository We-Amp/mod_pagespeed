#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Pre-build pricing fetch script.
 *
 * Fetches localized pricing for ModPageSpeed Pro products from FastSpring's
 * REST API and writes src/data/fastspring-pricing.json.  The output file is
 * committed to git so it doubles as a stale-but-usable fallback when the API
 * is unreachable.
 *
 * Environment variables (sourced from ~/.weamp/credentials.env):
 *   FASTSPRING_API_USER — FastSpring API username
 *   FASTSPRING_API_PASS — FastSpring API password
 *
 * Usage:  node scripts/fetch-pricing.js
 */

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  describePricingCacheAge,
  isPricingCacheFresh,
  pricingCacheAge,
} from './lib/pricing-cache-age.mjs';

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const DATA_DIR = join(ROOT, 'src', 'data');
const OUTPUT_FILE = join(DATA_DIR, 'fastspring-pricing.json');

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// ladder SKUs. The legacy mps{1,2}-pro-* slugs stay in the array
// during the transition so the committed fallback still covers in-field
// admin consoles deep-linking /buy/?product=mps2-pro-*; dropping them is
// owned by P3.6 (the same change as the FastSpring retirement), not "later".
const PRODUCTS = [
  'starter-site-annual',
  'starter-site-monthly',
  'business-site-annual',
  'business-site-monthly',
  'mps2-pro-monthly',
  'mps2-pro-annual',
  'mps1-pro-monthly',
  'mps1-pro-annual',
];
const SPOT_CHECK_COUNTRIES = ['US', 'DE', 'GB'];
const MIN_COUNTRIES = 200;
const STALENESS_HOURS = 48;
const API_BASE = 'https://api.fastspring.com';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function fatal(msg) {
  console.error(`\x1b[31mERROR:\x1b[0m ${msg}`);
  process.exit(1);
}

function warn(msg) {
  console.warn(`\x1b[33mWARNING:\x1b[0m ${msg}`);
}

function info(msg) {
  console.log(`\x1b[36mINFO:\x1b[0m ${msg}`);
}

/**
 * Read the cached pricing file if it exists. Returns { data, age, fresh } or
 * null; `age` is measured from the data's own fetchedAt timestamp (see
 * lib/pricing-cache-age.mjs).
 */
function readCachedFile() {
  try {
    const data = JSON.parse(readFileSync(OUTPUT_FILE, 'utf-8'));
    const age = pricingCacheAge(data);
    return { data, age, fresh: isPricingCacheFresh(age, STALENESS_HOURS) };
  } catch {
    return null;
  }
}

/**
 * Make an authenticated GET request to the FastSpring API.
 */
async function fsGet(path) {
  const url = `${API_BASE}${path}`;
  const auth = Buffer.from(`${apiUser}:${apiPass}`).toString('base64');

  const res = await fetch(url, {
    headers: {
      Authorization: `Basic ${auth}`,
      Accept: 'application/json',
    },
  });

  if (!res.ok) {
    throw new Error(`FastSpring API ${res.status} ${res.statusText} for ${url}`);
  }

  return res.json();
}

// ---------------------------------------------------------------------------
// Credentials (checked lazily — a fresh cache skips the API entirely)
// ---------------------------------------------------------------------------

const apiUser = process.env.FASTSPRING_API_USER;
const apiPass = process.env.FASTSPRING_API_PASS;

// Freshness is read from the cached data's own fetchedAt timestamp, which
// records when the prices were fetched; the file's mtime only records when a
// checkout wrote it. A build that structurally cannot hold credentials -- a
// container image build, for one -- gets the same committed fallback either
// way, however old it is.
// PRICING_ALLOW_STALE lets such a build proceed on the committed file, loudly.
// It is never set for a build that publishes prices to customers.
const allowStale = process.env.PRICING_ALLOW_STALE === '1';

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

async function main() {
  // Ensure output directory exists
  mkdirSync(DATA_DIR, { recursive: true });

  const cached = readCachedFile();

  // If a fresh cached file exists, use it without requiring API credentials.
  // This allows builds in CI/CD or new-developer environments that lack
  // FastSpring credentials, as long as the committed JSON is recent enough --
  // or as long as the build has declared that it cannot do better.
  if (cached && (cached.fresh || allowStale) && (!apiUser || !apiPass)) {
    warn(
      'FastSpring API credentials not available. ' +
        `Using cached pricing (${describePricingCacheAge(cached.age, STALENESS_HOURS)})` +
        (cached.fresh ? '.' : ' -- PRICING_ALLOW_STALE is set.'),
    );
    return;
  }

  if (!apiUser || !apiPass) {
    // Two different problems wear the same failure here, and saying the wrong
    // one sends the reader looking for credentials that were never the point.
    if (cached) {
      fatal(
        `Cached pricing is stale (${describePricingCacheAge(cached.age, STALENESS_HOURS)}) ` +
          'and there are no FastSpring API credentials to refresh it with.\n' +
          '  Source your credentials before running this script:\n\n' +
          '    source ~/.weamp/credentials.env\n\n' +
          '  A build that cannot hold credentials and accepts the committed\n' +
          '  fallback sets PRICING_ALLOW_STALE=1 instead.',
      );
    }
    fatal(
      'Missing FASTSPRING_API_USER and/or FASTSPRING_API_PASS environment variables.\n' +
        '  Source your credentials before running this script:\n\n' +
        '    source ~/.weamp/credentials.env\n\n' +
        '  See scripts/credentials.env.example for the expected format.',
    );
  }

  let products;

  try {
    products = await fetchAllPricing();
  } catch (err) {
    warn(`FastSpring API unreachable: ${err.message}`);

    if (cached && cached.fresh) {
      warn(`Using cached pricing (${describePricingCacheAge(cached.age, STALENESS_HOURS)}).`);
      // Leave the existing file in place
      return;
    }

    if (cached) {
      fatal(
        `Cached pricing is stale (${describePricingCacheAge(cached.age, STALENESS_HOURS)}). ` +
          'Cannot build with stale prices. Fix the API connection and retry.',
      );
    }

    fatal('No cached pricing file exists and the API is unreachable. Cannot build.');
  }

  const output = {
    fetchedAt: new Date().toISOString(),
    products,
  };

  writeFileSync(OUTPUT_FILE, JSON.stringify(output, null, 2) + '\n', 'utf-8');
  info(`Wrote ${OUTPUT_FILE}`);
}

// ---------------------------------------------------------------------------
// Fetch logic
// ---------------------------------------------------------------------------

/**
 * Fetch bulk pricing for all products and run validation.
 * Returns the products map: { "mps2-pro-monthly": { "US": {...}, ... }, ... }
 */
async function fetchAllPricing() {
  const products = {};

  // 1. Fetch bulk pricing for each product (all countries)
  for (const productId of PRODUCTS) {
    info(`Fetching pricing for ${productId} ...`);
    const bulk = await fsGet(`/products/price/${productId}`);
    const pricing = extractPricing(bulk, productId);

    // Sanity check: at least MIN_COUNTRIES countries
    const countryCount = Object.keys(pricing).length;
    if (countryCount < MIN_COUNTRIES) {
      fatal(
        `Expected >= ${MIN_COUNTRIES} countries for ${productId}, ` +
          `got ${countryCount}. API response may be incomplete.`,
      );
    }
    info(`  ${countryCount} countries received.`);

    products[productId] = pricing;
  }

  // 2. Cross-check: fetch per-country pricing for spot-check countries
  for (const productId of PRODUCTS) {
    for (const country of SPOT_CHECK_COUNTRIES) {
      info(`Cross-checking ${productId} / ${country} ...`);
      const perCountry = await fsGet(`/products/price/${productId}?country=${country}`);
      const perCountryPricing = extractPricing(perCountry, productId);

      const bulkEntry = products[productId][country];
      const checkEntry = perCountryPricing[country];

      if (!bulkEntry) {
        fatal(`Bulk response missing country ${country} for ${productId}.`);
      }
      if (!checkEntry) {
        fatal(`Per-country response missing ${country} for ${productId}.`);
      }

      if (bulkEntry.currency !== checkEntry.currency) {
        fatal(
          `Currency mismatch for ${productId}/${country}: ` +
            `bulk=${bulkEntry.currency}, per-country=${checkEntry.currency}`,
        );
      }
      if (Math.abs(bulkEntry.price - checkEntry.price) > 0.01) {
        fatal(
          `Price mismatch for ${productId}/${country}: ` +
            `bulk=${bulkEntry.price}, per-country=${checkEntry.price}`,
        );
      }
    }
  }

  info('All cross-checks passed.');
  return products;
}

/**
 * Extract the flat pricing map from a FastSpring /products/price response.
 *
 * FastSpring returns:
 *   { "products": [{ "product": "...", "pricing": { "US": {...}, ... } }] }
 *
 * We return:
 *   { "US": { "currency": "USD", "price": 49.0, "display": "$49.00" }, ... }
 *
 * `display` is normalized per brand typography rules (see normalizeDisplay).
 */
function extractPricing(apiResponse, productId) {
  const productsArr = apiResponse?.products;
  if (!Array.isArray(productsArr) || productsArr.length === 0) {
    fatal(`Unexpected API response structure for ${productId}: missing "products" array.`);
  }

  const entry = productsArr.find((p) => p.product === productId);
  if (!entry) {
    fatal(`Product "${productId}" not found in API response.`);
  }

  if (entry.result && entry.result !== 'success') {
    fatal(`API returned result="${entry.result}" for ${productId}.`);
  }

  const rawPricing = entry.pricing;
  if (!rawPricing || typeof rawPricing !== 'object') {
    fatal(`No pricing data in API response for ${productId}.`);
  }

  // Flatten to { country: { currency, price, display } }, normalizing each entry.
  const result = {};
  for (const [country, data] of Object.entries(rawPricing)) {
    if (!data.currency || data.price == null || !data.display) {
      warn(`Skipping incomplete pricing entry for ${productId}/${country}.`);
      continue;
    }
    const price = normalizePrice(data.currency, data.price);
    result[country] = {
      currency: data.currency,
      price,
      display: normalizeDisplay(country, data.currency, price, data.display),
    };
  }

  return result;
}

// ---------------------------------------------------------------------------
// Display normalization (brand typography rules)
// ---------------------------------------------------------------------------
//
// FastSpring returns locale-formatted `display` strings that mix conventions
// (e.g. `$49.00`, `US$49.00`, `€ 49,83`, `49,00 €`, `₹ 1,998.92`). The brand
// guide wants a single consistent presentation per currency family:
//   - INR: whole rupees, no decimals, `₹X,XXX` (en-IN grouping)
//   - USD in canonical-US territories: no cents, `$X`
//   - EUR: `€` prefix with no space, keep API's decimal separator
// Other currencies pass through unchanged.

const CANONICAL_USD_COUNTRIES = new Set([
  'US',
  'AS',
  'AQ',
  'GU',
  'MH',
  'FM',
  'MP',
  'PW',
  'PR',
  'VI',
  'UM',
  'BV',
  'IO',
]);

function normalizePrice(currency, price) {
  if (currency === 'INR') return Math.round(price);
  return price;
}

function normalizeDisplay(country, currency, price, rawDisplay) {
  if (currency === 'INR') {
    return '₹' + price.toLocaleString('en-IN', { maximumFractionDigits: 0 });
  }
  if (currency === 'USD' && CANONICAL_USD_COUNTRIES.has(country)) {
    return '$' + Math.round(price);
  }
  if (currency === 'EUR') {
    const stripped = rawDisplay.replace(/EUR/g, '').replace(/€/g, '').trim();
    return '€' + stripped;
  }
  return rawDisplay;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

main().catch((err) => {
  fatal(`Unexpected error: ${err.message}`);
});
