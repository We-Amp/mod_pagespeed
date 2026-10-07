// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { linkSegments, type LinkSegment } from "./linkify";

const joined = (segments: LinkSegment[]) => segments.map((s) => s.text).join("");
const links = (segments: LinkSegment[]) =>
  segments.filter((s): s is Extract<LinkSegment, { kind: "link" }> => s.kind === "link").map((s) => [s.text, s.href]);

describe("linkSegments", () => {
  it("leaves text without a URL alone", () => {
    expect(linkSegments("plain text")).toEqual([{ kind: "text", text: "plain text" }]);
    expect(linkSegments("")).toEqual([]);
  });

  it("links an http or https URL with the parsed URL as href and the original text as text", () => {
    expect(linkSegments("Fetch of https://www.example.test/a.css failed")).toEqual([
      { kind: "text", text: "Fetch of " },
      { kind: "link", text: "https://www.example.test/a.css", href: "https://www.example.test/a.css" },
      { kind: "text", text: " failed" },
    ]);
    expect(links(linkSegments("HTTPS://Example.test/x"))).toEqual([["HTTPS://Example.test/x", "https://example.test/x"]]);
    expect(links(linkSegments("a https://a.test/1 b http://b.test/2"))).toEqual([
      ["https://a.test/1", "https://a.test/1"],
      ["http://b.test/2", "http://b.test/2"],
    ]);
  });

  it("never links another scheme", () => {
    for (const t of ["javascript:alert(1)", "data:text/html,<script>x</script>", "ftp://host/file", "vbscript:msgbox", "file:///etc/passwd"]) {
      expect(links(linkSegments(t)), t).toEqual([]);
    }
  });

  it("ends a URL at quotes, angle brackets and whitespace, and leaves trailing punctuation outside", () => {
    expect(links(linkSegments("'https://umami.example.test/script.js'"))).toEqual([
      ["https://umami.example.test/script.js", "https://umami.example.test/script.js"],
    ]);
    expect(links(linkSegments('https://ok.test/a"onmouseover=alert(3)'))).toEqual([["https://ok.test/a", "https://ok.test/a"]]);
    expect(links(linkSegments("https://a.test/<x>"))).toEqual([["https://a.test/", "https://a.test/"]]);
    expect(linkSegments("see https://a.test/b).")).toEqual([
      { kind: "text", text: "see " },
      { kind: "link", text: "https://a.test/b", href: "https://a.test/b" },
      { kind: "text", text: ")." },
    ]);
  });

  it("does not link a scheme inside a word or a URL that does not parse", () => {
    expect(links(linkSegments("xhttps://a.test"))).toEqual([]);
    expect(links(linkSegments("http://"))).toEqual([]);
  });

  it("stays linear on adversarial input: 100 kB of punctuation inside a URL finishes well under a second", () => {
    const run = ".,)".repeat(33_000);
    const start = performance.now();
    // The punctuation run is NOT at the end of the URL ...
    const inner = `see https://a.test/${run}x and more`;
    expect(joined(linkSegments(inner))).toBe(inner);
    expect(links(linkSegments(inner))).toEqual([[`https://a.test/${run}x`, new URL(`https://a.test/${run}x`).href]]);
    // ... and here it is, all of it trailing.
    expect(links(linkSegments(`https://a.test/${run} tail`))).toEqual([["https://a.test/", "https://a.test/"]]);
    expect(performance.now() - start).toBeLessThan(500);
  });

  it("always gives back exactly the input text", () => {
    for (const t of [
      "<img src=x onerror=alert(1)> javascript:alert(2) data:text/html,<b>x</b> https://ok.test/a\"onmouseover=alert(3)",
      "Ungültige Größe https://ü.test/ä?q=1, ok",
      "https://a.test/b).",
    ]) {
      expect(joined(linkSegments(t))).toBe(t);
    }
  });
});
