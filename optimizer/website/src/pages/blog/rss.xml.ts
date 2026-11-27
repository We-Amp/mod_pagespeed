// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import rss from '@astrojs/rss';
import { getCollection } from 'astro:content';
import type { APIContext } from 'astro';

const escapeXml = (s: string) =>
  s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export async function GET(context: APIContext) {
  const posts = (await getCollection('blog'))
    .filter((post) => !post.data.draft)
    .sort((a, b) => b.data.date.getTime() - a.data.date.getTime());

  const selfHref = new URL('blog/rss.xml', context.site!).href;
  const lastBuildDate = (posts[0]?.data.date ?? new Date(0)).toUTCString();

  return rss({
    // Covers the whole blog history (1.1 and 2.0 posts), so the channel is not version-scoped.
    title: 'mod_pagespeed Blog',
    description:
      'Technical insights on web performance, automatic optimization, and the engineering behind mod_pagespeed.',
    site: context.site!,
    xmlns: {
      dc: 'http://purl.org/dc/elements/1.1/',
      atom: 'http://www.w3.org/2005/Atom',
    },
    customData: [
      '<language>en-us</language>',
      `<lastBuildDate>${lastBuildDate}</lastBuildDate>`,
      `<atom:link href="${selfHref}" rel="self" type="application/rss+xml" />`,
    ].join(''),
    items: posts.map((post) => ({
      title: post.data.title,
      pubDate: post.data.date,
      description: post.data.description,
      link: `/blog/${post.id}/`,
      // dc:creator (a person's name) rather than RSS <author> (which expects an email).
      customData: `<dc:creator>${escapeXml(post.data.author)}</dc:creator>`,
    })),
  });
}
