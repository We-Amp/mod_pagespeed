// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The three accepted access-log formats. The combined parser finds the core
// `[time] "request" status bytes "referer" "user-agent"` anywhere in the line,
// so prefixed variants (Apache vhost_combined) and nginx formats that append
// fields after the user agent parse too. The regular expression is unanchored,
// has no nested quantifiers, and every character class excludes the character
// that opens the next retry (the time class excludes `[`), so a retry never
// rescans text a previous attempt consumed and the total work is linear in the
// line length.
import type { Format, ParsedLine } from './types';

const MONTHS: Record<string, number> = {
  jan: 0,
  feb: 1,
  mar: 2,
  apr: 3,
  may: 4,
  jun: 5,
  jul: 6,
  aug: 7,
  sep: 8,
  oct: 9,
  nov: 10,
  dec: 11,
};

export function parseClfTime(s: string): number | null {
  const m = /^(\d{1,2})\/([A-Za-z]{3})\/(\d{4}):(\d{2}):(\d{2}):(\d{2}) ([+-])(\d{2})(\d{2})$/.exec(
    s.trim(),
  );
  if (!m) return null;
  const month = MONTHS[m[2].toLowerCase()];
  if (month === undefined) return null;
  const utc = Date.UTC(Number(m[3]), month, Number(m[1]), Number(m[4]), Number(m[5]), Number(m[6]));
  const offsetMin = (Number(m[8]) * 60 + Number(m[9])) * (m[7] === '+' ? 1 : -1);
  return utc - offsetMin * 60_000;
}

export function splitRequest(request: string): { method: string; path: string; query: string } {
  const parts = request.split(' ');
  let method = '';
  let target = request;
  if (parts.length >= 2 && /^[A-Z]+$/.test(parts[0])) {
    method = parts[0];
    target = parts[1];
  }
  const q = target.indexOf('?');
  return q < 0
    ? { method, path: target, query: '' }
    : { method, path: target.slice(0, q), query: target.slice(q + 1) };
}

const CORE = /\[([^\[\]]*)\] "([^"]*)" (\d{3}) (-|\d+)(?: "([^"]*)" "([^"]*)")?/;

function dashNull(s: string | undefined): string | null {
  return s === undefined || s === '' || s === '-' ? null : s;
}

export function parseCombinedLine(line: string): ParsedLine | null {
  const m = CORE.exec(line);
  if (!m) return null;
  const { method, path, query } = splitRequest(m[2]);
  return {
    time: parseClfTime(m[1]),
    method,
    path,
    query,
    status: Number(m[3]),
    bytes: m[4] === '-' ? 0 : Number(m[4]),
    referer: dashNull(m[5]),
    userAgent: dashNull(m[6]),
  };
}

export function detectFormat(firstLine: string): Format {
  if (/^#(Software: Microsoft Internet Information Services|Fields:)/.test(firstLine)) return 'iis';
  const m = CORE.exec(firstLine);
  if (!m) return 'unknown';
  return m[5] === undefined ? 'common' : 'combined';
}

export class IisParser {
  fields: string[] | null = null;
  private index = Object.create(null) as Record<string, number>;

  get hasBytes(): boolean {
    return 'sc-bytes' in this.index;
  }
  get hasUserAgent(): boolean {
    return 'cs(User-Agent)' in this.index;
  }

  private col(row: string[], name: string): string | undefined {
    const i = this.index[name];
    return i === undefined ? undefined : row[i];
  }

  parseLine(line: string): ParsedLine | 'directive' | null {
    if (line.startsWith('#')) {
      if (line.startsWith('#Fields:')) {
        this.fields = line.slice('#Fields:'.length).trim().split(/\s+/);
        this.index = Object.create(null) as Record<string, number>;
        this.fields.forEach((f, i) => {
          if (!(f in this.index)) this.index[f] = i;
        });
      }
      return 'directive';
    }
    if (!this.fields) return null;
    const row = line.split(' ');
    if (row.length < Math.min(this.fields.length, 4)) return null;
    const statusRaw = this.col(row, 'sc-status');
    if (statusRaw === undefined || !/^\d{3}$/.test(statusRaw)) return null;
    const status = Number(statusRaw);
    const stem = this.col(row, 'cs-uri-stem');
    if (!Number.isInteger(status) || stem === undefined) return null;
    const date = this.col(row, 'date');
    const time = this.col(row, 'time');
    const t = date && time ? Date.parse(`${date}T${time}Z`) : NaN;
    const bytesRaw = this.col(row, 'sc-bytes');
    const query = this.col(row, 'cs-uri-query');
    const decode = (v: string | null): string | null => (v === null ? null : v.replace(/\+/g, ' '));
    return {
      time: Number.isNaN(t) ? null : t,
      method: this.col(row, 'cs-method') ?? '',
      path: stem,
      query: query === undefined || query === '-' ? '' : query,
      status,
      bytes:
        bytesRaw === undefined
          ? null
          : bytesRaw === '-'
            ? 0
            : /^\d+$/.test(bytesRaw)
              ? Number(bytesRaw)
              : null,
      referer: decode(dashNull(this.col(row, 'cs(Referer)'))),
      userAgent: decode(dashNull(this.col(row, 'cs(User-Agent)'))),
    };
  }
}
