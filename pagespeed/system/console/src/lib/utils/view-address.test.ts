// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { withParams } from "./view-address";

describe("withParams", () => {
  it("adds a parameter to an address without a query", () => {
    expect(withParams("#/logs", { source: "module" })).toBe("#/logs?source=module");
  });

  it("appends a new parameter after the ones already there", () => {
    expect(withParams("#/logs?level=debug", { source: "optimizer" })).toBe("#/logs?level=debug&source=optimizer");
  });

  it("replaces a parameter where it stands", () => {
    expect(withParams("#/logs?source=module&level=error", { source: "optimizer" })).toBe(
      "#/logs?source=optimizer&level=error",
    );
  });

  it("removes a parameter for null, and the '?' with the last one", () => {
    expect(withParams("#/logs?source=module&group=0", { group: null })).toBe("#/logs?source=module");
    expect(withParams("#/logs?source=module", { source: null })).toBe("#/logs");
    expect(withParams("#/logs", { source: null })).toBe("#/logs");
  });

  it("applies several changes at once", () => {
    expect(withParams("#/logs?source=optimizer&level=debug&group=0", { source: null, group: null })).toBe(
      "#/logs?level=debug",
    );
    expect(withParams("#/logs?level=debug", { source: "optimizer", group: "0" })).toBe(
      "#/logs?level=debug&source=optimizer&group=0",
    );
  });

  it("keeps the host lens and every other parameter as written", () => {
    expect(withParams("#/logs?lens=www.example.test&level=info", { source: "module" })).toBe(
      "#/logs?lens=www.example.test&level=info&source=module",
    );
    expect(withParams("#/rule-packs?pack=a&lens=www.example.test", { pack: "b" })).toBe(
      "#/rule-packs?pack=b&lens=www.example.test",
    );
    expect(withParams("#/logs?q=a%20b&x=%41", { group: "0" })).toBe("#/logs?q=a%20b&x=%41&group=0");
  });

  it("keeps a repeated parameter once", () => {
    expect(withParams("#/logs?source=module&source=optimizer", { source: "both" })).toBe("#/logs?source=both");
    expect(withParams("#/logs?group=0&level=info&group=0", { group: null })).toBe("#/logs?level=info");
  });

  it("encodes a value", () => {
    expect(withParams("#/rule-packs", { pack: "a b&c" })).toBe("#/rule-packs?pack=a+b%26c");
  });

  it("treats an empty address as the default page", () => {
    expect(withParams("", { lens: null })).toBe("#/overview");
  });
});
