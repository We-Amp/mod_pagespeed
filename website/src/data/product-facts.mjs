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

// --- Vendor / product identity / canonical site URLs ------------------------
// The identity facts the admin console also consumes — vendor and product
// names, the canonical site URLs — are DEFINED ONCE in
// ../../../shared/product-facts.mjs (the file the console syncs verbatim) and
// re-exported here unchanged, so every website importer keeps reading them
// from this module. test/sync/console-facts-sync.test.ts asserts the lockstep.
// Edit the shared file; never re-declare one of these names here.
// The import binds the names this file's own derivations (LLMS_TOKENS below)
// reference; the re-export keeps every website importer reading them from
// this module — `export { … } from` alone creates no local binding.
import { PRODUCT_NAME, SUPPORT_URL } from '../../../shared/product-facts.mjs';
export {
  VENDOR,
  WEBSITE,
  VENDOR_URL,
  PRODUCT_NAME,
  PRODUCT_DISPLAY_NAME,
  PRIVACY_URL,
  TERMS_URL,
  SUPPORT_URL,
} from '../../../shared/product-facts.mjs';
export const COMPANY_COUNTRY = 'The Netherlands';
export const COMPANY_FOUNDED_YEAR = 2012;
export const COMPANY_KVK = '57898138';

// --- License terms -----------------------------------------------------------
// Canonical support-terms URL. The trailing slash is canonical.
export const LICENSING_TERMS_URL = 'https://we-amp.com/licensing/';

// --- Commercial contact -------------------------------------------------------
// The ONE address the site gives for anything commercial: support
// subscriptions, the hosting partner program, consulting,
// IISpeed transfers. Security disclosure keeps its own address
// (security@modpagespeed.com) and is never routed here.
export const COMMERCIAL_EMAIL = 'info@we-amp.com';

// --- Offerings (converged line) -----------------------------------------------
// What is for sale is support, in three tiers. The software is free and identical under every tier. These
// rows are what /support/ and /pricing/ present and what the agent files and
// /api/product.json describe. Prices are in euro, per year, excl. VAT
// (prices are listed in EUR; checkout runs on FastSpring); the first-response target
// of each tier is the last entry of its `includes`.
//   scope       what the tier covers: people and response, never a count of
//               servers or installations
//   prices      annualEur is the yearly price; from marks "from" pricing;
//               monthlyEur stays null (annual billing only)
//   includes    what the tier adds, in display order; the first entry of a
//               higher tier names the tier it builds on
//   hardened    always false: there are no subscriber-only builds; the
//               software and its signed packages are the same for everyone
//   cta         where the tier's button goes: a hosted checkout page for
//               Standard and Priority, the contact form for Enterprise;
//               offer is the umami cta_commercial offer value
export const SUPPORT_TIERS = [
  {
    id: 'standard',
    name: 'Standard support',
    kind: 'support',
    hardened: false,
    scope: 'one organization, 1 person',
    prices: { annualEur: 490, from: false, monthlyEur: null },
    cta: {
      href: 'https://mpspro.onfastspring.com/support-standard-annual',
      label: 'Buy Standard',
      offer: 'buy-standard',
      external: true,
    },
    note: 'a direct channel to the engineers who build the product, for one organization and one person at your organization who can open tickets',
    includes: [
      'Ticket and email channel to the engineers who build the product',
      'Configuration, upgrade and incident help',
      'Advance notice of security releases',
      'One onboarding configuration review',
      'First response within 2 business days',
    ],
  },
  {
    id: 'priority',
    name: 'Priority support',
    kind: 'support',
    hardened: false,
    scope: 'one organization, up to 3 people',
    prices: { annualEur: 1900, from: false, monthlyEur: null },
    cta: {
      href: 'https://mpspro.onfastspring.com/support-priority-annual',
      label: 'Buy Priority',
      offer: 'buy-priority',
      external: true,
    },
    note: 'everything in Standard plus vulnerability notifications timed to the EU Cyber Resilience Act reporting windows, for one organization and up to 3 people at it who can open tickets, with faster first-response targets',
    includes: [
      'Everything in Standard',
      'Vulnerability notifications timed to the EU Cyber Resilience Act reporting windows',
      'Up to 3 people at your organization who can open tickets',
      'Upgrade rehearsal on request',
      'Quarterly review call',
      'First response next business day; severity 1 (production down) within 8 business hours',
    ],
  },
  {
    id: 'enterprise',
    name: 'Enterprise',
    kind: 'support',
    hardened: false,
    scope: 'one organization, people as agreed',
    prices: { annualEur: 7500, from: true, monthlyEur: null },
    cta: {
      href: '/contact/?topic=enterprise',
      label: 'Talk to us about Enterprise',
      offer: 'enterprise',
      external: false,
    },
    note: 'everything in Priority plus a named engineer, response targets per agreement, custom build targets and backports, for one organization and the people at it agreed in your order',
    includes: [
      'Everything in Priority',
      'A named engineer',
      'People at your organization who can open tickets, as agreed',
      'Custom build targets (distributions, nginx versions), published for everyone',
      'Backports, published for everyone',
      'Master agreement, purchase order and invoice',
      'Roadmap input and consulting days',
      'Severity 1 (production down) within 4 business hours; other severities per agreement',
    ],
  },
];
// The line under the tier cards: what is signed and attested is not a tier
// feature; security fixes reach everyone at the same time.
export const SAME_SOFTWARE_LINE =
  'We sell support; the software and its signed packages are the same for everyone, with or without a subscription, on every server. Security fixes reach every user at the same time. A build or backport made for an Enterprise customer is published for everyone when it is delivered.';
