// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// What the owner of a site on a hosted platform can do inside that platform,
// keyed by the platform id the scanner reports. An entry is listed only when
// every statement in it was checked against the vendor's own help pages; each
// action links the page it was checked against. A platform without an entry
// gets the general advice of the scan result instead.

export interface PlatformAction {
  /** One plain sentence: what to do and where. */
  text: string;
  /** The vendor help page the sentence was checked against. */
  source: string;
  /** The link text for the source. */
  sourceLabel: string;
}

export interface PlatformGuidance {
  /** The vendor's help domain; every source is on it. */
  vendorDomain: string;
  /** The date the statements were last checked, YYYY-MM-DD. */
  checked: string;
  /** One to three actions; a scan result lists the first two. */
  actions: PlatformAction[];
}

export const PLATFORM_GUIDANCE: Record<string, PlatformGuidance> = {
  wix: {
    vendorDomain: 'support.wix.com',
    checked: '2026-11-21',
    actions: [
      {
        text: 'Set the title tag and the meta description of each page in that page’s SEO settings.',
        source:
          'https://support.wix.com/en/article/customizing-your-pages-seo-settings-in-the-seo-panel',
        sourceLabel: 'Wix help: page SEO settings',
      },
      {
        text: 'Open the robots.txt editor in the SEO section of your Wix dashboard and check that the file does not block AI crawlers.',
        source: 'https://support.wix.com/en/article/editing-your-sites-robotstxt-file',
        sourceLabel: 'Wix help: robots.txt',
      },
    ],
  },
};

/** The guidance for a platform id, or null when there is none. */
export function guidanceFor(id: unknown): PlatformGuidance | null {
  if (typeof id !== 'string' || !Object.hasOwn(PLATFORM_GUIDANCE, id)) return null;
  return PLATFORM_GUIDANCE[id];
}
