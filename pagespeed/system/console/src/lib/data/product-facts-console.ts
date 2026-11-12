// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Console-local product facts.
 *
 * Identity facts (product/vendor names, site URLs) come from the canonical
 * product-facts single source, synced verbatim to ./product-facts.mjs (see
 * sync-product-facts.sh) and re-exported here so console code imports one
 * module. Only genuinely console-local derivations live in this file.
 */
import { PRODUCT_DISPLAY_NAME, VENDOR_URL, WEBSITE } from "./product-facts.mjs";

export {
  PRIVACY_URL,
  PRODUCT_DISPLAY_NAME,
  PRODUCT_NAME,
  SUPPORT_URL,
  SUPPORT_TERMS_URL,
  TERMS_URL,
  VENDOR,
  VENDOR_URL,
  WEBSITE,
} from "./product-facts.mjs";

/** Document title; injected into index.html at build time (vite.config.ts). */
export const CONSOLE_TITLE = `${PRODUCT_DISPLAY_NAME} Admin Console`;

/** Bare hosts for display (link text, aria labels). */
export const VENDOR_HOST = new URL(VENDOR_URL).host;
export const WEBSITE_HOST = new URL(WEBSITE).host;

/**
 * The website's support page: what a support subscription covers and how to
 * reach support (SUPPORT_URL is the page that sells the subscriptions).
 */
export const SUPPORT_PAGE_URL = `${WEBSITE}/support/`;

/** The one base of every documentation link the console shows. */
export const DOCS_BASE = `${WEBSITE}/docs`;

/**
 * The documentation pages and sections the console links to: every link
 * comes from this table, so they share one base and are checked in one place.
 */
export const DOCS = {
  /** The admin console's own page. */
  console: `${DOCS_BASE}/admin-console/`,
  /** Who may open the admin pages. */
  access: `${DOCS_BASE}/admin-console/#access-control`,
  /** The module's message log. */
  messages: `${DOCS_BASE}/admin-console/#message-history`,
  /** What the Optimizer status page shows. */
  optimizerStatus: `${DOCS_BASE}/admin-console/#optimizer-daemon-panels`,
  /** How to add the optimizer to a server. */
  optimizerSetup: `${DOCS_BASE}/configuration/#area-optimizer-worker`,
  /** How to get more diagnostic information. */
  diagnose: `${DOCS_BASE}/troubleshooting/#how-to-get-more-diagnostic-information`,
  /** Why a URL is not served optimized yet. */
  neverOptimized: `${DOCS_BASE}/troubleshooting/#pages-are-never-served-optimized--cache-never-warms`,
  /** Why an image keeps its original format. */
  imagesNotConverting: `${DOCS_BASE}/troubleshooting/#images-not-converting-to-webpavif`,
  /** Cache-Control headers the origin should send. */
  cacheControl: `${DOCS_BASE}/cache-control/`,
  /** Authorizing other hosts' resources. */
  domainAuthorization: `${DOCS_BASE}/domain-configuration/#domain-authorization`,
} as const;

/** The admin console's page in the product documentation. */
export const DOCS_URL = DOCS.console;
