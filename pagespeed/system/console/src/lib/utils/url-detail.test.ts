// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  DEFAULT_VARIANT_SORT,
  epochMsOf,
  epochSecToMs,
  nextVariantSortState,
  originalContentId,
  previewable,
  sentinelLabel,
  sortVariantRows,
  statusLabel,
  toVariantRows,
} from "./url-detail";

// The daemon's shape (cache_handlers.cc:141-316).
const BODY = {
  url: "/hero.png",
  hostname: "www.example.test",
  scheme: "https",
  alternates: [
    {
      alternate_id: 0, format: "original", content_type: "image", origin_content_type: "image/png",
      size: 45210, hit_count: 128, last_access: 1_790_000_000_000, cache_inserted_at: 1_789_999_000,
      viewport: "desktop", density: "1x", save_data: false, encoding: "identity",
    },
    {
      alternate_id: 1, format: "webp", content_type: "image", origin_content_type: "image/png",
      size: 30984, hit_count: 1042, last_access: 1_790_000_100_000, ssimulacra2_score: 92.5,
      viewport: "desktop", density: "1x", save_data: false, encoding: "identity",
      content_class: "photo", original_size: 45210,
    },
    { alternate_id: 60, is_sentinel: true, sentinel_name: "content_hash", hit_count: 0 },
  ],
  count: 3,
};

describe("toVariantRows", () => {
  it("maps the daemon shape, resolving the MIME from format + origin type", () => {
    const rows = toVariantRows(BODY);
    expect(rows).toHaveLength(3);
    const original = rows.find((r) => r.id === 0);
    expect(original?.mimeType).toBe("image/png");
    expect(original?.typeClass).toBe("image");
    expect(original?.cachedAtMs).toBe(1_789_999_000_000);
    const webp = rows.find((r) => r.id === 1);
    expect(webp?.mimeType).toBe("image/webp");
    expect(webp?.density).toBe("1x");
    expect(webp?.contentClass).toBe("photo");
    expect(webp?.quality).toBe(92.5);
    expect(webp?.originalSize).toBe(45210);
    expect(webp?.lastAccessMs).toBe(1_790_000_100_000);
    const sentinel = rows.find((r) => r.id === 60);
    expect(sentinel?.isSentinel).toBe(true);
    expect(sentinel?.sentinelName).toBe("content_hash");
  });
  it("survives garbage without NaN or undefined leaking", () => {
    const rows = toVariantRows({
      alternates: [
        null,
        42,
        {},
        {
          alternate_id: "x", size: -5, hit_count: -1, last_access: "soon",
          ssimulacra2_score: NaN, density: 2, original_size: "big",
        },
      ],
    } as never);
    expect(rows).toHaveLength(2); // null and 42 are skipped; {} and the garbage object normalize
    for (const row of rows) {
      expect(row.id).toBeNull();
      expect(row.hits).toBe(0);
      expect(row.size).toBeNull();
      expect(row.originalSize).toBeNull();
      expect(row.quality).toBeNull(); // NaN is not a score
      expect(row.lastAccessMs).toBeNull();
      expect(row.density).toBe(""); // a number is not the daemon's "1x"/"2x+"
    }
    expect(toVariantRows(null)).toEqual([]);
    expect(toVariantRows({ alternates: "no" } as never)).toEqual([]);
  });
  it("keys duplicate ids distinctly: the each-block keys can never collide", () => {
    const rows = toVariantRows({
      alternates: [
        { alternate_id: 7, format: "webp" },
        { alternate_id: 7, format: "webp" },
        { format: "webp" },
        { format: "webp" },
      ],
    } as never);
    expect(rows).toHaveLength(4);
    expect(new Set(rows.map((r) => r.rowKey)).size).toBe(4);
  });
});

describe("previewable", () => {
  const row = (over: Record<string, unknown>) =>
    toVariantRows({ alternates: [{ alternate_id: 5, content_type: "image", ...over }] } as never)[0];
  it("is true only for image-class variants the module's gate serves", () => {
    expect(previewable(row({ format: "webp" }))).toBe(true);
    expect(previewable(row({ format: "avif" }))).toBe(true);
    expect(previewable(row({ format: "original", origin_content_type: "image/jpeg" }))).toBe(true);
    expect(previewable(row({ format: "jpeg", origin_content_type: "image/jpeg" }))).toBe(true);
    expect(previewable(row({ format: "svg" }))).toBe(false); // the gate answers 415
    expect(previewable(row({ format: "original", origin_content_type: "" }))).toBe(false);
    expect(previewable(row({ format: "original", content_type: "css", origin_content_type: "text/css" }))).toBe(false);
    expect(previewable(row({ format: "webp", is_sentinel: true }))).toBe(false);
    expect(previewable(row({ format: "webp", alternate_id: "x" }))).toBe(false);
  });
});

