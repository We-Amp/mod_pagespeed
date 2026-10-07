// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import type { DaemonAlternate } from "$lib/api/types";
import { computeUrlStatus } from "./url-status";

const alt = (over: Partial<DaemonAlternate>): DaemonAlternate => ({
  alternate_id: 0,
  format: "original",
  encoding: "identity",
  content_type: "image",
  is_sentinel: false,
  ...over,
});

describe("computeUrlStatus", () => {
  it("is null with no non-sentinel alternates", () => {
    expect(computeUrlStatus([])).toBeNull();
    expect(
      computeUrlStatus([{ is_sentinel: true, sentinel_name: "content_hash" }]),
    ).toBeNull();
  });
  it("flags revalidation before anything else", () => {
    expect(
      computeUrlStatus([
        alt({ format: "webp" }),
        alt({ format: "avif", needs_revalidation: true }),
      ]),
    ).toBe("revalidating");
  });
  it("is original-only with exactly one non-sentinel alternate", () => {
    expect(computeUrlStatus([alt({})])).toBe("original-only");
  });
  it("classifies image coverage: webp+avif complete, webp alone partial", () => {
    const base = [alt({}), alt({ alternate_id: 1, format: "webp" })];
    expect(computeUrlStatus(base)).toBe("partial");
    expect(
      computeUrlStatus([...base, alt({ alternate_id: 2, format: "avif" })]),
    ).toBe("complete");
  });
  it("recognizes an image entry by its content_type CLASS alone", () => {
    // Two "original"-format image variants (e.g. two viewports), nothing
    // modern yet: an image entry with partial coverage, not a text one.
    expect(
      computeUrlStatus([
        alt({ viewport: "mobile" }),
        alt({ alternate_id: 1, viewport: "desktop", encoding: "gzip" }),
      ]),
    ).toBe("partial");
  });
  it("classifies text coverage: gzip+brotli complete, gzip alone partial", () => {
    const gzipOnly = [
      alt({ content_type: "css", encoding: "identity" }),
      alt({ alternate_id: 1, content_type: "css", encoding: "gzip" }),
    ];
    expect(computeUrlStatus(gzipOnly)).toBe("partial");
    expect(
      computeUrlStatus([
        ...gzipOnly,
        alt({ alternate_id: 2, content_type: "css", encoding: "brotli" }),
      ]),
    ).toBe("complete");
  });
  it("counts only the daemon's own brotli spelling", () => {
    expect(
      computeUrlStatus([
        alt({ content_type: "js", encoding: "gzip" }),
        alt({ alternate_id: 1, content_type: "js", encoding: "br" }),
      ]),
    ).toBe("partial");
  });
});
