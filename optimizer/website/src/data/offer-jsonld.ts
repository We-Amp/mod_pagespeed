// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Single source of truth for the schema.org offer node emitted across the site.
//
// Converged model: the standard signed packages are free, so the node is a
// single €0 Offer. Google
// requires a Product node to carry offers/review/aggregateRating — silently
// dropping offers would invalidate the Product markup on every importing page,
// and a €0 Offer is accurate (the standard packages cost nothing) and
// rich-result-valid. (The pre-GA AggregateOffer spanned the paid per-site
// license rungs; the license ladder is retired.) The three support tiers are
// separate priced Offer nodes (supportTierOffersJsonLd) — never emit a
// price-less Offer.
//
// Centralizes `priceValidUntil` (recommended by Google for offer eligibility)
// so it doesn't drift across pages, and keeps the description derived from
// product-facts.mjs — no price, license or support literals in this file.

import {
  LICENSE_CLAUSE_CAP,
  SUPPORT_URL,
  PRICE_VALID_UNTIL,
  SUPPORT_TIERS,
  formatEur,
} from './product-facts.mjs';

// Re-exported from product-facts.mjs. Google treats a stale priceValidUntil as
// expired, so the canonical value is bumped ~yearly in product-facts.mjs.
export { PRICE_VALID_UNTIL };

// The one sentence every offer node carries. The license clause comes from
// product-facts.mjs (publication-derived: "licensed under" today, "open source
// under" once the source is published); support plans are
// sold separately and are deliberately not part of this offer (the standard
// packages cost €0).
export const DEFAULT_OFFER_DESCRIPTION =
  `${LICENSE_CLAUSE_CAP} — the signed packages are free ` +
  `to install and run. Support plans are sold separately; the software and its signed packages are the same for everyone: ${SUPPORT_URL}`;

// Return policy intentionally omitted from structured data — /terms/ grants no
// unconditional money-back guarantee; the EU 14-day withdrawal right for a
// service is conditional: a consumer who asked for it to start within the 14
// days and then withdraws pays for what was provided (Art. 14(3)); it ends on
// full performance (Art. 16(a)). Re-add a
// MerchantReturnPolicy ONLY if a real 14-day money-back guarantee is adopted and
// stated on /terms/ + /pricing/ (owner decision).

export interface OfferOptions {
  /** Canonical URL for this offer (defaults to the pricing/support page). */
  url?: string;
  /** Human-readable offer description (defaults to the license line). */
  description?: string;
}

/**
 * Build the canonical schema.org Offer node: the standard packages are free (€0).
 * Pass `url`/`description` to keep a page's existing values; everything else
 * (price, currency, priceValidUntil, availability) comes from the shared
 * constants above.
 */
export function offerJsonLd(opts: OfferOptions = {}) {
  return {
    '@type': 'Offer',
    availability: 'https://schema.org/InStock',
    price: 0,
    priceCurrency: 'EUR',
    priceValidUntil: PRICE_VALID_UNTIL,
    url: opts.url ?? 'https://modpagespeed.com/pricing/',
    description: opts.description ?? DEFAULT_OFFER_DESCRIPTION,
  };
}

/**
 * One priced Offer per support tier (euro, per year, excl. VAT), built from
 * SUPPORT_TIERS so the markup never states a price the page does not.
 * Enterprise is a "from" price, said in its description.
 */
export function supportTierOffersJsonLd(opts: { url?: string } = {}) {
  const pageUrl = opts.url ?? 'https://modpagespeed.com/pricing/';
  return SUPPORT_TIERS.map((t) => ({
    '@type': 'Offer',
    name: t.name,
    availability: 'https://schema.org/InStock',
    // Every Offer carries `price` (Google's Product checks require it). A
    // "from" price is a minimum: it says so in priceSpecification.minPrice
    // and in the description, never as a flat price on its own.
    price: t.prices.annualEur,
    priceCurrency: 'EUR',
    ...(t.prices.from
      ? {
          priceSpecification: {
            '@type': 'PriceSpecification',
            minPrice: t.prices.annualEur,
            priceCurrency: 'EUR',
          },
        }
      : {}),
    priceValidUntil: PRICE_VALID_UNTIL,
    url: t.cta.external ? t.cta.href : `${pageUrl}#tiers`,
    description:
      `${t.name}, ${t.scope}: ${t.prices.from ? 'from ' : ''}${formatEur(t.prices.annualEur)} ` +
      'per year, billed annually, excl. VAT.',
  }));
}