describe("epoch helpers keep the units straight", () => {
  it("last_access is milliseconds, cache_inserted_at seconds", () => {
    expect(epochMsOf(1_790_000_000_000)).toBe(1_790_000_000_000);
    expect(epochSecToMs(1_789_999_000)).toBe(1_789_999_000_000);
  });
  it("rejects garbage", () => {
    for (const bad of [undefined, null, "now", -5, 0, NaN, Infinity]) {
      expect(epochMsOf(bad)).toBeNull();
      expect(epochSecToMs(bad)).toBeNull();
    }
  });
  it("bounds values to the Date range: toISOString throws past 8.64e15 ms", () => {
    expect(epochMsOf(8.64e15)).toBe(8.64e15);
    expect(epochMsOf(8.64e15 + 1)).toBeNull();
    expect(epochMsOf(1e300)).toBeNull();
    expect(epochSecToMs(8.64e12)).toBe(8.64e15); // the largest in-range seconds value
    expect(epochSecToMs(9e12)).toBeNull(); // 9e15 ms overflows the Date range
    expect(epochSecToMs(1e300)).toBeNull();
  });
  it("rows with out-of-range timestamps treat them as absent", () => {
    const rows = toVariantRows({
      alternates: [{ alternate_id: 1, last_access: 1e300, cache_inserted_at: 9e12 }],
    } as never);
    expect(rows[0].lastAccessMs).toBeNull();
    expect(rows[0].cachedAtMs).toBeNull();
  });
});

describe("sortVariantRows / nextVariantSortState", () => {
  it("opens hits-descending and flips on the active key", () => {
    expect(DEFAULT_VARIANT_SORT).toEqual({ key: "hits", asc: false });
    expect(nextVariantSortState(DEFAULT_VARIANT_SORT, "hits")).toEqual({ key: "hits", asc: true });
    expect(nextVariantSortState(DEFAULT_VARIANT_SORT, "format")).toEqual({ key: "format", asc: true });
  });
  it("sorts numbers with nulls last, format alphabetically", () => {
    const rows = toVariantRows(BODY);
    const byHits = sortVariantRows(rows, { key: "hits", asc: false });
    expect(byHits.map((r) => r.hits)).toEqual([1042, 128, 0]);
    const byQuality = sortVariantRows(rows, { key: "quality", asc: false });
    expect(byQuality[0].quality).toBe(92.5);
    expect(byQuality[byQuality.length - 1].quality).toBeNull();
    const byFormat = sortVariantRows(rows, { key: "format", asc: true });
    expect(byFormat.map((r) => r.format || r.sentinelName)).toEqual([
      "content_hash",
      "original",
      "webp",
    ]);
  });
});

describe("sentinelLabel", () => {
  it("labels the daemon's sentinel names in plain language, never the raw code", () => {
    expect(sentinelLabel("content_hash")).toBe("Content hash record");
    expect(sentinelLabel("original_content")).toBe("Original content record");
    expect(sentinelLabel("early_hints")).toBe("Early hints record");
    expect(sentinelLabel("warmup")).toBe("Warmup request record");
    expect(sentinelLabel("subresource_manifest")).toBe("Subresource manifest record");
    expect(sentinelLabel("browser_profile")).toBe("Browser profile record");
    expect(sentinelLabel("headers_sidecar")).toBe("Response headers record");
    expect(sentinelLabel("agent_markdown")).toBe("Agent markdown record");
    expect(sentinelLabel("llms_txt")).toBe("llms.txt record");
    expect(sentinelLabel("llms_txt_meta")).toBe("llms.txt metadata record");
    expect(sentinelLabel("negative_verdict")).toBe("Negative verdict record");
    expect(sentinelLabel("decline_tombstone")).toBe("Decline tombstone record");
  });

  it("never shows an unknown name's raw code; an absent name is a pending entry", () => {
    expect(sentinelLabel("unknown_sentinel")).toBe("Internal record");
    expect(sentinelLabel("something_new")).toBe("Internal record");
    expect(sentinelLabel(null)).toBe("Pending");
    expect(sentinelLabel("")).toBe("Pending");
  });
});

describe("statusLabel", () => {
  it("labels every status", () => {
    expect(statusLabel("complete")).toBe("Complete");
    expect(statusLabel("partial")).toBe("Partial");
    expect(statusLabel("original-only")).toBe("Original only");
    expect(statusLabel("revalidating")).toBe("Revalidating");
    expect(statusLabel(null)).toBe("Nothing cached");
  });
});

describe("originalContentId", () => {
  // The optimizer records a cached original as the "original_content"
  // sentinel (no viewport, density or format of its own); its bytes are the
  // original image, so it is what a variant is compared against when no
  // same-shaped original entry exists.
  const withSentinel = toVariantRows({
    alternates: [
      { alternate_id: 12, is_sentinel: true, sentinel_name: "original_content", size: 241401, hit_count: 18 },
      { alternate_id: 9, is_sentinel: false, format: "webp", viewport: "desktop", density: "1x", size: 33160, hit_count: 13 },
      { alternate_id: 60, is_sentinel: true, sentinel_name: "content_hash", size: 32, hit_count: 1 },
    ],
  });

  it("finds the original-content record", () => {
    expect(originalContentId(withSentinel)).toBe(12);
  });

  it("is null without one, or when it is empty or has no usable id", () => {
    expect(originalContentId(toVariantRows({ alternates: [] }))).toBeNull();
    expect(
      originalContentId(toVariantRows({ alternates: [{ alternate_id: 12, is_sentinel: true, sentinel_name: "original_content", size: 0 }] })),
    ).toBeNull();
    expect(
      originalContentId(toVariantRows({ alternates: [{ alternate_id: -1, is_sentinel: true, sentinel_name: "original_content", size: 10 }] })),
    ).toBeNull();
  });
});
