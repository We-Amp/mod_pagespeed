// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Which URL the gallery generator requests when it measures a subresource.
//
// The demo host optimizes resources in place: a plain request for
// `images/a.png` returns the original on a cold cache and an optimized copy
// once the background rewrite has finished. The "before" side is measured
// first, so a plain fetch would credit that in-place optimization to the
// filter under test, although the filter never touched the URL.
//
// A resource URL that the filter did not rewrite is therefore requested with
// `PageSpeed=off` on BOTH sides, which returns the original bytes, so an
// unchanged URL measures the same before and after. A rewritten resource
// (a `.pagespeed.` URL) is what the filter produced and is measured as served.
// Resources on other hosts are left alone: the demo host does not serve them.

const REWRITTEN = /\.pagespeed\./;

/**
 * @param {string} resourceUrl absolute resource URL from the page
 * @param {string} demoOrigin origin of the demo host, e.g. https://demo.test
 * @returns {string} the URL to fetch for the byte count
 */
export function resourceFetchUrl(resourceUrl, demoOrigin) {
  const url = new URL(resourceUrl);
  if (url.origin !== new URL(demoOrigin).origin) return url.href;
  if (REWRITTEN.test(url.pathname)) return url.href;
  url.searchParams.set("PageSpeed", "off");
  url.hash = "";
  return url.href;
}