// The pricing terms, said the same way everywhere they are said.
export const PRICING_TERMS_LINE = 'Prices are per year, billed annually, excl. VAT.';
export const PRICE_HOLD_LINE =
  'The price you sign at holds for one year. Existing customers keep their current rate; nobody is repriced mid-term.';
export const PRICING_LINE = `${PRICING_TERMS_LINE} ${PRICE_HOLD_LINE}`;
export const RESPONSE_TARGETS_LINE =
  'Response targets are first-response times. Support runs on CET business days, and business hours are CET.';
// The fixed-price agent-readiness audit (one site, one report, one review call).
export const AUDIT_PRICE_EUR = 950;
// Euro amounts as displayed on the site: a symbol and thousands separators.
export const formatEur = (n) => `\u20ac${String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',')}`;
// The artifact axis: the standard signed packages are free for everyone.
// What ships today for everyone:
// GPG-signed apt/yum repositories, a GPG-signed SHA256SUMS (SHA256SUMS.asc) on
// the module's GitHub releases from 2.5.0 on (no per-file .asc; the optimizer's
// GitHub release assets carry no signature, only SHA256SUMS), the
// Authenticode-signed Windows installer, cosign-signed container images with
// an SBOM attestation and build provenance, and the source-tree SPDX SBOM and
// OpenVEX file; every release from 2.6.0 on also carries its SPDX SBOM and
// VEX file, for everyone. No surface may claim provenance on packages. There is no private or subscriber package repository: security
// fixes ship to everyone at once through the public channels.
// Signing keys. Each entry copies its fields verbatim from the internal
// signing-key register, the source of truth for keys (id, name, status,
// purpose, fingerprint, public_url), so this block can later be generated
// from it. Change the register first, then this block.
// Fingerprints are the full 40 hex digits, never a short key id; show them
// with formatFingerprint(). Label each key by what it signs wherever a page
// shows one.
export const REPOSITORY_KEY = {
  id: 'repository',
  name: 'We-Amp Repository Signing Key',
  status: 'active',
  purpose:
    'Signs the apt and yum repositories at packages.modpagespeed.com and the rpm headers in them. Nothing else.',
  fingerprint: 'DF00E296BE9141CEA5411346F50D6054F10712A0',
  public_url: 'https://packages.modpagespeed.com/pubkey.gpg',
};
export const RELEASE_KEY = {
  id: 'release',
  name: 'We-Amp Release Signing Key',
  status: 'active',
  purpose: 'Signs SHA256SUMS.asc of GitHub releases and signed source tags. Nothing else.',
  fingerprint: '411DEFDB571351D2B25AEFCC9C02B5BDCCE75313',
  public_url:
    'https://raw.githubusercontent.com/We-Amp/mod_pagespeed/master/RELEASE-SIGNING-KEY.asc',
};
// Page links for the release key (not register fields): the key file as a
// GitHub page and the verification steps in SECURITY.md.
export const RELEASE_KEY_PAGE_URL =
  'https://github.com/We-Amp/mod_pagespeed/blob/master/RELEASE-SIGNING-KEY.asc';
