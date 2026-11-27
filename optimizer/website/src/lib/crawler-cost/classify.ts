// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Classification of a log line: which AI crawler claimed the request (by the
// user agent it sent, never verified), whether a path is a page or an asset,
// and which AI assistant sent a visitor (by Referer host or a query marker).
import crawlerData from '../../data/ai-crawlers.json';
import referrerData from '../../data/answer-engine-referrers.json';
import type { CrawlerEntry, ReferrerEntry } from './types';

export interface ReferrerData {
  hosts: ReferrerEntry[];
  queryMarkers: Array<{ param: string; value: string; operator: string }>;
}

/** robots.txt control tokens that never appear as a user agent; kept out of the list. */
export const ROBOTS_ONLY_TOKENS: readonly string[] = ['Google-Extended', 'Applebot-Extended'];

const CRAWLERS = crawlerData as CrawlerEntry[];
const REFERRERS = referrerData as ReferrerData;

export function classifyUserAgent(
  ua: string | null,
  list: CrawlerEntry[] = CRAWLERS,
): CrawlerEntry | null {
  if (!ua) return null;
  const hay = ua.toLowerCase();
  for (const entry of list) {
    if (hay.includes(entry.token.toLowerCase())) return entry;
  }
  return null;
}

const ASSET_EXTENSIONS = new Set([
  'css',
  'js',
  'mjs',
  'map',
  'png',
  'jpg',
  'jpeg',
  'gif',
  'svg',
  'webp',
  'avif',
  'ico',
  'woff',
  'woff2',
  'ttf',
  'otf',
  'eot',
  'mp4',
  'webm',
  'mp3',
  'pdf',
  'xml',
  'json',
  'txt',
  'zip',
  'gz',
  'tgz',
  'br',
  'wasm',
  'csv',
]);

export function isPagePath(path: string): boolean {
  const slash = path.lastIndexOf('/');
  const last = slash >= 0 ? path.slice(slash + 1) : path;
  const dot = last.lastIndexOf('.');
  if (dot < 0) return true;
  const ext = last.slice(dot + 1).toLowerCase();
  if (ext === 'html' || ext === 'htm') return true;
  return !ASSET_EXTENSIONS.has(ext);
}

function refererHost(referer: string): string | null {
  if (!/^https?:\/\//i.test(referer)) return null;
  try {
    const host = new URL(referer).hostname.toLowerCase();
    return host.startsWith('www.') ? host.slice(4) : host;
  } catch {
    return null;
  }
}

export function referralOperator(
  referer: string | null,
  query: string,
  data: ReferrerData = REFERRERS,
): string | null {
  if (referer && referer !== '-') {
    const host = refererHost(referer);
    if (host) {
      for (const h of data.hosts) {
        if (host === h.host || host.endsWith('.' + h.host)) return h.operator;
      }
    }
  }
  if (query) {
    const params = new URLSearchParams(query);
    for (const m of data.queryMarkers) {
      if (params.get(m.param) === m.value) return m.operator;
    }
  }
  return null;
}
