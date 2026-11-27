// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The default social card, one URL for every og:image and JSON-LD `image`
// that uses it. Production serves /og-* as immutable for a year, so the query
// string is the only way a regenerated card reaches anyone: bump `v` whenever
// scripts/generate-title-cards.mjs changes og-default.png.

import { WEBSITE } from './product-facts.mjs';

export const OG_DEFAULT_IMAGE = '/og-default.png?v=4';
export const OG_DEFAULT_IMAGE_URL = `${WEBSITE}${OG_DEFAULT_IMAGE}`;
