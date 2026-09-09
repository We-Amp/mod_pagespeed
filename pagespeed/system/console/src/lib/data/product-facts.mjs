// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// product-facts.mjs — SINGLE SOURCE OF TRUTH for drift-prone product facts.
//
// WHY THIS FILE EXISTS
//   Prices, the launch-promo end date, version numbers, the bundled-nginx
//   version, the variant count and the BuiltWith heritage number used to be
//   hand-duplicated across src/pages/api/product.json.ts,
//   src/data/offer-jsonld.ts, src/data/launch-promo.ts and the two
//   public/llms*.txt files. Every release that touched one of them risked
//   leaving the others stale (the 1.1->1.15 renumber drifted across these
//   files before it was caught by hand).
//
// HOW DRIFT IS NOW PREVENTED
//   - product.json.ts / offer-jsonld.ts / launch-promo.ts IMPORT from here, so
//     there is exactly one copy of each value at compile time.
//   - public/llms.txt and public/llms-full.txt are GENERATED at prebuild by
//     scripts/generate-llms.mjs, which substitutes LLMS_TOKENS (below) into
//     scripts/llms-templates/*.tmpl. Hand-editing the generated .txt or
//     changing a fact here without regenerating fails the byte-equivalence
//     drift guard in test/sync/llms-generated.test.ts.
//
// This is a .mjs (not .ts) so the node prebuild generator can import it
// directly AND the Astro/Vite TypeScript consumers can too (tsconfig has
// allowJs). Keep it dependency-free and side-effect-free.

// --- Vendor / company -------------------------------------------------------
export const VENDOR = 'We-Amp B.V.';
export const COMPANY_COUNTRY = 'The Netherlands';
export const COMPANY_FOUNDED_YEAR = 2012;
export const COMPANY_KVK = '57898138';
export const WEBSITE = 'https://modpagespeed.com';
// The vendor's corporate site (license terms live there — see
// LICENSING_TERMS_URL below).
export const VENDOR_URL = 'https://we-amp.com/';

// --- Product identity -------------------------------------------------------
// The one-file rename point: every name-bearing surface — website
// copy, package descriptions, the web console's chrome — derives the product
// name from these two constants, so the converged-product identity change is
// an edit HERE and nowhere else. PRODUCT_NAME is the lowercase engine name as
// it appears in directives and prose; PRODUCT_DISPLAY_NAME is the camel-cased
// marketing form.
export const PRODUCT_NAME = 'mod_pagespeed';
export const PRODUCT_DISPLAY_NAME = 'ModPageSpeed';

// --- Canonical site URLs ----------------------------------------------------
// Derived from WEBSITE so a domain change flows to every link; the trailing
// slash is canonical (same rule as LICENSING_TERMS_URL below). SUPPORT_URL is
// the pricing page, which presents the subscription tiers and their support
// levels (the ladder below) — there is no separate support page.
export const PRIVACY_URL = `${WEBSITE}/privacy/`;
export const TERMS_URL = `${WEBSITE}/terms/`;
export const SUPPORT_URL = `${WEBSITE}/pricing/`;

// --- Pricing (USD, per-site ladder) ------------------------------------------
// One ladder, all engines, licensed per site. PRICING_TIERS below is the
// canonical shape; every pricing surface derives from it. The pre-2026-06
// per-server prices (long retired) are retired — nothing may
// display them, and the unit prose is 'site', never 'server'.
export const PRICE_UNIT = 'site'; // a license covers a site, never a server
// Canonical license-terms URL. The trailing slash is canonical.
export const LICENSING_TERMS_URL = 'https://we-amp.com/licensing/';

// Flagship (Business) prices, exported flat so pre-ladder importers
// (offer-jsonld.ts, product.json.ts) keep working. Single copy: the Business
// ladder row below references these constants; a drift test asserts equality.
export const PRICE_MONTHLY_USD = 0; // Business, per site, monthly billing
export const PRICE_ANNUAL_USD = 0; // Business, per site, annual billing

