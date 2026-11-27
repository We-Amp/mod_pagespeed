// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Drives dropped files through detection, parsing and aggregation. Runs in
// the Web Worker on the page and under vitest with Blob-backed sources.
import { Aggregator } from './aggregate';
import { IisParser, detectFormat, parseCombinedLine } from './formats';
import { StreamError, readLines } from './stream';
import type { Aggregate, FileReport, Format } from './types';

export interface LineSource {
  name: string;
  size: number;
  stream(): ReadableStream<Uint8Array>;
}
export interface AnalyzeHooks {
  onProgress?(bytesRead: number, bytesTotal: number): void;
  onFile?(report: FileReport): void;
}
/** Non-directive lines examined before a file that is mostly unparsed is given up. */
export const GIVE_UP_WINDOW = 200;

export async function analyzeFiles(
  files: LineSource[],
  hooks: AnalyzeHooks = {},
): Promise<Aggregate> {
  const agg = new Aggregator();
  const total = files.reduce((n, f) => n + f.size, 0);
  let read = 0;
  for (const file of files) {
    const report: FileReport = {
      name: file.name,
      format: 'unknown',
      gzip: false,
      lines: 0,
      givenUp: false,
      sample: null,
      error: null,
    };
    let unparsed = 0;
    // Blank lines are skipped and never counted as junk, but a run of them as
    // long as the give-up window means the file is not a log (a file of
    // newlines would otherwise be read to its end for nothing).
    let blankRun = 0;
    const noteUnparsed = (text: string): void => {
      agg.addUnparsed(text);
      unparsed += 1;
      report.sample ??= text.slice(0, 200);
    };
    let fileRead = 0;
    let iis: IisParser | null = null;
    let detected = false;
    try {
      const lines = readLines(file.stream(), {
        onBytes: (n) => {
          read += n;
          fileRead += n;
          hooks.onProgress?.(read, total);
        },
        onGzip: () => {
          report.gzip = true;
        },
      });
      for await (const chunk of lines) {
        if (chunk.cut) {
          noteUnparsed(chunk.text);
        } else {
          const text = chunk.text;
          if (text.trim() === '') {
            blankRun += 1;
            if (blankRun >= GIVE_UP_WINDOW) {
              if (report.lines === 0) report.givenUp = true;
              break;
            }
            continue;
          }
          blankRun = 0;
          if (!detected) {
            detected = true;
            const f: Format = detectFormat(text);
            report.format = f;
            if (f === 'iis') iis = new IisParser();
          }
          if (iis) {
            const r = iis.parseLine(text);
            if (r === 'directive') {
              if (iis.fields) report.format = iis.hasUserAgent ? 'iis' : 'common';
              continue;
            }
            if (r === null) {
              noteUnparsed(text);
            } else {
              // Files without a user-agent column are counted but kept out of
              // the totals so they cannot dilute shares for combined logs.
              if (report.format !== 'common') agg.add(r);
              report.lines += 1;
            }
          } else {
            const r = parseCombinedLine(text);
            if (r === null) {
              noteUnparsed(text);
            } else {
              // Files without a user-agent column are counted but kept out of
              // the totals so they cannot dilute shares for combined logs.
              if (report.format !== 'common') agg.add(r);
              report.lines += 1;
            }
          }
        }
        if (report.lines + unparsed === GIVE_UP_WINDOW && unparsed > report.lines) {
          report.givenUp = true;
          break;
        }
      }
    } catch (e) {
      report.error =
        e instanceof StreamError
          ? e.message
          : `could not read this file: ${e instanceof Error ? e.message : String(e)}`;
    }
    if (
      !report.givenUp &&
      report.lines + unparsed < GIVE_UP_WINDOW &&
      unparsed > report.lines &&
      unparsed > 0
    )
      report.givenUp = true;
    if (fileRead < file.size) {
      read += file.size - fileRead;
      hooks.onProgress?.(read, total);
    }
    agg.addFile(report);
    hooks.onFile?.(report);
  }
  return agg.result();
}
