// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ---------------------------------------------------------------------------
// CSV/JSON export utilities for dashboard and savings data.
// ---------------------------------------------------------------------------

/**
 * Generate a timestamped filename.
 * Example: `pagespeed-stats-2024-01-15T10-30-00.json`
 */
export function timestampedFilename(prefix: string, ext: string): string {
  const iso = new Date()
    .toISOString()
    .replace(/:/g, '-')
    .replace(/\.\d{3}Z$/, '');
  return `${prefix}-${iso}.${ext}`;
}

/**
 * Escape a single CSV cell value.
 * - If the value contains a comma, double-quote, or newline, wrap it in
 *   double-quotes and escape any internal double-quotes by doubling them.
 */
export function escapeCsvCell(value: string | number): string {
  const str = String(value);
  if (str.includes(',') || str.includes('"') || str.includes('\n')) {
    return '"' + str.replace(/"/g, '""') + '"';
  }
  return str;
}

/**
 * Build a CSV string from headers and rows.
 */
export function buildCsvString(
  headers: string[],
  rows: (string | number)[][],
): string {
  const lines: string[] = [];
  lines.push(headers.map(escapeCsvCell).join(','));
  for (const row of rows) {
    lines.push(row.map(escapeCsvCell).join(','));
  }
  return lines.join('\n') + '\n';
}

/**
 * Trigger a browser file download for the given content.
 * SSR-safe: only runs when `document` is available.
 */
export function triggerDownload(content: string, filename: string, mime: string): void {
  if (typeof document === 'undefined') return;

  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);

  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();

  // Clean up after a short delay to allow the download to start.
  setTimeout(() => {
    URL.revokeObjectURL(url);
    anchor.remove();
  }, 100);
}

/**
 * Export arbitrary data as a pretty-printed JSON file download.
 */
export function exportJson(data: unknown, filename: string): void {
  const content = JSON.stringify(data, null, 2) + '\n';
  triggerDownload(content, filename, 'application/json');
}

/**
 * Export tabular data as a CSV file download.
 */
export function exportCsv(
  headers: string[],
  rows: (string | number)[][],
  filename: string,
): void {
  const content = buildCsvString(headers, rows);
  triggerDownload(content, filename, 'text/csv');
}