// The six ladder rows, in display order. Uniform keys per row:
//   id            stable machine id (matches the FastSpring plan prefix where
//                 one exists: starter-site-*, business-site-*)
//   name          display name — Community is '$0' / 'self-attested', NEVER
//                 'Free' in a heading
//   flagship      exactly one row is the flagship (Business)
//   unit          what one license covers ('site'; 'host' for Hoster)
//   prices        { annualUsd, monthlyUsd } — null = not offered on that cycle
//   priceQualifier '' | 'from' (floor) | 'about' (anchor, billed manually)
//   note          the tier's fence, one short clause; no double quotes (rows
//                 are substituted into ai-plugin.json via PRICING_LADDER_LINE)
export const PRICING_TIERS = [
  {
    id: 'unlicensed',
    name: 'Unlicensed',
    flagship: false,
    unit: 'site',
    prices: { annualUsd: null, monthlyUsd: null },
    priceQualifier: '',
    note: 'no signup, no support subscription',
  },
  {
    id: 'community',
    name: 'Community',
    flagship: false,
    unit: 'site',
    prices: { annualUsd: 0, monthlyUsd: null },
    priceQualifier: '',
    note: 'self-attested, renew every 365 days',
  },
  {
    id: 'starter',
    name: 'Starter',
    flagship: false,
    unit: 'site',
    prices: { annualUsd: 0, monthlyUsd: 0 },
    priceQualifier: '',
    note: 'one site, up to two servers',
  },
  {
    id: 'business',
    name: 'Business',
    flagship: true,
    unit: 'site',
    prices: { annualUsd: PRICE_ANNUAL_USD, monthlyUsd: PRICE_MONTHLY_USD },
    priceQualifier: '',
    note: 'unlimited servers per site',
  },
  {
    id: 'enterprise',
    name: 'Enterprise',
    flagship: false,
    unit: 'site',
    prices: { annualUsd: 0, monthlyUsd: null },
    priceQualifier: 'from',
    note: 'organization-wide: unlimited sites, servers, and engines',
  },
  {
    id: 'hoster',
    name: 'Hoster',
    flagship: false,
    unit: 'host',
    prices: { annualUsd: null, monthlyUsd: 0 },
    priceQualifier: 'about',
    note: 'for hosting providers, billed per host, contact sales',
  },
];

// PURITY RULE FOR THIS FILE
//   Every top-level derivation below is either a plain literal or a call
//   annotated /* @__PURE__ */ whose arguments are bare identifiers. Downstream
//   bundlers (the 1.15 web console imports only names and URLs from this file)
//   can then drop the whole pricing / apt-matrix block as dead code instead of
//   embedding the ladder literals. Keep it that way: no bare
//   `X.map(...).join(...)` or `Object.fromEntries(X.map(...))` at top level —
//   wrap the derivation in a helper and call it through the annotation.

// id -> row lookup, e.g. TIERS.business.prices.annualUsd.
const byId = (rows) => Object.fromEntries(rows.map((r) => [r.id, r]));
export const TIERS = /* @__PURE__ */ byId(PRICING_TIERS);

const usd = (n) => '$' + n.toLocaleString('en-US');
/** Render one row's price label, derived from the configured tier values. */
function tierPriceLabel(t) {
  const q = t.priceQualifier ? `${t.priceQualifier} ` : '';
  const { annualUsd, monthlyUsd } = t.prices;
  if (annualUsd != null && monthlyUsd != null) {
    return `${q}${usd(annualUsd)}/year or ${usd(monthlyUsd)}/month`;
  }
  if (annualUsd != null) return annualUsd === 0 ? '$0' : `${q}${usd(annualUsd)}/year`;
  if (monthlyUsd != null) return `${q}${usd(monthlyUsd)}/${t.unit}/month`;
  return '$0';
}
// id -> price label, derived — never restate a ladder number by hand.
const priceLabels = (rows) => Object.fromEntries(rows.map((t) => [t.id, tierPriceLabel(t)]));
export const TIER_PRICE_LABELS = /* @__PURE__ */ priceLabels(PRICING_TIERS);

