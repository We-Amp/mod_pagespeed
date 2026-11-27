// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The one schema.org Organization node for We-Amp B.V. The home page emits it
// as a top-level node and /about/ as its mainEntity; every other node points at
// it by `@id`. The `@id` is the same one iispeed.com and we-amp.com use, so
// search engines reconcile one legal entity across the three properties. The
// canonical URL is we-amp.com (the legal entity's site), not modpagespeed.com.

import { PRODUCT_DISPLAY_NAME, VENDOR, VENDOR_URL } from './product-facts.mjs';

export const ORGANIZATION_ID = 'https://we-amp.com/#organization';

// The company's logo (the We-Amp mark, not the product logo).
export const ORGANIZATION_LOGO_URL = 'https://we-amp.com/images/weamp.svg';

// What a product or article node carries as its publisher: the `@id` that
// resolves to the full node, plus the name and URL for consumers that do not
// follow references.
export const ORGANIZATION_REF = {
  '@type': 'Organization',
  '@id': ORGANIZATION_ID,
  name: VENDOR,
  url: VENDOR_URL,
};

// What an article node (TechArticle, BlogPosting) carries as its publisher:
// the reference above plus the company's logo, which article rich results
// expect on the publisher. The same logo as the full node, so the publisher
// of every article is recognisably the same organization.
export const ORGANIZATION_PUBLISHER = {
  ...ORGANIZATION_REF,
  logo: { '@type': 'ImageObject', url: ORGANIZATION_LOGO_URL },
};

// The brand We-Amp owns. The nginx module's historical name and the 2.0 line
// (a separate line with its own releases, so not an alternate name of the
// product node) stay as alternate names of the brand.
export const PRODUCT_BRAND = {
  '@type': 'Brand',
  name: PRODUCT_DISPLAY_NAME,
  alternateName: ['ngx_pagespeed', 'ModPageSpeed 2.0'],
};

export const ORGANIZATION = {
  '@type': 'Organization',
  '@id': ORGANIZATION_ID,
  name: VENDOR,
  url: VENDOR_URL,
  logo: ORGANIZATION_LOGO_URL,
  description:
    'Dutch software and consulting company specializing in HTTP infrastructure, web performance, and reverse proxy engineering.',
  foundingDate: '2012',
  founder: {
    '@type': 'Person',
    name: 'Otto van der Schaaf',
    sameAs: ['https://www.linkedin.com/in/oschaaf', 'https://github.com/oschaaf'],
  },
  address: {
    '@type': 'PostalAddress',
    addressLocality: 'Castricum',
    addressCountry: 'NL',
  },
  identifier: [{ '@type': 'PropertyValue', propertyID: 'KvK', value: '57898138' }],
  sameAs: [
    'https://modpagespeed.com',
    'https://iispeed.com',
    'https://ngxpagespeed.com',
    'https://github.com/We-Amp',
    'https://github.com/We-Amp/mod_pagespeed',
    'https://github.com/oschaaf',
  ],
  brand: PRODUCT_BRAND,
};
