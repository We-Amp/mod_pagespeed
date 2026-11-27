// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Types and utilities for HAR-like waterfall chart data.
 *
 * These types model the network request waterfall displayed in the
 * WaterfallChart component, closely following the HAR 1.2 timing model.
 */

// -- Resource types -----------------------------------------------------------

export type ResourceType =
  | 'document'
  | 'stylesheet'
  | 'script'
  | 'image'
  | 'font'
  | 'xhr'
  | 'other';

// -- Timing breakdown ---------------------------------------------------------

/** Per-request timing phases, all in milliseconds. */
export interface RequestTiming {
  /** DNS lookup duration. */
  dns: number;
  /** TCP connection duration. */
  connect: number;
  /** TLS handshake duration (0 for non-HTTPS). */
  tls: number;
  /** Time to first byte (waiting for the server response). */
  ttfb: number;
  /** Content download duration. */
  download: number;
}

// -- Waterfall entry ----------------------------------------------------------

/** One network request in the waterfall. */
export interface WaterfallEntry {
  /** Full request URL. */
  url: string;
  /** HTTP method (GET, POST, etc.). */
  method: string;
  /** Classified resource type. */
  resourceType: ResourceType;
  /** Start time in ms from navigation start. */
  startTime: number;
  /** Timing breakdown for each phase. */
  timing: RequestTiming;
  /** Bytes transferred (on the wire, after compression). */
  size: number;
  /** Bytes decoded (uncompressed). */
  decodedSize: number;
  /** HTTP status code. */
  status: number;
  /** MIME type of the response. */
  mimeType: string;
  /** Whether served from browser cache. */
  fromCache: boolean;
  /** Whether served via a service worker. */
  fromServiceWorker: boolean;
  /** Protocol used (e.g. "h2", "h3", "http/1.1"). */
  protocol: string;
}

// -- Navigation timing --------------------------------------------------------

/** Key navigation timing milestones, all in ms from navigation start. */
export interface NavigationTiming {
  /** DOMContentLoaded event firing time. */
  domContentLoaded: number;
  /** Load event firing time. */
  loadEvent: number;
  /** First Paint time. */
  firstPaint: number;
  /** First Contentful Paint time. */
  firstContentfulPaint: number;
  /** Largest Contentful Paint time. */
  largestContentfulPaint: number;
}

// -- Full waterfall dataset ---------------------------------------------------

/** Complete waterfall dataset for rendering. */
export interface WaterfallData {
  /** All network request entries, typically sorted by startTime. */
  entries: WaterfallEntry[];
  /** Navigation timing milestones. */
  navigationTiming: NavigationTiming;
  /** Sum of all entries' transferred sizes. */
  totalTransferSize: number;
  /** Sum of all entries' decoded sizes. */
  totalDecodedSize: number;
}

// -- Timing phase metadata (for rendering) ------------------------------------

/** Metadata for a single timing phase bar segment. */
export interface TimingPhase {
  /** Machine-readable phase key. */
  key: keyof RequestTiming;
  /** Human-readable label. */
  label: string;
  /** CSS color for this phase. */
  color: string;
}

/** Ordered timing phases with display colors. */
export const TIMING_PHASES: readonly TimingPhase[] = [
  { key: 'dns', label: 'DNS', color: '#5fc9f3' },
  { key: 'connect', label: 'Connect', color: '#4caf50' },
  { key: 'tls', label: 'TLS', color: '#9c27b0' },
  { key: 'ttfb', label: 'TTFB', color: '#ff9800' },
  { key: 'download', label: 'Download', color: '#2196f3' },
] as const;

// -- Navigation milestone metadata --------------------------------------------

/** Metadata for a navigation timing milestone line. */
export interface MilestoneConfig {
  key: keyof NavigationTiming;
  label: string;
  color: string;
  /** Dash pattern for SVG stroke-dasharray, or empty for solid. */
  dashArray: string;
}

/** Milestone lines rendered on the waterfall. */
export const MILESTONES: readonly MilestoneConfig[] = [
  {
    key: 'domContentLoaded',
    label: 'DCL',
    color: '#2196f3',
    dashArray: '4 2',
  },
  { key: 'loadEvent', label: 'Load', color: '#f44336', dashArray: '4 2' },
  { key: 'firstPaint', label: 'FP', color: '#9c27b0', dashArray: '2 2' },
  {
    key: 'firstContentfulPaint',
    label: 'FCP',
    color: '#4caf50',
    dashArray: '2 2',
  },
  {
    key: 'largestContentfulPaint',
    label: 'LCP',
    color: '#ff9800',
    dashArray: '',
  },
] as const;

// -- Resource type labels -----------------------------------------------------

/** Short display labels for resource types. */
export const RESOURCE_TYPE_LABELS: Record<ResourceType, string> = {
  document: 'HTML',
  stylesheet: 'CSS',
  script: 'JS',
  image: 'Image',
  font: 'Font',
  xhr: 'XHR',
  other: 'Other',
};

/** Colors for resource type indicators. */
export const RESOURCE_TYPE_COLORS: Record<ResourceType, string> = {
  document: '#e91e63',
  stylesheet: '#9c27b0',
  script: '#ff9800',
  image: '#4caf50',
  font: '#00bcd4',
  xhr: '#607d8b',
  other: '#795548',
};

// -- Utility functions --------------------------------------------------------

/** Total duration of a request (sum of all timing phases). */
export function totalDuration(timing: RequestTiming): number {
  return timing.dns + timing.connect + timing.tls + timing.ttfb + timing.download;
}

/** End time of a request (startTime + total duration). */
export function entryEndTime(entry: WaterfallEntry): number {
  return entry.startTime + totalDuration(entry.timing);
}

// formatBytes is now in @pagespeed/api-client — re-export for backwards compat
export { formatBytes } from '@pagespeed/api-client';

/**
 * Format a millisecond duration into a human-readable string.
 *
 * Examples: "0 ms", "42 ms", "1.23 s", "12.5 s"
 */
export function formatDuration(ms: number): string {
  if (ms < 0) return '0 ms';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  const s = ms / 1000;
  // Use 1 decimal place for >= 10s, 2 for < 10s.
  // Check against 9.995 to avoid "10.00 s" from rounding 9.999.
  return s >= 9.995 ? `${s.toFixed(1)} s` : `${s.toFixed(2)} s`;
}

/**
 * Truncate a URL for display, showing the filename or last path segment.
 *
 * @param url  Full URL string.
 * @param maxLen  Maximum length for the result (default 60).
 */
export function truncateUrl(url: string, maxLen: number = 60): string {
  if (url.length <= maxLen) return url;

  try {
    const parsed = new URL(url);
    const path = parsed.pathname;
    const segments = path.split('/').filter(Boolean);
    const last = segments.length > 0 ? segments[segments.length - 1] : '';

    if (last.length > 0 && last.length <= maxLen - 3) {
      return `.../${last}`;
    }
  } catch {
    // Not a valid URL; fall through to simple truncation.
  }

  return url.slice(0, maxLen - 3) + '...';
}

/**
 * Compute the end of the waterfall timeline (max of all entry end times
 * and all milestone times).
 */
export function waterfallEndTime(data: WaterfallData): number {
  let maxTime = 0;
  for (const entry of data.entries) {
    const end = entryEndTime(entry);
    if (end > maxTime) maxTime = end;
  }
  const nt = data.navigationTiming;
  for (const key of Object.keys(nt) as (keyof NavigationTiming)[]) {
    if (nt[key] > maxTime) maxTime = nt[key];
  }
  return maxTime;
}