// The ladder as markdown bullets (llms*.txt) and as one line (ai-plugin.json).
// Both derive from PRICING_TIERS, so a tier change flows to every surface.
const ladderMd = (rows, labels) =>
  rows.map((t) => `- ${t.name} — ${labels[t.id]}: ${t.note}`).join('\n');
const ladderLine = (rows, labels) =>
  rows.map((t) => `${t.name} ${labels[t.id]} (${t.note})`).join('; ');
export const PRICING_LADDER_MD = /* @__PURE__ */ ladderMd(PRICING_TIERS, TIER_PRICE_LABELS);
export const PRICING_LADDER_LINE = /* @__PURE__ */ ladderLine(PRICING_TIERS, TIER_PRICE_LABELS);

// Business annual vs 12 monthly cycles (derived).
const savingsPct = (annual, monthly) => Math.round((1 - annual / (12 * monthly)) * 100);
export const ANNUAL_SAVINGS_PCT = /* @__PURE__ */ savingsPct(PRICE_ANNUAL_USD, PRICE_MONTHLY_USD);
// Google-recommended Offer freshness date; bump ~yearly (see offer-jsonld.ts).
export const PRICE_VALID_UNTIL = '2026-12-31';

// --- Launch promo -----------------------------------------------------------
// Canonical promo facts. launch-promo.ts re-exports this as LAUNCH_PROMO so the
// ~6 existing page consumers are unchanged.
export const PROMO = {
  // The ladder launched at list price — no promo carryover. The
  // legacy discount promo died with the per-server SKUs; never re-activate
  // without new SKU-side discounts to back it (checkout must grant what
  // banners promise).
  active: false,
  endDate: '2026-06-16',
  endDateDisplay: 'June 16, 2026',
  percentOff: 0,
  // Discount year one: the first 12 monthly cycles, or the first annual cycle.
  cycles: 12,
};

// --- Heritage (BuiltWith) ---------------------------------------------------
// Display strings (locale-independent) — these are facts as published, not
// computed, so store the rendered form to keep generation deterministic.
export const BUILTWITH_SITES = '231,341';
export const BUILTWITH_AS_OF = 'May 2026';
export const BUILTWITH_HISTORICAL = '1.9 million+';

// --- Image pipeline (ModPageSpeed 2.0) --------------------------------------
export const MAX_VARIANTS = 37; // 36 raster + 1 SVG
export const RASTER_VARIANTS = 36; // 3 formats x 3 viewports x 2 densities x 2 Save-Data
// IMAGE_FORMATS (the flat 2.0 format list) is DERIVED from IMAGE_FORMAT_SUPPORT
// in the "Image format support" section below — it has to sit after V1_LINE.

// --- Versions ---------------------------------------------------------------
// The 1.15 line: the maintained continuation of Google's open-source
// mod_pagespeed, renumbered from 1.1 (forward-semver successor to the last
// upstream release, 1.14.36.1 — the final Apache-incubator release; Google's
// last stable was 1.13.35.2). NOTE: this is the marketing LINE version that appears in
// human-facing copy — it is NOT the per-release semver (that lives in the
// release manifests, releases/{1.1,2.0}.yaml). /1.1/ URL paths are
// intentionally frozen and are NOT derived from V1_LINE.
export const V1_LINE = '1.15';
export const V1_RENUMBERED_FROM = '1.1';
// 1.14.36.1 was the final UPSTREAM release — the one Apache-incubator
// (incubating) release, Aug 2020. It was NOT a Google release; Google's last
// stable was 1.13.35.2 (Feb 2018). Named "upstream" (not "Google") accordingly.
export const LAST_UPSTREAM_VERSION = '1.14.36.1';
// ModPageSpeed 2.0 GA date (the ASP.NET Core middleware went GA the same day).
export const V2_GA_DATE = '2026-05-17';
// The 2.0 marketing LINE label, the counterpart of V1_LINE. Use these two
// constants as the edition keys everywhere — never a bare string literal, and
// never the frozen /1.1/ URL path.
export const V2_LINE = '2.0';