export const RELEASE_VERIFY_URL =
  'https://github.com/We-Amp/mod_pagespeed/blob/master/SECURITY.md#verifying-a-release';
// The 1.x key: no longer used, being revoked (status retiring; the revocation
// is not published yet). Pages name it only as that, never link it, and never
// offer it as a current key, although the register still records its
// public_url.
export const LEGACY_1X_KEY = {
  id: 'legacy-1x',
  name: '1.x direct-download key',
  status: 'retiring',
  purpose:
    'None from 2026-10-20. Signed the 1.x direct downloads (per-file .asc) and the source tags v2.1.0 to v2.4.0.',
  fingerprint: 'A358270318045B75C552FEA8DA8FEBD5000BC194',
  public_url: 'https://modpagespeed.com/releases/v1.1.0/weamp-pkg-public.asc',
};
export const SIGNING_KEYS = [REPOSITORY_KEY, RELEASE_KEY, LEGACY_1X_KEY];
// 'DF00E296...' -> 'DF00 E296 BE91 41CE A541  1346 F50D 6054 F107 12A0' (gpg's own layout).
export const formatFingerprint = (f) => {
  const g = f.match(/.{4}/g);
  return `${g.slice(0, 5).join(' ')}  ${g.slice(5).join(' ')}`;
};
export const ARTIFACT_ACCESS = {
  standardPackages: 'free',
  hardenedBuilds: 'not-offered',
  hardenedPricing: 'not-offered',
  privateRepository: 'not-offered',
};
// The support ladder as markdown bullets and one line, with each tier's yearly
// price (pure helpers — see the purity rule below).
const tierPrice = (t) => `${t.prices.from ? 'from ' : ''}${formatEur(t.prices.annualEur)} a year`;
// A tier's first-response target: by convention the last entry of `includes`,
// which is what the tier cards show. Pages that state the target outside the
// cards (the terms of service) read it through this helper.
export const tierTarget = (t) => t.includes[t.includes.length - 1];
const ladderMd = (rows) =>
  rows.map((t) => `- ${t.name} — ${t.note}; ${tierPrice(t)}; ${tierTarget(t)}`).join('\n');
const ladderLine = (rows) =>
  rows.map((t) => `${t.name} (${t.note}; ${tierPrice(t)}; ${tierTarget(t)})`).join('; ');
export const SUPPORT_LADDER_MD = /* @__PURE__ */ ladderMd(SUPPORT_TIERS);
export const SUPPORT_LADDER_LINE = /* @__PURE__ */ ladderLine(SUPPORT_TIERS);

// PURITY RULE FOR THIS FILE
//   Every top-level derivation below is either a plain literal or a call
//   annotated /* @__PURE__ */ whose arguments are bare identifiers. Downstream
//   bundlers (the 1.15 web console imports only names and URLs from this file)
//   can then drop the whole support-tier / apt-matrix block as dead code
//   instead of embedding the literals. Keep it that way: no bare
//   `X.map(...).join(...)` or `Object.fromEntries(X.map(...))` at top level —
//   wrap the derivation in a helper and call it through the annotation.

// Google-recommended Offer freshness date; bump ~yearly (see offer-jsonld.ts).
export const PRICE_VALID_UNTIL = '2027-12-31';

// --- Heritage (BuiltWith) ---------------------------------------------------
// Display strings (locale-independent) — these are facts as published, not
// computed, so store the rendered form to keep generation deterministic.
export const BUILTWITH_SITES = '231,341';
export const BUILTWITH_AS_OF = 'May 2026';
export const BUILTWITH_HISTORICAL = '1.9 million+';

