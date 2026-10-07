// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Text in the console meets WCAG AA (4.5:1) in both colour schemes, and no
// component uses a colour token as text that cannot meet it.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { contrastRatio, themeTokens } from "./utils/contrast";

const LIB = fileURLToPath(new URL(".", import.meta.url));
const SRC = join(LIB, "..");
const css = readFileSync(join(LIB, "theme.css"), "utf8");
const tokens = themeTokens(css);

const AA = 4.5;
const SURFACES = ["--ps-bg", "--ps-bg-secondary", "--ps-surface"];
const TEXT = [
  "--ps-text",
  "--ps-text-secondary",
  "--ps-warning-text",
  "--ps-success-text",
  "--ps-error",
  "--ps-primary",
];
// Table headers and the neutral badge sit on the tertiary background.
const ON_TERTIARY = ["--ps-text", "--ps-text-secondary"];
// Inverse text on filled buttons and badges.
const FILLED = ["--ps-primary", "--ps-error", "--ps-success-text"];
// Fixed-background badges (the Logs page's "Fatal" tag, the warning fill's
// black text, and the on-image label chips — a fixed dark scrim behind
// white text): the pairing does not vary with the colour scheme, but is
// still checked against both schemes' token sets, so a future edit that
// made one half scheme-dependent without the other would be caught.
const FIXED_BADGES: ReadonlyArray<readonly [fg: string, bg: string]> = [
  ["--ps-severity-fatal-text", "--ps-severity-fatal-bg"],
  ["--ps-warning-contrast-text", "--ps-warning"],
  ["--ps-on-scrim", "--ps-scrim"],
];
// The top bar is its own band, not --ps-primary: in dark mode the primary
// turns pale blue, which made the band glare and dropped the badge below AA.
const TOPBAR_PAIRS: ReadonlyArray<readonly [fg: string, bg: string]> = [
  ["--ps-topbar-fg", "--ps-topbar-bg"],
  ["--ps-topbar-fg-muted", "--ps-topbar-bg"],
];
// One level palette shared by the module's messages and the optimizer's log.
// Every pairing is an existing token pair; this pins it in both schemes.
const LEVEL_BADGE_PAIRS: ReadonlyArray<readonly [fg: string, bg: string]> = [
  ["--ps-text-inverse", "--ps-error"],
  ["--ps-warning-contrast-text", "--ps-warning"],
  ["--ps-severity-fatal-text", "--ps-severity-fatal-bg"],
  ["--ps-text-secondary", "--ps-bg-tertiary"],
];

/** color-mix(in srgb, tint p%, base) over two #rrggbb colours. */
function mix(tint: string, percent: number, base: string): string {
  const rgb = (hex: string) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
  const p = percent / 100;
  const [a, b] = [rgb(tint), rgb(base)];
  return `#${a.map((v, i) => Math.round(v * p + b[i] * (1 - p)).toString(16).padStart(2, "0")).join("")}`;
}

// Every category badge class (FormatBadge, QualityBadge, ContentClassBadge):
// its text is the plain text token; the category colour lives on the 12%
// color-mix tint over --ps-bg (and the border). The pairing must hold AA in
// both schemes — a status token as the text colour did not (the tint is too
// close to it in the light scheme).
const BADGE_CLASSES = [
  "format-webp",
  "format-avif",
  "format-jpeg",
  "format-png",
  "quality-good",
  "quality-acceptable",
  "quality-poor",
  "class-photo",
  "class-screenshot",
  "class-illustration",
  "class-noisy",
] as const;
const BADGE_TINTS: Record<(typeof BADGE_CLASSES)[number], string> = {
  "format-webp": "--ps-primary",
  "format-avif": "--ps-secondary",
  "format-jpeg": "--ps-warning",
  "format-png": "--ps-success",
  "quality-good": "--ps-success",
  "quality-acceptable": "--ps-warning",
  "quality-poor": "--ps-error",
  "class-photo": "--ps-warning",
  "class-screenshot": "--ps-primary",
  "class-illustration": "--ps-success",
  "class-noisy": "--ps-error",
};

function svelteFiles(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) out.push(...svelteFiles(path));
    else if (name.endsWith(".svelte")) out.push(path);
  }
  return out;
}

describe("contrastRatio", () => {
  it("matches the WCAG definition", () => {
    expect(contrastRatio("#000000", "#ffffff")).toBeCloseTo(21, 1);
    expect(contrastRatio("#a8a29e", "#ffffff")).toBeCloseTo(2.52, 2);
    expect(contrastRatio("#ffffff", "#a8a29e")).toBeCloseTo(2.52, 2);
  });
});

