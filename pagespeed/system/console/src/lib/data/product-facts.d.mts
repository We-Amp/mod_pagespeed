// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Type declarations for the synced product-facts copy (./product-facts.mjs —
 * see ../sync-product-facts.sh). Only the exports the console consumes are
 * declared; the canonical shared file also exports PRODUCT_STATEMENT and
 * SUPPORT_TERMS_URL, which no console code imports today.
 */
export const VENDOR: string;
export const VENDOR_URL: string;
export const WEBSITE: string;
export const PRODUCT_NAME: string;
export const PRODUCT_DISPLAY_NAME: string;
export const PRIVACY_URL: string;
export const TERMS_URL: string;
export const SUPPORT_URL: string;