// --- Image pipeline (optimizer worker) ---------------------------------------
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
// The 2.0 marketing LINE label, the counterpart of V1_LINE. Use the line
// constants as the edition keys everywhere — never a bare string literal, and
// never the frozen /1.1/ URL path. CURRENT_LINE is the converged line every
// current-product surface derives from, the ASP.NET Core middleware included.
export const V2_LINE = '2.0';
export const CURRENT_LINE = '2.1';

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
//   4. RENDERING RULE (approved editorial policy, mps2 #1000): rendered marketing copy
//      fixes EDITION scoping ONLY. Ports are modelled here because the model
//      must be precise, but NO helper below emits an edition label glued to a
//      port list, and no copy may render ports, opt-in caveats, Accept-header
//      notes, or Experimental labels. editionClause() is the sanctioned copy
//      helper and never emits a port. Ports stay separately retrievable via
//      portsFor() for non-copy consumers (the JSON API) only.
//
// FACTS ENCODED HERE (verified 2026-07):
//   - The 1.15 line ships AVIF encoding on Apache, nginx (standard and
//     lite) and the native IIS module; the AV1 encoder is statically linked.
//     AVIF is OPT-IN: its four filters sit outside rewrite_images and outside
//     CoreFilters. The IIS module is x64-only and labelled Experimental. None
//     of that nuance is rendered into copy — see rule 4.
//   - The 2.0 line has AVIF in the nginx worker and the ASP.NET Core
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
// Converged 2.1: the module ships on Apache and nginx; WebP + AVIF encode in
// the module (carried from the 1.x line). SVG auto-vectorization runs in the
// optimizer worker, whose module wiring is Apache-first — VERIFY the SVG port
// list at the GA pin before this ships (parked-PR checklist item).
const PORTS_2_1 = ['Apache', 'nginx'];
const PORTS_2_1_WORKER = ['Apache'];

