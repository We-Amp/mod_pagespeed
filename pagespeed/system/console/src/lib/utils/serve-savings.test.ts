// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { serveSavingsView } from "./serve-savings";

const zero = { hits: 0, optimized_bytes: 0, original_bytes: 0 };
// The block as a live module-serves host reports it.
const LIVE = {
  css: { hits: 197, optimized_bytes: 21447548, original_bytes: 21472396 },
  html: zero,
  image: { hits: 45, optimized_bytes: 258379, original_bytes: 922228 },
  js: zero,
};

describe("serveSavingsView", () => {
  it("lists the classes the module served, in a fixed order, and names the ones it did not", () => {
    const v = serveSavingsView(LIVE)!;
    expect(v.served.map((r) => r.label)).toEqual(["CSS", "Images"]);
    expect(v.notServed).toEqual(["HTML", "JavaScript"]);
    expect(v.served[0]).toEqual({
      key: "css", label: "CSS", hits: 197, original: 21472396, served: 21447548, saved: 24848, percent: 0,
    });
    expect(v.served[1].percent).toBe(72);
    expect(v.total).toEqual({ saved: 688697, original: 22394624, percent: 3 });
  });

  it("nothing served yet: every class is 'not served' and there is no total", () => {
    const v = serveSavingsView({ html: zero, css: zero, js: zero, image: zero })!;
    expect(v.served).toEqual([]);
    expect(v.notServed).toEqual(["HTML", "CSS", "JavaScript", "Images"]);
    expect(v.total).toBeNull();
  });

  it("a class the console does not know keeps its own name, after the known ones", () => {
    const v = serveSavingsView({ font: { hits: 1, original_bytes: 100, optimized_bytes: 50 }, css: LIVE.css })!;
    expect(v.served.map((r) => r.label)).toEqual(["CSS", "font"]);
  });

  it("saved stays within [0, original] and the percent within [0, 100]", () => {
    const grew = serveSavingsView({ css: { hits: 1, original_bytes: 10, optimized_bytes: 30 } })!;
    expect(grew.served[0]).toMatchObject({ saved: 0, percent: 0 });
    const negative = serveSavingsView({ css: { hits: 1, original_bytes: 10, optimized_bytes: -1000 } })!;
    expect(negative.served[0]).toMatchObject({ served: 0, saved: 10, percent: 100 });
    expect(negative.total).toEqual({ saved: 10, original: 10, percent: 100 });
  });

  it("junk is no data: malformed classes and malformed counters are skipped, not counted as zero", () => {
    const v = serveSavingsView({ css: "x", js: { hits: "3", original_bytes: null }, image: [1, 2] })!;
    expect(v.served).toEqual([]);
    expect(v.notServed).toEqual([]);
    expect(v.total).toBeNull();
  });

  it("a class with a byte count missing is no data, never a 100 % saving", () => {
    for (const block of [{ css: { original_bytes: 10 } }, { css: { hits: 5, original_bytes: 10 } }]) {
      const v = serveSavingsView(block)!;
      expect(v.served).toEqual([]);
      expect(v.notServed).toEqual([]);
      expect(v.total).toBeNull();
    }
  });

  it("no block, or not an object: null", () => {
    expect(serveSavingsView(undefined)).toBeNull();
    expect(serveSavingsView(null)).toBeNull();
    expect(serveSavingsView([])).toBeNull();
    expect(serveSavingsView("css")).toBeNull();
  });
});
