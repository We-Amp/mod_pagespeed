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