export const IMAGE_FORMAT_SUPPORT = [
  {
    format: 'WebP',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1 },
    },
  },
  {
    format: 'AVIF',
    editions: {
      [V1_LINE]: { ports: PORTS_1_15 },
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1 },
    },
  },
  {
    format: 'SVG',
    editions: {
      [V2_LINE]: { ports: PORTS_2_0 },
      [CURRENT_LINE]: { ports: PORTS_2_1_WORKER },
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
 * Render the image-format clause the vs/* comparison rows need: the bare list
 * of formats the converged line encodes, with no line enumeration:
 *
 *   editionClause(['WebP', 'AVIF'])         -> 'WebP and AVIF'
 *   editionClause(['WebP', 'AVIF', 'SVG'])  -> 'WebP, AVIF, and SVG'
 *   editionClause(['SVG'])                  -> 'SVG'
 *
 * Never emits a port or an edition label (rule 4). A format the converged line
 * does not encode, and an unknown format, are skipped rather than guessed. The
 * edition/port model stays retrievable for non-copy consumers (the JSON API)
 * through editionsFor() and portsFor(), whose shapes do not move.
 */
export function editionClause(formats = IMAGE_FORMAT_SUPPORT.map((r) => r.format)) {
  return joinList(formats.filter((format) => editionsFor(format).includes(CURRENT_LINE)));
}

// BACK-COMPAT: the flat format list, unchanged in name, shape and value
// (['WebP', 'AVIF', 'SVG']). Its consumers — src/pages/api/product.json.ts and
// src/pages/self-hosted-image-optimization.astro — must render byte-identically.
// Derived from the converged line; the value is identical to the old
// 2.0-derived list, so the public contract does not move.
export const IMAGE_FORMATS = /* @__PURE__ */ formatsFor(CURRENT_LINE);

// --- Capability support (the non-format facts the exclusivity rules guard) ----
//
// The machine-checkable counterpart of IMAGE_FORMAT_SUPPORT for the capabilities
// that copy keeps trying to fence to 2.0. test/content-accuracy.test.ts ties
// each exclusivity rule to one of these rows (its "anchor"), so a change to a
// fact fails the test that names it instead of leaving a rule enforcing a fact
// that stopped being true. Same allowlist discipline as the format table: an
// absent edition is an edition we do not claim, and Envoy never appears.
//
// Editions are the marketing line labels (V1_LINE, V2_LINE), as in the format
// table. Ports are listed only where the product source states them.
export const CAPABILITY_SUPPORT = [
  {
    // Zero-copy serving from the Cyclone cache, opt-in via CycloneZeroCopy /
    // CycloneZeroCopyServe; on the 1.15 line as of v1.15.0+r19.
    capability: 'zero-copy-serving',
    editions: {
      [V1_LINE]: { ports: ['nginx', 'Apache', 'IIS'] },
      [V2_LINE]: {},
    },
  },
  {
    // The cache varies on client capability: image format, mobile UA,
    // Save-Data and small-screen on the 1.15 line.
    capability: 'variant-aware-caching',
    editions: {
      [V1_LINE]: {},
      [V2_LINE]: {},
    },
  },
];

/** Edition labels that carry `capability`, in table order. */
export function capabilityEditions(capability) {
  const row = CAPABILITY_SUPPORT.find((r) => r.capability === capability);
  return Object.keys(row?.editions ?? {});
}

// What IS 2.0-only is narrower than the categories above. Copy may fence these
// to 2.0, and the exclusivity rules deliberately do not flag a sentence that
// names one of them. Each entry is a plain-language label, not a regex.
export const V2_ONLY_PROPERTIES = [
  'tablet and desktop viewport classes',
  'pixel density (Sec-CH-DPR) in the cache key',
  'transfer-encoding alternates',
  'proactive generation of the full variant matrix',
  'SVG auto-vectorization',
  'Jpegli',
  'ML-predicted quality',
];

// --- Native packages (the signed apt/yum repository) -------------------------
// The package names the repository serves; the release manifest carries the
// repository URL and the release version, not these names. The generated
// agent files take them from here.
export const PKG_APACHE_MODULE = 'mod-pagespeed'; // the Apache module
export const PKG_NGINX_MODULE = 'nginx-module-pagespeed'; // the nginx module
export const PKG_OPTIMIZER = 'pagespeed-optimizer'; // the optimizer worker
// The newest published version of the two module packages above, as the
// package index at https://packages.modpagespeed.com lists it (apt
// dists/<suite>/main/binary-*/Packages; the upstream part of the version, so
// 1.17.0 for 1.17.0-r1). The module reports it in its response header
// (X-Mod-Pagespeed on Apache, X-Page-Speed on nginx), which is what the
// homepage quick-start shows. Bump it when new module packages are published;
// test/sync/module-package-version.test.ts compares it with the live index.
export const MODULE_PACKAGE_VERSION = '2.8.2';

// --- NuGet packages ---------------------------------------------------------
export const PKG_ASPNETCORE = 'WeAmp.PageSpeed.AspNetCore'; // the ASP.NET Core middleware
export const PKG_SIDECAR = 'WeAmp.PageSpeed.Sidecar'; // the current module, run behind Kestrel
export const PKG_SIDECAR_NATIVE = 'WeAmp.PageSpeed.Sidecar.NativeAssets.Linux';
// From 2.6.0 on the sidecar packages carry the product version and ship with
// every full release (release-notes-2-6.mdx, release-notes-2-8.mdx), so the
// package version is the release version in the release manifest.
// Its own literal: a module-only release moves MODULE_PACKAGE_VERSION but not the NuGet packages.
export const SIDECAR_VERSION = '2.8.1'; // WeAmp.PageSpeed.Sidecar package version on nuget.org
// nginx bundled in the sidecar native pair: the bundled nginx of the release,
// docker/nginx-version.txt (release-notes-2-5.mdx: 1.30.6 covers the
// container images and the sidecar native pair).
export const SIDECAR_NGINX_VERSION = '1.30.6';
export const SIDECAR_RIDS = ['linux-x64', 'linux-arm64'];
export const ASPNETCORE_RIDS = ['linux-x64', 'linux-arm64', 'osx-arm64', 'win-x64'];
// Target frameworks the ASP.NET Core middleware ships for.
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
// Debian 11 (bullseye) is deliberately absent: the repository's bullseye
// suite tops out at the final 1.15.0 module packages (glibc 2.34, needed by
// the 2.1 serving components, is not available there) — see
// migrating-to-2-1.md's Debian 11 platform note and release-notes.mdx's
// current-release operator note.
export const NGINX_APT_DISTROS = [
  { distro: 'Debian 12 bookworm', nginx: '1.22.1' },
  { distro: 'Debian 13 trixie', nginx: '1.26.3' },
  { distro: 'Ubuntu 22.04 jammy', nginx: '1.18.0' },
  { distro: 'Ubuntu 24.04 noble', nginx: STOCK_NGINX_UBUNTU },
  { distro: 'Ubuntu 26.04 resolute', nginx: '1.28.3' },
];
export const NGINX_APT_ARCHES = 'amd64 + arm64';
// Compact prose listing of the prebuilt apt matrix, e.g.
// "Debian 12 bookworm (nginx 1.22.1), Debian 13 trixie (nginx 1.26.3), …".
const aptMatrix = (rows) => rows.map((d) => `${d.distro} (nginx ${d.nginx})`).join(', ');
export const NGINX_APT_MATRIX = /* @__PURE__ */ aptMatrix(NGINX_APT_DISTROS);

// The Enterprise Linux releases of nginx-module-pagespeed in the signed yum
// repository (release-notes-2-8.mdx, 2.8.0 Packages: x86_64 and aarch64 on 9,
// x86_64 only on 10). `nginx` is the stock nginx the el9 build is pinned to.
export const NGINX_RPM_DISTROS = [
  { release: '9', family: 'el9', arches: 'x86_64 + aarch64', nginx: STOCK_NGINX_ALMA },
  { release: '10', family: 'el10', arches: 'x86_64', nginx: null },
];

// Platform lists for install blurbs, derived from the two tables above so
// every page names the same releases. "Debian 12/13, Ubuntu 22.04/24.04/26.04".
const splitDistro = (row) => {
  const [name, version, codename] = row.distro.split(' ');
  return { name, version, codename };
};
const aptByName = (rows) => {
  /** @type {Map<string, string[]>} */
  const byName = new Map();
  for (const row of rows) {
    const { name, version } = splitDistro(row);
    byName.set(name, [...(byName.get(name) ?? []), version]);
  }
  return [...byName];
};
const joinAnd = (items) =>
  items.length < 2 ? items.join('') : `${items.slice(0, -1).join(', ')} and ${items.at(-1)}`;
const aptShort = (rows, space) =>
  aptByName(rows)
    .map(([name, versions]) => `${name}${space}${versions.join('/')}`)
    .join(', ');
const rpmShort = (rows) => joinAnd(rows.map((r) => `${r.release} (${r.arches})`));
/** "Debian 12/13, Ubuntu 22.04/24.04/26.04" (plain spaces). */
export const NGINX_APT_SHORT = /* @__PURE__ */ aptShort(NGINX_APT_DISTROS, ' ');
/** The same with non-breaking spaces between a distribution and its versions. */
export const NGINX_APT_SHORT_NBSP = /* @__PURE__ */ aptShort(NGINX_APT_DISTROS, '\u00a0');
/** "9 (x86_64 + aarch64) and 10 (x86_64)" */
export const NGINX_RPM_SHORT = /* @__PURE__ */ rpmShort(NGINX_RPM_DISTROS);
/** "Debian 12 and 13 and Ubuntu 22.04, 24.04 and 26.04" */
export const NGINX_APT_PROSE = /* @__PURE__ */ aptByName(NGINX_APT_DISTROS)
  .map(([name, versions]) => `${name} ${joinAnd(versions)}`)
  .join(' and ');
// The Apache module (mod-pagespeed) ships in the same apt suites and EL
// releases as the nginx module (the apt index serves mod-pagespeed in every
// suite of NGINX_APT_DISTROS), so the lists below are shared by both modules.
/** "Debian 12/13, Ubuntu 22.04/24.04/26.04" for either module. */
export const MODULE_APT_SHORT = NGINX_APT_SHORT;
/** One row per supported release of the Apache module, for a bullet list. */
export const APACHE_PLATFORM_ROWS = /* @__PURE__ */ [
  ...NGINX_APT_DISTROS.map((row) => {
    const { name, version, codename } = splitDistro(row);
    return `${name} ${version} (${codename}), ${NGINX_APT_ARCHES}`;
  }),
  ...NGINX_RPM_DISTROS.map(
    (r) =>
      `AlmaLinux ${r.release} / RHEL ${r.release} / Rocky ${r.release} (${r.family}), ${r.arches}`,
  ),
];
/** One row per supported release, for a bullet list. */
export const NGINX_PLATFORM_ROWS = /* @__PURE__ */ [
  ...NGINX_APT_DISTROS.map((row) => {
    const { name, version, codename } = splitDistro(row);
    return `${name} ${version} (${codename}), ${NGINX_APT_ARCHES}, stock nginx ${row.nginx}`;
  }),
  ...NGINX_RPM_DISTROS.map(
    (r) =>
      `AlmaLinux ${r.release} / RHEL ${r.release} / Rocky ${r.release} (${r.family}), ${r.arches}` +
      (r.nginx ? `, stock nginx ${r.nginx}` : ''),
  ),
];

// --- Software license -------------------------------------------------------
// Single source of truth for the license the software is distributed under,
// mirrored by the machine-readable /api/product.json `license` and
// `source_publication` fields AND the human /license/ page, so those surfaces
// cannot drift on the license name, its SPDX id, or the status. The sync test
// (test/sync/source-publication-sync.test.ts) pins the record and fails if a
// page re-states any of it as a literal.
export const SOURCE_PUBLICATION = {
  status: 'published', // source repositories went public 2026-09-16
  license: 'Apache License 2.0', // full name (first mention in prose)
  licenseId: 'Apache-2.0', // SPDX id (product.json `license` / `source_publication.license`)
};
// Publication-derived phrasing. "Licensed under the Apache License 2.0" is
// true today; "open source" is true only once the public repositories exist
// (`status: 'published'`). Copy that asserts either MUST go through these, so
// the claim flips with the fact and never by hand — the sync test scans for
// hard-coded "open source" / "open-source" claims about the current product.
export const SOURCE_IS_PUBLISHED = SOURCE_PUBLICATION.status === 'published';
const licenseClause = (sp, published) =>
  published ? `open source under the ${sp.license}` : `licensed under the ${sp.license}`;
export const LICENSE_CLAUSE = /* @__PURE__ */ licenseClause(
  SOURCE_PUBLICATION,
  SOURCE_IS_PUBLISHED,
);
const capitalize = (s) => s.charAt(0).toUpperCase() + s.slice(1);
export const LICENSE_CLAUSE_CAP = /* @__PURE__ */ capitalize(LICENSE_CLAUSE);
// The one-line CTA footer most pages carry. Derived, so the publication flip
// reaches every page at once.
const freeToRun = (cap) => `${cap} — free in development and in production.`;
export const FREE_TO_RUN_LINE = /* @__PURE__ */ freeToRun(LICENSE_CLAUSE_CAP);

/**
 * Token map consumed by scripts/generate-llms.mjs. Every value is the EXACT
 * string that must appear in the generated public/llms*.txt at the
 * corresponding {{TOKEN}} site. Derive from the typed constants above so there
 * is one source — never hardcode a second copy of a number here.
 */
const llmsTokens = () => ({
  PRODUCT_NAME,
  CURRENT_LINE,
  V2_LINE,
  LICENSE_NAME: SOURCE_PUBLICATION.license,
  LICENSE_ID: SOURCE_PUBLICATION.licenseId,
  LICENSE_CLAUSE,
  LICENSE_CLAUSE_CAP,
  SUPPORT_LADDER_MD,
  SUPPORT_LADDER_LINE,
  SUPPORT_URL,
  LICENSING_TERMS_URL,
  COMMERCIAL_EMAIL,
  PRICING_LINE,
  RESPONSE_TARGETS_LINE,
  AUDIT_PRICE: formatEur(AUDIT_PRICE_EUR),
  MAX_VARIANTS: String(MAX_VARIANTS),
  RASTER_VARIANTS: String(RASTER_VARIANTS),
  SIDECAR_VERSION,
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
