// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// product-facts.mjs — the product-identity facts shared with the admin console.
//
// WHAT LIVES HERE
//   The handful of identity facts every product surface derives its chrome
//   from — vendor and product names, the canonical site URLs — plus the one
//   sentence about how the software is licensed and where support
//   subscriptions are described. Nothing else: no pricing, no plans, no
//   marketing copy. Those stay with the website, which imports and re-exports
//   these constants from website/src/data/product-facts.mjs so there is exactly
//   one definition of each value.
//
// WHY IT IS ITS OWN FILE
//   The admin console is built in the module repository. It copies this file
//   VERBATIM into its own tree and byte-compares the copy against this path on
//   the default branch, so a rename or URL change is an edit here and nowhere
//   else. The console must be able to take the whole file at face value, and
//   it does not build against the website tree, so the console-facing facts
//   live in this dependency-free module rather than inside the website's
//   much larger facts file. Keep it that way: plain ESM, no imports, no side
//   effects, and only the exports listed in
//   website/test/sync/console-facts-sync.test.ts — which also asserts that the
//   website's re-exports stay in lockstep with this file.

// --- Vendor / company -------------------------------------------------------
export const VENDOR = 'We-Amp B.V.';
export const WEBSITE = 'https://modpagespeed.com';
// The vendor's corporate site (the support terms live there — see
// SUPPORT_TERMS_URL below).
export const VENDOR_URL = 'https://we-amp.com/';

// --- Product identity -------------------------------------------------------
// The one-file rename point: every name-bearing surface — website copy,
// package descriptions, the admin console's chrome — derives the product name
// from these two constants, so a product identity change is an edit HERE and
// nowhere else. PRODUCT_NAME is the lowercase engine name as it appears in
// directives and prose; PRODUCT_DISPLAY_NAME is the camel-cased marketing form.
export const PRODUCT_NAME = 'mod_pagespeed';
export const PRODUCT_DISPLAY_NAME = 'ModPageSpeed';

// --- Canonical site URLs ----------------------------------------------------
// Derived from WEBSITE so a domain change flows to every link; the trailing
// slash is canonical. SUPPORT_URL is the page that presents the support
// subscriptions — there is no separate support page.
export const PRIVACY_URL = `${WEBSITE}/privacy/`;
export const TERMS_URL = `${WEBSITE}/terms/`;
export const SUPPORT_URL = `${WEBSITE}/pricing/`;

// --- Open-source posture and support ---------------------------------------
// The one sentence a product surface says about how the software is licensed.
// Render it verbatim; do not paraphrase it.
export const PRODUCT_STATEMENT = 'mod_pagespeed 2.1 is open source (Apache-2.0).';
// Where the support subscriptions and their terms are described. The trailing
// slash is canonical.
export const SUPPORT_TERMS_URL = 'https://we-amp.com/licensing/';