// --- Image format support (format-first, edition-keyed, port-scoped) --------
//
// CANONICAL SOURCE OF TRUTH for "which image format does which product edition
// encode, on which ports". Every image-format claim on the site — copy, table
// cell, JSON API, llms.txt — must trace back to this table.
//
// READ BEFORE EDITING
//
//   1. AN ABSENT PORT IS A PORT WE DO NOT CLAIM. This is an ALLOWLIST of
//      VERIFIED support, never a best-effort sketch. Adding a port here
//      licenses every downstream surface to claim it, so only add one after
//      verifying it against the product source.
//
//   2. ENVOY IS DELIBERATELY ABSENT FROM EVERY ROW. The 1.15 Envoy port is not
//      shipped, is excluded from CI, and its AVIF status is UNVERIFIED. Envoy
//      must never be claimed — or implied — to encode AVIF. A guard test in
//      test/content-accuracy.test.ts asserts that 'Envoy' appears in no port
//      list here. Do NOT "fix" that test by adding Envoy to this table.
//
//   3. EDITIONS ARE THE MARKETING LINE LABELS (V1_LINE = '1.15', V2_LINE =
//      '2.0'), consistent with how V1_LINE is defined above — NOT the frozen
//      /1.1/ URL path, and NOT a per-release semver.
//
//   4. RENDERING RULE (approved editorial policy, the optimizer line): rendered marketing copy
//      fixes EDITION scoping ONLY. Ports are modelled here because the model
//      must be precise, but NO helper below emits an edition label glued to a
//      port list, and no copy may render ports, opt-in caveats, Accept-header
//      notes, or Experimental labels. editionClause() is the sanctioned copy
//      helper and never emits a port. Ports stay separately retrievable via
//      portsFor() for non-copy consumers (the JSON API) only.
//
// FACTS ENCODED HERE (verified 2026-07):
//   - mod_pagespeed 1.15 ships AVIF encoding on Apache, nginx (standard and
//     lite) and the native IIS module; the AV1 encoder is statically linked.
//     AVIF is OPT-IN: its four filters sit outside rewrite_images and outside
//     CoreFilters. The IIS module is x64-only and labelled Experimental. None
//     of that nuance is rendered into copy — see rule 4.
//   - ModPageSpeed 2.0 has AVIF in the nginx worker and the ASP.NET Core
//     middleware.
//   - SVG auto-vectorization stays 2.0-only (as do Jpegli and ML-predicted
//     quality, which are not image FORMATS and so are not modelled here).
//   - Ships on BOTH lines, never fence to 2.0: variant-aware caching and
//     zero-copy serving. 1.15 varies its cache on client capability
//     (image format, mobile UA, Save-Data, small-screen) and ships zero-copy
//     serving on nginx, Apache and IIS as of v1.15.0+r19, opt-in via
//     CycloneZeroCopy. What IS 2.0-only is narrower: tablet/desktop viewport
//     classes, pixel density, transfer-encoding alternates, and proactive
//     generation of the full variant matrix. This comment previously made the
//     wrong claim and no guard caught it -- see rule f3.
// The rows below reference these lists directly (no spread copy): a spread is a
// side effect to bundlers and would pin the whole table into consumers that
// import only names and URLs. portsFor() copies on read, so nothing shares a
// mutable array with a caller.
const PORTS_1_15 = ['Apache', 'nginx', 'IIS']; // no Envoy — see rule 2 above
const PORTS_2_0 = ['nginx', 'ASP.NET Core'];

export const IMAGE_FORMAT_SUPPORT = [
  {
    format: 'WebP',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
    },
  },
  {
    format: 'AVIF',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
    },
  },
  {
    format: 'SVG',
    editions: {
      [V2_LINE]: { ports: PORTS_2_0 },
    },
  },
];

// format -> row lookup, e.g. FORMAT_SUPPORT.AVIF.editions['1.15'].ports.
const byFormat = (rows) => Object.fromEntries(rows.map((r) => [r.format, r]));
export const FORMAT_SUPPORT = /* @__PURE__ */ byFormat(IMAGE_FORMAT_SUPPORT);

