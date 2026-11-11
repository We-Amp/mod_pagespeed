// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tables, cards, badges, notes, notices and headings look the same on every
// page because they are declared once, in theme.css. A page may lay them out
// (a margin, a column width) through its own class names, but it never
// re-declares the shared ones or styles the bare elements, which is how the
// copies drifted apart before.

import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const LIB = fileURLToPath(new URL(".", import.meta.url));
const PAGES = join(LIB, "..", "pages");
const theme = readFileSync(join(LIB, "theme.css"), "utf8");

/** The selectors of every rule in a component's <style> block, comments removed. */
function selectors(source: string): string[] {
  const style = /<style[^>]*>([\s\S]*?)<\/style>/.exec(source)?.[1] ?? "";
  const css = style.replace(/\/\*[\s\S]*?\*\//g, "");
  const out: string[] = [];
  for (const [, prelude] of css.matchAll(/([^{}]+)\{/g)) {
    const text = prelude.trim();
    if (text.startsWith("@")) continue;
    out.push(...text.split(",").map((s) => s.trim().replace(/\s+/g, " ")));
  }
  return out;
}

function styleOf(source: string): string {
  return /<style[^>]*>([\s\S]*?)<\/style>/.exec(source)?.[1] ?? "";
}

const pages = readdirSync(PAGES)
  .filter((f) => f.endsWith(".svelte"))
  .map((f) => ({ name: f, source: readFileSync(join(PAGES, f), "utf8") }));

// A rule on a bare table part or heading restyles every one on the page.
const BARE_ELEMENT = /^(table|thead|tbody|tfoot|tr|th|td|caption|h[1-6])(?![\w-])/;
// The shared classes and their modifiers.
const SHARED_CLASS = /\.(card|card-muted|loading|note|notice(?:-[\w-]+)?|badge(?:-[\w-]+)?|section-title|card-title|minor-title|table-frame|info-grid|empty-state)(?![\w-])/;

describe("shared component layer", () => {
  it("finds the pages", () => {
    expect(pages.length).toBeGreaterThanOrEqual(14);
  });

  for (const page of pages) {
    it(`${page.name} styles no bare table part or heading`, () => {
      expect(selectors(page.source).filter((s) => BARE_ELEMENT.test(s))).toEqual([]);
    });
    it(`${page.name} declares none of the shared table, card, badge, note or heading classes`, () => {
      expect(selectors(page.source).filter((s) => SHARED_CLASS.test(s))).toEqual([]);
    });
    it(`${page.name} never removes the focus outline`, () => {
      expect(styleOf(page.source)).not.toMatch(/outline:\s*(none|0)\b/);
    });
  }

  it("declares each shared class once, in the theme", () => {
    for (const cls of [
      ".section-title",
      ".card-title",
      ".minor-title",
      ".note",
      ".notice",
      ".card",
      ".card-muted",
      ".badge",
      ".table-frame",
      ".info-grid",
      ".empty-state",
      ".loading",
    ]) {
      const escaped = cls.replace(".", "\\.");
      expect(theme.match(new RegExp(`^${escaped}\\s*\\{`, "gm")), cls).toHaveLength(1);
    }
    expect(theme.match(/^table\.data\s*\{/gm)).toHaveLength(1);
  });

  it("gives every badge one size: variants set colours only", () => {
    const blocks = [...theme.matchAll(/^\.badge-[\w-]+\s*\{([^}]*)\}/gm)].map((m) => m[1]);
    expect(blocks.length).toBeGreaterThan(0);
    for (const block of blocks) expect(block).not.toMatch(/font-size|padding|border-radius|line-height/);
  });

  it("the body text of a page is the body type role", () => {
    expect(theme).toMatch(/^main\s*\{[^}]*font-size:\s*var\(--ps-type-body-size\)/m);
  });
});