describe("theme tokens", () => {
  for (const scheme of ["light", "dark"] as const) {
    const t = tokens[scheme];
    it(`${scheme}: every text token passes AA on every surface`, () => {
      for (const fg of TEXT) {
        for (const bg of SURFACES) {
          expect(contrastRatio(t[fg], t[bg]), `${fg} on ${bg}`).toBeGreaterThanOrEqual(AA);
        }
      }
    });
    it(`${scheme}: text on the tertiary background passes AA`, () => {
      for (const fg of ON_TERTIARY) {
        expect(contrastRatio(t[fg], t["--ps-bg-tertiary"]), fg).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: inverse text on filled controls passes AA`, () => {
      for (const bg of FILLED) {
        expect(contrastRatio(t["--ps-text-inverse"], t[bg]), `inverse on ${bg}`).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: text on a fixed-background badge passes AA`, () => {
      for (const [fg, bg] of FIXED_BADGES) {
        expect(contrastRatio(t[fg], t[bg]), `${fg} on ${bg}`).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: top bar text passes AA on its band`, () => {
      for (const [fg, bg] of TOPBAR_PAIRS) {
        expect(contrastRatio(t[fg], t[bg]), `${fg} on ${bg}`).toBeGreaterThanOrEqual(AA);
      }
      // The scope badge sits on a 15% white overlay over the band.
      const badgeBg = mix("#ffffff", 15, t["--ps-topbar-bg"]);
      expect(contrastRatio(t["--ps-topbar-fg"], badgeBg), "scope badge").toBeGreaterThanOrEqual(AA);
    });
    it(`${scheme}: the shared level badges pass AA in both schemes`, () => {
      for (const [fg, bg] of LEVEL_BADGE_PAIRS) {
        expect(contrastRatio(t[fg], t[bg]), `${fg} on ${bg}`).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: badge text passes AA on its colour-mix tint`, () => {
      for (const cls of BADGE_CLASSES) {
        const tint = BADGE_TINTS[cls];
        const bg = mix(t[tint], 12, t["--ps-bg"]);
        expect(contrastRatio(t["--ps-text"], bg), `.${cls}: --ps-text on 12% ${tint}`).toBeGreaterThanOrEqual(AA);
      }
    });
  }
  it("defines no tertiary text colour", () => {
    expect(tokens.light["--ps-text-tertiary"]).toBeUndefined();
  });
  it("the logo accent stays visible on the dark top bar", () => {
    // The chevron and blinking cursor are decorative marks on the band;
    // their fill is a token that must clear AA in dark, where a fill tuned
    // for a light band disappeared. Light keeps its long-standing accent,
    // decorative and exempt from the text floor.
    expect(
      contrastRatio(tokens.dark["--ps-topbar-accent"], tokens.dark["--ps-topbar-bg"]),
    ).toBeGreaterThanOrEqual(AA);
  });
});

describe("type scale", () => {
  it("monospace falls back to the system stack, never Courier", () => {
    expect(css).toContain(
      "--ps-font-mono: ui-monospace, 'SF Mono', 'JetBrains Mono', Menlo, Consolas, monospace;",
    );
  });
  it("defines the metric classes once, with tabular digits", () => {
    for (const cls of [".metric-value", ".metric-label", ".metric-context"]) {
      expect(css).toContain(cls);
    }
    expect(css).toContain("font-variant-numeric: tabular-nums");
  });
  it("charts read their axis and grid colours from theme tokens", () => {
    const chart = readFileSync(join(SRC, "lib/TimeSeriesChart.svelte"), "utf8");
    expect(chart).toContain('cssVar("--ps-text-secondary"');
    expect(chart).toContain('cssVar("--ps-border"');
  });
});

describe("level badges", () => {
  it("the one palette is defined once, globally, from tokens", () => {
    for (const cls of [".level-badge-error", ".level-badge-warning", ".level-badge-fatal", ".level-badge-info"]) {
      expect(css).toContain(cls);
    }
    expect(css).toMatch(/\.level-badge-fatal\s*\{[^}]*--ps-severity-fatal-bg/);
    expect(css).toMatch(/\.level-badge-info,\s*\.level-badge-debug/);
  });
  it("no page ships its own level colours", () => {
    // Logs shows the module's messages and the optimizer's log alike.
    for (const page of ["Logs"]) {
      const src = readFileSync(join(SRC, "pages", `${page}.svelte`), "utf8");
      expect(src, page).toContain("level-badge");
      expect(src, page).not.toMatch(/#8b0000/);
    }
  });
});

// The savings split's stacked bar: each segment fill must stand out from the
// bar's track (which also shows as the gap between segments) and from the
// card behind it — the muted per-host card included — and neighbouring
// segments must differ by 3:1, so the bar reads without relying on hue.
describe("savings split bar", () => {
  const savings = readFileSync(join(SRC, "pages/Savings.svelte"), "utf8");
  const background = (selector: string): string => {
    const block = new RegExp(`\\${selector}\\s*\\{([^}]*)\\}`).exec(savings);
    const token = block && /background:\s*var\((--ps-[\w-]+)\)/.exec(block[1]);
    expect(token, `${selector} background token`).not.toBeNull();
    return (token as RegExpExecArray)[1];
  };
  const order = [".split-already-optimal", ".split-optimized-served", ".split-served-encoded"];
  const fills = order.map(background);
  const track = background(".split-bar");
  for (const scheme of ["light", "dark"] as const) {
    const t = tokens[scheme];
    it(`${scheme}: every segment passes 4.5:1 on the track and the card`, () => {
      for (const [i, fill] of fills.entries()) {
        for (const bg of [track, "--ps-surface", "--ps-bg-secondary"]) {
          expect(contrastRatio(t[fill], t[bg]), `${order[i]} (${fill}) on ${bg}`).toBeGreaterThanOrEqual(AA);
        }
      }
    });
    it(`${scheme}: neighbouring segments differ by 3:1`, () => {
      for (let i = 1; i < fills.length; i++) {
        expect(
          contrastRatio(t[fills[i - 1]], t[fills[i]]),
          `${order[i - 1]} next to ${order[i]}`,
        ).toBeGreaterThanOrEqual(3);
      }
    });
  }
  it("separates segments with a gap in the track colour", () => {
    expect(savings).toMatch(/\.split-bar\s*\{[^}]*\bgap:/);
  });
});

describe("components", () => {
  const files = svelteFiles(SRC);
  const offenders = (pattern: RegExp) =>
    files.filter((f) => pattern.test(readFileSync(f, "utf8"))).map((f) => relative(SRC, f));

  it("never use the tertiary text colour", () => {
    expect(offenders(/--ps-text-tertiary/)).toEqual([]);
  });
  it("never use the warning or success colour as a text colour", () => {
    expect(offenders(/(^|[^-])color:\s*var\(--ps-(warning|success)\)/m)).toEqual([]);
  });
  it("never put white text on a status colour, whichever property comes first", () => {
    const whiteOnStatus = (css: string) =>
      [...css.matchAll(/\{([^{}]*)\}/g)].some(
        ([, block]) =>
          /(^|[^-])color:\s*#fff(fff)?\s*;/m.test(block) &&
          /background(-color)?:\s*var\(--ps-(error|success|warning)\)/.test(block),
      );
    expect(files.filter((f) => whiteOnStatus(readFileSync(f, "utf8"))).map((f) => relative(SRC, f))).toEqual([]);
  });
  it("never hard-code a hex colour as a text colour: it must go through a theme token", () => {
    expect(offenders(/(^|[^-])color:\s*#[0-9a-fA-F]{3,8}\b/m)).toEqual([]);
  });
  it("never hard-code a hex colour as a background colour: it must go through a theme token", () => {
    expect(offenders(/(^|[^-])background(-color)?:\s*#[0-9a-fA-F]{3,8}\b/m)).toEqual([]);
  });
  it("gives every category badge class the plain text colour on its tint", () => {
    // Binds the badge components to the pairings BADGE_TINTS checks above.
    const found = new Set<string>();
    for (const file of ["lib/FormatBadge.svelte", "lib/QualityBadge.svelte", "lib/ContentClassBadge.svelte"]) {
      const css = readFileSync(join(SRC, file), "utf8");
      for (const cls of BADGE_CLASSES) {
        const block = new RegExp(`\\.${cls}(?:\\s*,\\s*\\.[\\w-]+)*\\s*\\{([^}]*)\\}`).exec(css);
        if (block === null) continue; // another component's class
        found.add(cls);
        expect(block[1], `${file} .${cls}`).toContain("color: var(--ps-text);");
      }
    }
    expect(found.size).toBe(BADGE_CLASSES.length); // no class silently dropped
  });
});