/** Edition labels that encode `format`, in table order. e.g. ['1.15', '2.0']. */
export function editionsFor(format) {
  return Object.keys(FORMAT_SUPPORT[format]?.editions ?? {});
}
/** Formats `edition` encodes, in table order. e.g. formatsFor('2.0') -> WebP, AVIF, SVG. */
export function formatsFor(edition) {
  return IMAGE_FORMAT_SUPPORT.filter((r) => edition in r.editions).map((r) => r.format);
}
/** Ports of `edition` that encode `format`. NOT for rendered copy — see rule 4. */
export function portsFor(format, edition) {
  return [...(FORMAT_SUPPORT[format]?.editions?.[edition]?.ports ?? [])];
}

/** 'a', 'a and b', 'a, b, and c'. */
function joinList(items) {
  if (items.length <= 1) return items[0] ?? '';
  if (items.length === 2) return `${items[0]} and ${items[1]}`;
  return `${items.slice(0, -1).join(', ')}, and ${items[items.length - 1]}`;
}

/**
 * Render the edition-scoping clause the vs/* comparison rows need. Formats
 * that share an identical edition set COLLAPSE into one group, so the clause
 * reads naturally whether or not the formats line up:
 *
 *   editionClause(['WebP', 'AVIF'])         -> 'WebP and AVIF across 1.15 and 2.0'
 *   editionClause(['WebP', 'AVIF', 'SVG'])  -> 'WebP and AVIF across 1.15 and 2.0, SVG in 2.0'
 *   editionClause(['SVG'])                  -> 'SVG in 2.0'
 *
 * Never emits a port (rule 4). Unknown formats are skipped rather than guessed.
 */
export function editionClause(formats = IMAGE_FORMAT_SUPPORT.map((r) => r.format)) {
  const groups = [];
  for (const format of formats) {
    const editions = editionsFor(format);
    if (editions.length === 0) continue; // unknown format — claim nothing
    const key = editions.join('\u001f');
    const group = groups.find((g) => g.key === key);
    if (group) group.formats.push(format);
    else groups.push({ key, editions, formats: [format] });
  }
  return groups
    .map(
      (g) =>
        `${joinList(g.formats)} ${g.editions.length > 1 ? 'across' : 'in'} ${joinList(g.editions)}`,
    )
    .join(', ');
}

// BACK-COMPAT: the flat 2.0 format list, unchanged in name, shape and value
// (['WebP', 'AVIF', 'SVG']). Its consumers — src/pages/api/product.json.ts and
// src/pages/self-hosted-image-optimization.astro — must render byte-identically.
// Derived, so a table change flows here instead of drifting.
export const IMAGE_FORMATS = /* @__PURE__ */ formatsFor(V2_LINE);

// --- NuGet packages ---------------------------------------------------------
export const PKG_ASPNETCORE = 'WeAmp.PageSpeed.AspNetCore'; // ModPageSpeed 2.0 middleware
export const PKG_SIDECAR = 'WeAmp.PageSpeed.Sidecar'; // mod_pagespeed 1.15 sidecar
export const PKG_SIDECAR_NATIVE = 'WeAmp.PageSpeed.Sidecar.NativeAssets.Linux';
export const SIDECAR_NGINX_VERSION = '1.30.2'; // nginx bundled inside the 1.15 sidecar
export const SIDECAR_RIDS = ['linux-x64', 'linux-arm64'];
export const ASPNETCORE_RIDS = ['linux-x64', 'linux-arm64', 'osx-arm64', 'win-x64'];
// Target frameworks the ModPageSpeed 2.0 ASP.NET Core middleware ships for.
// Authoritative for the .NET runtime requirement (.NET 8 or .NET 10) — NOT .NET 9.
// Mirrors aspnet-getting-started.mdx ("targets net8.0 and net10.0"). Keep this in
// lockstep with the package's <TargetFrameworks>; the content-accuracy guard asserts it.
export const ASPNETCORE_TFMS = ['net8.0', 'net10.0'];

