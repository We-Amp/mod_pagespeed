// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
export type CrawlerKind = 'training' | 'search' | 'fetcher' | 'other';

export interface CrawlerEntry {
  token: string; // substring matched case-insensitively against the claimed user agent
  operator: string; // "OpenAI", "Anthropic", ...
  kind: CrawlerKind;
  docs: string; // the operator's public page for the token
  added: string; // YYYY-MM-DD
}

export interface ReferrerEntry {
  host: string; // lowercase, no leading "www."
  operator: string; // matches CrawlerEntry.operator where the same company runs both
  product: string;
  checked: string; // YYYY-MM-DD
}

export interface ParsedLine {
  time: number | null; // ms since epoch, null when the timestamp did not parse
  method: string;
  path: string; // request path without the query
  query: string; // query without the leading "?", "" when none
  status: number;
  bytes: number | null; // response body bytes as logged; null when the log carries no bytes field
  referer: string | null; // null for "-" or a missing field
  userAgent: string | null; // null for "-" or a missing field
}

export type Format = 'combined' | 'common' | 'iis' | 'unknown';

export interface CrawlerRow {
  token: string;
  operator: string;
  kind: CrawlerKind;
  requests: number;
  bytes: number;
  pageRequests: number; // isPagePath(path), any status
}

export interface FileReport {
  name: string;
  format: Format;
  gzip: boolean;
  lines: number; // parsed lines
  givenUp: boolean; // more than half of the first 200 non-directive lines unparsed
  sample: string | null; // the file's own first unparsed line, cut to 200 characters, null when every line parsed
  error: string | null; // decompression or read error, in words
}

export interface Aggregate {
  requests: number;
  bytes: number; // sum over lines with a bytes value
  bytesMissing: number; // lines without a bytes field (IIS without sc-bytes)
  unparsed: number;
  unparsedSamples: string[]; // first three unparsed lines, each cut to 200 characters
  agentlessLines: number; // parsed lines whose user agent is "-" or missing (privacy-stripped logs)
  firstTime: number | null;
  lastTime: number | null;
  crawlers: CrawlerRow[]; // sorted by requests descending, then token
  crawlerRequests: number; // all kinds, all paths
  crawlerBytes: number;
  crawlerPageRequests: number; // kinds training, search, other; page paths; any status
  fetcherPageRequests: number; // kind fetcher; page paths; any status
  referredPageRequests: number; // status 200 or 304, non-crawler agent, page path, AI referer or marker
  referredByOperator: Record<string, number>;
  files: FileReport[];
}

export interface Rates {
  perGb: number;
  perMillionRequests: number;
  currency: string; // three letters, shown as typed
}
