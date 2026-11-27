// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Sums one or more parsed logs into the numbers the page shows. The client
// address never enters here: nothing is sessionised, referred visits are
// counted as landing page requests.
import { classifyUserAgent, isPagePath, referralOperator } from './classify';
import type { Aggregate, CrawlerRow, FileReport, ParsedLine } from './types';

export function emptyAggregate(): Aggregate {
  return {
    requests: 0,
    bytes: 0,
    bytesMissing: 0,
    unparsed: 0,
    unparsedSamples: [],
    agentlessLines: 0,
    firstTime: null,
    lastTime: null,
    crawlers: [],
    crawlerRequests: 0,
    crawlerBytes: 0,
    crawlerPageRequests: 0,
    fetcherPageRequests: 0,
    referredPageRequests: 0,
    referredByOperator: {},
    files: [],
  };
}

export class Aggregator {
  private readonly a = emptyAggregate();
  private readonly rows = new Map<string, CrawlerRow>();

  add(line: ParsedLine): void {
    const a = this.a;
    a.requests += 1;
    if (line.bytes === null) a.bytesMissing += 1;
    else a.bytes += line.bytes;
    if (line.time !== null) {
      if (a.firstTime === null || line.time < a.firstTime) a.firstTime = line.time;
      if (a.lastTime === null || line.time > a.lastTime) a.lastTime = line.time;
    }
    if (line.userAgent === null) a.agentlessLines += 1;
    const page = isPagePath(line.path);
    const crawler = classifyUserAgent(line.userAgent);
    if (crawler) {
      let row = this.rows.get(crawler.token);
      if (!row) {
        row = {
          token: crawler.token,
          operator: crawler.operator,
          kind: crawler.kind,
          requests: 0,
          bytes: 0,
          pageRequests: 0,
        };
        this.rows.set(crawler.token, row);
      }
      row.requests += 1;
      row.bytes += line.bytes ?? 0;
      a.crawlerRequests += 1;
      a.crawlerBytes += line.bytes ?? 0;
      if (page) {
        row.pageRequests += 1;
        if (crawler.kind === 'fetcher') a.fetcherPageRequests += 1;
        else a.crawlerPageRequests += 1;
      }
      return;
    }
    if (page && (line.status === 200 || line.status === 304)) {
      const operator = referralOperator(line.referer, line.query);
      if (operator) {
        a.referredPageRequests += 1;
        a.referredByOperator[operator] = (a.referredByOperator[operator] ?? 0) + 1;
      }
    }
  }

  addUnparsed(sample: string): void {
    this.a.unparsed += 1;
    if (this.a.unparsedSamples.length < 3) this.a.unparsedSamples.push(sample.slice(0, 200));
  }

  addFile(report: FileReport): void {
    this.a.files.push(report);
  }

  result(): Aggregate {
    const crawlers = [...this.rows.values()]
      .map((r) => ({ ...r }))
      .sort((x, y) => y.requests - x.requests || x.token.localeCompare(y.token));
    return {
      ...this.a,
      crawlers,
      referredByOperator: { ...this.a.referredByOperator },
      unparsedSamples: [...this.a.unparsedSamples],
      files: [...this.a.files],
    };
  }
}