// --- Stock nginx targets for the signed apt/yum 1.15 nginx packages ----------
// The nginx-module-pagespeed dynamic module is prebuilt and signed per distro,
// pinned to that distro's stock nginx. Each .so is exact-version-pinned to its
// distro's nginx (nginx's --with-compat does NOT relax the version check), so
// upgrading nginx past the stock version needs a matching rebuild (contact us).
// amd64 + arm64 for every row.
export const STOCK_NGINX_UBUNTU = '1.24.0'; // Ubuntu 24.04 noble
export const STOCK_NGINX_ALMA = '1.20.1'; // AlmaLinux 9 (rpm/Apache target)

// The signed apt matrix for nginx-module-pagespeed (Debian + Ubuntu), each row
// pinned to the distro's stock nginx. Order is the install-doc display order.
export const NGINX_APT_DISTROS = [
  { distro: 'Debian 11 bullseye', nginx: '1.18.0' },
  { distro: 'Debian 12 bookworm', nginx: '1.22.1' },
  { distro: 'Debian 13 trixie', nginx: '1.26.3' },
  { distro: 'Ubuntu 22.04 jammy', nginx: '1.18.0' },
  { distro: 'Ubuntu 24.04 noble', nginx: STOCK_NGINX_UBUNTU },
];
export const NGINX_APT_ARCHES = 'amd64 + arm64';
// Compact prose listing of the prebuilt apt matrix, e.g.
// "Debian 11 bullseye (nginx 1.18.0), Debian 12 bookworm (nginx 1.22.1), …".
const aptMatrix = (rows) => rows.map((d) => `${d.distro} (nginx ${d.nginx})`).join(', ');
export const NGINX_APT_MATRIX = /* @__PURE__ */ aptMatrix(NGINX_APT_DISTROS);

// --- Software license -------------------------------------------------------
// Single source of truth for the license the software is distributed under,
// mirrored by the machine-readable /api/product.json `license` and
// `source_publication` fields AND the human /license/ page, so those surfaces
// cannot drift on the license name, its SPDX id, or the status. The sync test
// (test/sync/source-publication-sync.test.ts) pins the record and fails if a
// page re-states any of it as a literal.
export const SOURCE_PUBLICATION = {
  status: 'licensed', // flips to 'published' with the commit that adds the public source location
  license: 'Apache License 2.0', // full name (first mention in prose)
  licenseId: 'Apache-2.0', // SPDX id (product.json `license` / `source_publication.license`)
};

/**
 * Token map consumed by scripts/generate-llms.mjs. Every value is the EXACT
 * string that must appear in the generated public/llms*.txt at the
 * corresponding {{TOKEN}} site. Derive from the typed constants above so there
 * is one source — never hardcode a second copy of a number here.
 */
const llmsTokens = () => ({
  PRICE_UNIT,
  PRICING_LADDER_MD,
  PRICING_LADDER_LINE,
  BUSINESS_PRICE_LABEL: TIER_PRICE_LABELS.business,
  BUSINESS_MONTHLY_USD: String(PRICE_MONTHLY_USD),
  STARTER_MONTHLY_USD: String(TIERS.starter.prices.monthlyUsd),
  LICENSING_TERMS_URL,
  PROMO_PCT: String(PROMO.percentOff),
  PROMO_END_HUMAN: PROMO.endDateDisplay,
  MAX_VARIANTS: String(MAX_VARIANTS),
  RASTER_VARIANTS: String(RASTER_VARIANTS),
  SIDECAR_NGINX_VERSION,
  BUILTWITH_SITES,
  BUILTWITH_AS_OF,
  BUILTWITH_HISTORICAL,
  LAST_UPSTREAM_VERSION,
  V1_LINE,
  V1_RENUMBERED_FROM,
  V2_GA_DATE,
  STOCK_NGINX_UBUNTU,
  STOCK_NGINX_ALMA,
  NGINX_APT_MATRIX,
  NGINX_APT_ARCHES,
  COMPANY_KVK,
  COMPANY_FOUNDED_YEAR: String(COMPANY_FOUNDED_YEAR),
});
export const LLMS_TOKENS = /* @__PURE__ */ llmsTokens();
