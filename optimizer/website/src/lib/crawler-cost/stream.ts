// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Bytes in, lines out, in bounded memory. Gzip is recognised by its magic
// bytes and inflated with fflate's streaming Gunzip, which reads every member
// of a concatenated archive (logrotate output); the browser's
// DecompressionStream stops after the first member and is not used.
import { Gunzip } from 'fflate';

export interface LineChunk {
  text: string;
  cut: boolean;
}
export interface ReadOptions {
  onBytes?: (n: number) => void;
  onGzip?: () => void;
  maxLineBytes?: number;
  maxDecompressedBytes?: number;
}
export class StreamError extends Error {}
const GZIP_DAMAGED =
  'the gzip data ended early or is damaged; the lines before that point are counted';

export function isGzip(bytes: Uint8Array): boolean {
  return bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
}

export async function* readLines(
  stream: ReadableStream<Uint8Array>,
  opts: ReadOptions = {},
): AsyncGenerator<LineChunk> {
  const maxLine = opts.maxLineBytes ?? 65_536;
  const maxOut = opts.maxDecompressedBytes ?? 20_000_000_000;
  const decoder = new TextDecoder('utf-8');
  const reader = stream.getReader();
  let pending = '';
  let cutting = false;
  let produced = 0;
  let gunzip: Gunzip | null = null;
  let first = true;
  const inflated: Uint8Array[] = [];
  let finished = false;
  // Truncation check without fflate internals: the last 4 input bytes of a
  // complete gzip member are its ISIZE trailer (output length mod 2^32), so
  // after the final push they must equal the inflated byte count of the last
  // member. fflate throws on most cuts but not on a cut at a flush boundary or
  // inside the trailer; the count catches those.
  let memberOut = 0;
  const tail4 = new Uint8Array(4);
  let tailLen = 0;
  function remember(bytes: Uint8Array): void {
    const n = Math.min(4, bytes.length);
    tail4.copyWithin(0, n, 4);
    tail4.set(bytes.subarray(bytes.length - n), 4 - n);
    tailLen = Math.min(4, tailLen + n);
  }

  function* feed(text: string): Generator<LineChunk> {
    let start = 0;
    for (;;) {
      const nl = text.indexOf('\n', start);
      if (nl < 0) break;
      const piece = text.slice(start, nl);
      start = nl + 1;
      if (cutting) {
        cutting = false;
        yield { text: pending.slice(0, 200), cut: true };
        pending = '';
        continue;
      }
      let line = pending + piece;
      pending = '';
      if (line.length > maxLine) {
        yield { text: line.slice(0, 200), cut: true };
        continue;
      }
      if (line.endsWith('\r')) line = line.slice(0, -1);
      yield { text: line, cut: false };
    }
    const rest = text.slice(start);
    if (cutting) return;
    pending += rest;
    if (pending.length > maxLine) cutting = true;
  }

  function* handle(bytes: Uint8Array): Generator<LineChunk> {
    produced += bytes.length;
    if (produced > maxOut)
      throw new StreamError(
        gunzip
          ? `more than ${Math.round(maxOut / 1e9)} GB after decompression; stopped here`
          : `this file is larger than ${Math.round(maxOut / 1e9)} GB; stopped here`,
      );
    yield* feed(decoder.decode(bytes, { stream: true }));
  }

  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      if (!value || value.length === 0) continue;
      opts.onBytes?.(value.length);
      if (first) {
        first = false;
        if (isGzip(value)) {
          opts.onGzip?.();
          gunzip = new Gunzip((chunk) => {
            memberOut += chunk.length;
            inflated.push(chunk);
          });
          gunzip.onmember = () => {
            memberOut = 0;
          };
        }
      }
      if (gunzip) {
        remember(value);
        let damaged = false;
        try {
          gunzip.push(value);
        } catch {
          damaged = true;
        }
        // Lines inflated before the damage are still good: yield them first.
        while (inflated.length) yield* handle(inflated.shift() as Uint8Array);
        if (damaged) throw new StreamError(GZIP_DAMAGED);
        while (inflated.length) yield* handle(inflated.shift() as Uint8Array);
      } else {
        yield* handle(value);
      }
    }
    if (gunzip) {
      try {
        gunzip.push(new Uint8Array(0), true);
      } catch {
        throw new StreamError(GZIP_DAMAGED);
      }
      while (inflated.length) yield* handle(inflated.shift() as Uint8Array);
      const isize = (tail4[0] | (tail4[1] << 8) | (tail4[2] << 16) | (tail4[3] << 24)) >>> 0;
      if (tailLen < 4 || isize !== memberOut >>> 0) throw new StreamError(GZIP_DAMAGED);
    }
    const tail = decoder.decode();
    if (tail) yield* feed(tail);
    if (cutting) yield { text: pending.slice(0, 200), cut: true };
    else if (pending !== '')
      yield { text: pending.endsWith('\r') ? pending.slice(0, -1) : pending, cut: false };
    finished = true;
  } finally {
    if (!finished) await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
}
