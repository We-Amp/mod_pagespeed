// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
import { constants, createGzip, gzipSync } from 'node:zlib';
import { describe, expect, it } from 'vitest';
import { StreamError, isGzip, readLines } from '../../src/lib/crawler-cost/stream';

const enc = new TextEncoder();
function streamOf(...parts: Array<Uint8Array | string>): ReadableStream<Uint8Array> {
  return new Blob(
    parts.map((p) => (typeof p === 'string' ? enc.encode(p) : p) as BlobPart),
  ).stream();
}
async function collect(s: ReadableStream<Uint8Array>, opts = {}) {
  const out: Array<{ text: string; cut: boolean }> = [];
  for await (const c of readLines(s, opts)) out.push(c);
  return out;
}

describe('readLines', () => {
  it('splits on LF and CRLF, drops the trailing carriage return and yields a last line without newline', async () => {
    const lines = await collect(streamOf('a\nb\r\nc'));
    expect(lines).toEqual([
      { text: 'a', cut: false },
      { text: 'b', cut: false },
      { text: 'c', cut: false },
    ]);
  });
  it('joins a line split across chunks and reports bytes read', async () => {
    let bytes = 0;
    const lines = await collect(streamOf('hel', 'lo\nwor', 'ld\n'), {
      onBytes: (n: number) => (bytes += n),
    });
    expect(lines.map((l) => l.text)).toEqual(['hello', 'world']);
    expect(bytes).toBe(12);
  });
  it('cuts a line over the cap, keeps 200 characters of it and marks it', async () => {
    const long = 'A'.repeat(70_000);
    const lines = await collect(streamOf(`${long}\nok\n`), { maxLineBytes: 65_536 });
    expect(lines[0].cut).toBe(true);
    expect(lines[0].text).toHaveLength(200);
    expect(lines[1]).toEqual({ text: 'ok', cut: false });
  });
  it('replaces invalid UTF-8 instead of failing', async () => {
    const lines = await collect(streamOf(new Uint8Array([0x61, 0xff, 0xfe, 0x62, 0x0a])));
    expect(lines[0].text).toBe('a��b');
  });
  it('detects gzip by the magic bytes, not the name, and inflates it', async () => {
    const gz = new Uint8Array(gzipSync('one\ntwo\n'));
    expect(isGzip(gz)).toBe(true);
    expect(isGzip(enc.encode('one'))).toBe(false);
    let flagged = false;
    const lines = await collect(streamOf(gz), { onGzip: () => (flagged = true) });
    expect(flagged).toBe(true);
    expect(lines.map((l) => l.text)).toEqual(['one', 'two']);
  });
  it('reads every member of a concatenated gzip file', async () => {
    const a = new Uint8Array(gzipSync('one\ntwo\n'));
    const b = new Uint8Array(gzipSync('three\nfour\n'));
    const lines = await collect(streamOf(a, b));
    expect(lines.map((l) => l.text)).toEqual(['one', 'two', 'three', 'four']);
  });
  it('throws a StreamError on a truncated gzip stream after yielding what it could', async () => {
    const text =
      Array.from(
        { length: 20_000 },
        (_, i) => `line ${i} ${((i * 2654435761) % 4294967296).toString(16)}`,
      ).join('\n') + '\n';
    const whole = new Uint8Array(gzipSync(text));
    const cut = whole.subarray(0, whole.length - 12);
    const seen: string[] = [];
    let error: unknown;
    try {
      for await (const c of readLines(streamOf(cut))) seen.push(c.text);
    } catch (e) {
      error = e;
    }
    expect(error).toBeInstanceOf(StreamError);
    expect(seen.length).toBeGreaterThan(0);
  });
  it('yields the lines of a complete member before the error for trailing garbage in one chunk', async () => {
    const good = new Uint8Array(gzipSync('one\ntwo\n'));
    const garbage = new Uint8Array(12).fill(0x1f);
    const joined = new Uint8Array(good.length + garbage.length);
    joined.set(good);
    joined.set(garbage, good.length);
    const seen: string[] = [];
    let error: unknown;
    try {
      for await (const c of readLines(streamOf(joined))) seen.push(c.text);
    } catch (e) {
      error = e;
    }
    expect(seen).toEqual(['one', 'two']);
    expect(error).toBeInstanceOf(StreamError);
  });
  it('stops with a StreamError past the decompressed-bytes guard', async () => {
    const gz = new Uint8Array(gzipSync('x'.repeat(10_000)));
    await expect(collect(streamOf(gz), { maxDecompressedBytes: 5_000 })).rejects.toBeInstanceOf(
      StreamError,
    );
  });
  it('throws on a cut at a full-flush block boundary and yields the lines before it', async () => {
    const first = Array.from({ length: 500 }, (_, i) => `first ${i}`).join('\n') + '\n';
    const second = Array.from({ length: 500 }, (_, i) => `second ${i}`).join('\n') + '\n';
    const gz = createGzip();
    const parts: Buffer[] = [];
    gz.on('data', (b: Buffer) => parts.push(b));
    gz.write(first);
    let flushedAt = 0;
    await new Promise<void>((resolve) =>
      gz.flush(constants.Z_FULL_FLUSH, () => {
        flushedAt = Buffer.concat(parts).length;
        resolve();
      }),
    );
    gz.write(second);
    await new Promise<void>((resolve) => {
      gz.on('end', () => resolve());
      gz.end();
    });
    const all = new Uint8Array(Buffer.concat(parts));
    expect(flushedAt).toBeGreaterThan(0);
    expect(flushedAt).toBeLessThan(all.length);
    const seen: string[] = [];
    let error: unknown;
    try {
      for await (const c of readLines(streamOf(all.subarray(0, flushedAt)))) seen.push(c.text);
    } catch (e) {
      error = e;
    }
    expect(error).toBeInstanceOf(StreamError);
    expect(seen).toHaveLength(500);
    expect(seen[499]).toBe('first 499');
  });
  it('throws when only the gzip trailer is cut, after yielding every line', async () => {
    const text = Array.from({ length: 1000 }, (_, i) => `row ${i}`).join('\n') + '\n';
    const whole = new Uint8Array(gzipSync(text));
    for (const drop of [4, 8]) {
      const seen: string[] = [];
      let error: unknown;
      try {
        for await (const c of readLines(streamOf(whole.subarray(0, whole.length - drop)))) {
          seen.push(c.text);
        }
      } catch (e) {
        error = e;
      }
      expect(error).toBeInstanceOf(StreamError);
      expect(seen).toHaveLength(1000);
    }
  });
  it('names the configured limit in the guard message', async () => {
    const gz = new Uint8Array(gzipSync('x'.repeat(10_000)));
    await expect(collect(streamOf(gz), { maxDecompressedBytes: 5_000 })).rejects.toThrow(
      /after decompression/,
    );
    await expect(collect(streamOf('x'.repeat(100)), { maxDecompressedBytes: 50 })).rejects.toThrow(
      /larger than/,
    );
  });
});
