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
    // A :global(...) rule reaches every page: judge what it wraps.
    const unwrapped = text.replace(/:global\(([^)]*)\)/g, "$1");
    out.push(...unwrapped.split(",").map((s) => s.trim().replace(/\s+/g, " ")).filter((s) => s !== ""));
  }
  return out;
}

function styleOf(source: string): string {
  return /<style[^>]*>([\s\S]*?)<\/style>/.exec(source)?.[1] ?? "";
}

const pages = readdirSync(PAGES)
  .filter((f) => f.endsWith(".svelte"))
  .map((f) => ({ name: f, source: readFileSync(join(PAGES, f), "utf8") }));

/** The JavaScript of every class={...} attribute, braces balanced. */
function classExpressions(markup: string): string[] {
  const out: string[] = [];
  for (const m of markup.matchAll(/\bclass=\{/g)) {
    let depth = 1;
    let i = m.index! + m[0].length;
    const start = i;
    for (; i < markup.length && depth > 0; i++) {
      if (markup[i] === "{") depth++;
      else if (markup[i] === "}") depth--;
    }
    out.push(markup.slice(start, i - 1));
  }
  return out;
}

// Every component: the pages, the shared ones beside this file, and the shell.
const components = [
  ...pages,
  ...readdirSync(LIB)
    .filter((f) => f.endsWith(".svelte"))
    .map((f) => ({ name: f, source: readFileSync(join(LIB, f), "utf8") })),
  { name: "App.svelte", source: readFileSync(join(LIB, "..", "App.svelte"), "utf8") },
];

// A rule on a bare table part or heading restyles every one on the page.
const BARE_ELEMENT = /^(table|thead|tbody|tfoot|tr|th|td|caption|h[1-6])(?![\w-])/;
// The shared classes and their modifiers.
const SHARED_CLASS = new RegExp(
  "\\.(" +
    [
      "card",
      "card-muted",
      "loading",
      "note",
      "notice(?:-[\\w-]+)?",
      "badge(?:-[\\w-]+)?",
      "level-badge(?:-[\\w-]+)?",
      "status-dot(?:-[\\w-]+)?",
      "status-word",
      "stat-tiles?(?:-[\\w-]+)?",
      "health-strip(?:-[\\w-]+)?",
      "section-title",
      "card-title",
      "minor-title",
      "table-frame(?:-[\\w-]+)?",
      "data(?:-dense|-inline|-raw)?",
      "info-grid",
      "empty-state",
    ].join("|") +
    ")(?![\\w-])",
);

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

  // A modifier joins its class with one hyphen (.notice-error, .card-muted),
  // never with "--", in the markup and in the styles alike.
  it("names no class with a double hyphen", () => {
    const offenders: string[] = [];
    for (const { name, source } of components) {
      const style = /<style[^>]*>[\s\S]*?<\/style>/.exec(source)?.[0] ?? "";
      const markup = source.replace(style, "");
      for (const [m] of style.matchAll(/\.-?[A-Za-z_][\w-]*--[\w-]*/g)) offenders.push(`${name}: ${m}`);
      for (const [, list] of markup.matchAll(/\bclass="([^"]*)"/g)) {
        for (const token of list.split(/\s+/)) if (token.includes("--")) offenders.push(`${name}: ${token}`);
      }
      for (const [m] of markup.matchAll(/\bclass:[\w-]*--[\w-]*/g)) offenders.push(`${name}: ${m}`);
      for (const expression of classExpressions(markup)) {
        for (const [, , text] of expression.matchAll(/(["'`])((?:(?!\1)[\s\S])*)\1/g)) {
          for (const token of text.split(/\s+/)) if (token.includes("--")) offenders.push(`${name}: ${token}`);
        }
      }
    }
    for (const [m] of theme.matchAll(/\.-?[A-Za-z_][\w-]*--[\w-]*/g)) offenders.push(`theme.css: ${m}`);
    expect(offenders).toEqual([]);
  });
});
