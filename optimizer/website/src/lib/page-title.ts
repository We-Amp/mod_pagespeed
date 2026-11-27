// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { PRODUCT_DISPLAY_NAME } from '../data/product-facts.mjs';

/** Search engines cut a <title> past roughly this many characters. */
export const TITLE_MAX = 60;

/**
 * A collection page's <title>: the page title plus the ` — mod_pagespeed`
 * suffix. The suffix is left off when the title already names the product (it
 * would only repeat the brand) and when it would push the title past
 * TITLE_MAX, so a long title keeps its own words rather than spending them on
 * a suffix the results page would cut off.
 */
export function brandedTitle(title: string): string {
  if (title.includes(PRODUCT_DISPLAY_NAME)) return title;
  const suffixed = `${title} — ${PRODUCT_DISPLAY_NAME}`;
  return suffixed.length <= TITLE_MAX ? suffixed : title;
}
