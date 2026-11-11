// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Text in the console meets WCAG AA (4.5:1) in both colour schemes, and no
// component uses a colour token as text that cannot meet it.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { colorDistance, contrastRatio, themeTokens, type Vision } from "./utils/contrast";

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

// --ps-muted-opacity per scheme: the first declaration is the light value,
// the second the dark override.
const MUTED_OPACITY = (() => {
  const values = [...css.matchAll(/--ps-muted-opacity:\s*([\d.]+)\s*;/g)].map((m) => Number(m[1]));
  return { light: values[0], dark: values[1] ?? values[0] };
})();

// Dark mode's planes, as pairs that sit next to each other on screen, with
// the least contrast between them that still reads as two planes. A frame
// is a line, not an area, so it needs more.
const DARK_PLANES: ReadonlyArray<readonly [name: string, a: string, b: string, floor: number]> = [
  ["page and sidebar", "--ps-bg", "--ps-bg-secondary", 1.1],
  ["page and card", "--ps-bg", "--ps-surface", 1.1],
  ["card and zebra row", "--ps-surface", "--ps-bg-secondary", 1.1],
  ["card and header band", "--ps-surface", "--ps-bg-tertiary", 1.1],
  ["card and row divider", "--ps-surface", "--ps-border-light", 1.1],
  ["card and hovered row", "--ps-surface", "--ps-surface-hover", 1.1],
  ["card and frame", "--ps-surface", "--ps-border", 1.4],
];

const CHART_SERIES = ["--ps-chart-1", "--ps-chart-2", "--ps-chart-3", "--ps-chart-4"];
const VISIONS: readonly Vision[] = ["normal", "deuteranope", "protanope"];
// Two thin lines closer than this read as one colour.
const SERIES_APART = 20;

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

describe("colorDistance", () => {
  it("is the CIE76 distance: 0 for one colour, 100 from black to white", () => {
    expect(colorDistance("#1d4ed8", "#1d4ed8")).toBe(0);
    for (const vision of ["normal", "deuteranope", "protanope"] as const) {
      expect(colorDistance("#000000", "#ffffff", vision)).toBeCloseTo(100, 0);
    }
  });
  it("brings red and green together for a red-green colour-blind reader", () => {
    const [red, green] = ["#d93025", "#1e8e3e"];
    const normal = colorDistance(red, green);
    expect(normal).toBeGreaterThan(80);
    expect(colorDistance(red, green, "deuteranope")).toBeLessThan(normal / 2);
    expect(colorDistance(red, green, "protanope")).toBeLessThan(normal / 2);
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
    it(`${scheme}: text passes AA on a hovered row or control`, () => {
      for (const fg of ["--ps-text", "--ps-text-secondary"]) {
        expect(contrastRatio(t[fg], t["--ps-surface-hover"]), fg).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: the current navigation item's text passes AA on its tint`, () => {
      expect(contrastRatio(t["--ps-primary"], t["--ps-primary-light"])).toBeGreaterThanOrEqual(AA);
    });
    it(`${scheme}: every text token passes AA on the muted card, dimmed as it is drawn`, () => {
      // The card is --ps-bg-secondary at --ps-muted-opacity over the page,
      // text and background alike.
      const percent = MUTED_OPACITY[scheme] * 100;
      const bg = mix(t["--ps-bg-secondary"], percent, t["--ps-bg"]);
      for (const fg of TEXT) {
        expect(contrastRatio(mix(t[fg], percent, t["--ps-bg"]), bg), fg).toBeGreaterThanOrEqual(AA);
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

describe("dark planes", () => {
  const t = tokens.dark;
  it("dark planes stay apart", () => {
    for (const [name, a, b, floor] of DARK_PLANES) {
      expect(contrastRatio(t[a], t[b]), name).toBeGreaterThanOrEqual(floor);
    }
  });
  it("the planes get lighter in one order: page, sidebar, card, band, hover, frame", () => {
    const order = ["--ps-bg", "--ps-bg-secondary", "--ps-surface", "--ps-bg-tertiary", "--ps-surface-hover", "--ps-border"];
    const light = order.map((token) => contrastRatio(t[token], "#000000"));
    expect([...light].sort((x, y) => x - y)).toEqual(light);
  });
  it("secondary text is quieter than primary text, on every plane", () => {
    for (const bg of ["--ps-bg", "--ps-bg-secondary", "--ps-surface", "--ps-bg-tertiary", "--ps-surface-hover"]) {
      const [primary, secondary] = ["--ps-text", "--ps-text-secondary"].map((fg) => contrastRatio(t[fg], t[bg]));
      expect(primary / secondary, bg).toBeGreaterThanOrEqual(1.5);
    }
  });
});

// The chart palette. Canvas colours are invisible to the browser checks,
// so every property a series colour needs is pinned here.
describe("chart palette", () => {
  for (const scheme of ["light", "dark"] as const) {
    const t = tokens[scheme];
    it(`${scheme}: every series and the grid is a colour the test can read`, () => {
      for (const token of [...CHART_SERIES, "--ps-chart-grid"]) {
        expect(t[token], token).toMatch(/^#[0-9a-f]{6}$/i);
      }
    });
    it(`${scheme}: every series passes 3:1 on the card and the muted card`, () => {
      for (const series of CHART_SERIES) {
        for (const bg of ["--ps-surface", "--ps-bg-secondary"]) {
          expect(contrastRatio(t[series], t[bg]), `${series} on ${bg}`).toBeGreaterThanOrEqual(3);
        }
      }
    });
    it(`${scheme}: every pair of series stays apart, also for red-green colour-blind readers`, () => {
      for (const vision of VISIONS) {
        for (const [i, a] of CHART_SERIES.entries()) {
          for (const b of CHART_SERIES.slice(i + 1)) {
            expect(colorDistance(t[a], t[b], vision), `${a} and ${b}, ${vision}`).toBeGreaterThanOrEqual(SERIES_APART);
          }
        }
      }
    });
    it(`${scheme}: no series can be taken for a status colour`, () => {
      for (const series of CHART_SERIES) {
        for (const status of ["--ps-success", "--ps-warning", "--ps-error"]) {
          expect(colorDistance(t[series], t[status]), `${series} and ${status}`).toBeGreaterThanOrEqual(SERIES_APART);
        }
      }
    });
    it(`${scheme}: the grid is fainter than every series`, () => {
      const grid = contrastRatio(t["--ps-chart-grid"], t["--ps-surface"]);
      for (const series of CHART_SERIES) {
        expect(grid, series).toBeLessThan(contrastRatio(t[series], t["--ps-surface"]));
      }
    });
  }
});

describe("type roles", () => {
  const role = (name: string) => new RegExp(`--ps-type-${name}:\\s*([^;]+);`).exec(css)?.[1];
  const rem = (name: string) => {
    const value = role(name);
    expect(value, name).toMatch(/^[\d.]+rem$/);
    return parseFloat(value as string);
  };
  it("declares a size for every role, in rem", () => {
    for (const name of ["title", "section", "card-title", "body", "label", "metric", "hero-metric"]) {
      expect(rem(`${name}-size`), name).toBeGreaterThan(0);
    }
  });
  it("orders the roles: label, body, card title, section, metric", () => {
    const sizes = ["label", "body", "card-title", "section", "metric"].map((name) => rem(`${name}-size`));
    expect([...sizes].sort((a, b) => a - b)).toEqual(sizes);
    expect(rem("hero-metric-size")).toBeGreaterThan(rem("metric-size"));
    expect(rem("title-size")).toBeGreaterThan(rem("section-size"));
  });
  it("body text is 14 px and labels are 12 px", () => {
    expect(rem("body-size")).toBe(0.875);
    expect(rem("label-size")).toBe(0.75);
  });
  it("the metric classes take their size from the roles", () => {
    expect(css).toMatch(/\.metric-value\s*\{[^}]*font-size:\s*var\(--ps-type-metric-size\)/);
    expect(css).toMatch(/\.metric-label\s*\{[^}]*font-size:\s*var\(--ps-type-label-size\)/);
  });
  it("the text face is the system's own: no web font is named", () => {
    expect(css).toMatch(/--ps-font-family:\s*system-ui,/);
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

// The shared notice: its text on its own background, and the leading rule
// that carries the state. The rule is a mark, not text, so it needs 3:1
// against the notice and against the page and card the notice sits on.
describe("notices", () => {
  const declared = (selector: string, property: string): string => {
    const block = new RegExp(`\\${selector}\\s*\\{([^}]*)\\}`).exec(css);
    const token = block && new RegExp(`(?:^|[;\\s])${property}:[^;]*var\\((--ps-[\\w-]+)\\)`).exec(block[1]);
    expect(token, `${selector} ${property} token`).not.toBeNull();
    return (token as RegExpExecArray)[1];
  };
  const background = declared(".notice", "background");
  const text = declared(".notice", "color");
  const variants = [".notice", ".notice-info", ".notice-warning", ".notice-error"];
  const rules = variants.map((v) => declared(v, v === ".notice" ? "border-inline-start" : "border-inline-start-color"));
  for (const scheme of ["light", "dark"] as const) {
    const t = tokens[scheme];
    it(`${scheme}: notice text and links pass AA on the notice`, () => {
      for (const fg of [text, "--ps-text-secondary", "--ps-primary"]) {
        expect(contrastRatio(t[fg], t[background]), `${fg} on ${background}`).toBeGreaterThanOrEqual(AA);
      }
    });
    it(`${scheme}: every notice's leading rule is visible on the notice and around it`, () => {
      for (const [i, rule] of rules.entries()) {
        for (const bg of [background, "--ps-bg", "--ps-surface"]) {
          expect(contrastRatio(t[rule], t[bg]), `${variants[i]} rule (${rule}) on ${bg}`).toBeGreaterThanOrEqual(3);
        }
      }
    });
  }
  it("never draws the rule in a status fill colour", () => {
    // The warning and success fills are too light to see on a light notice;
    // the rule uses the colours that pass as text.
    expect(rules).not.toContain("--ps-warning");
    expect(rules).not.toContain("--ps-success");
  });
});

// Forced colours replace author colours at paint time. The marks that
// survive are declared once, on state attributes and shared classes, so a
// page restyle cannot drop them (e2e/forced-colors.mock.spec.ts checks the
// painted result).
describe("forced colours", () => {
  it("has one block, in the theme, keyed on state and shared classes", () => {
    expect(css.match(/@media\s*\(forced-colors:\s*active\)/g)).toHaveLength(1);
    const block = css.slice(css.search(/@media\s*\(forced-colors:\s*active\)/));
    for (const selector of ['[aria-current="page"]', '[aria-selected="true"]', '[aria-pressed="true"]', ".bar-fill", ".status-dot"]) {
      expect(block, selector).toContain(selector);
    }
  });
  it("no component declares its own forced-colours rules", () => {
    const own = svelteFiles(SRC)
      .filter((f) => /@media[^{]*forced-colors/.test(readFileSync(f, "utf8")))
      .map((f) => relative(SRC, f));
    expect(own).toEqual([]);
  });
});

// The savings split's stacked bar: each share is a .bar-fill whose colour
// must stand out from the bar's track (which also shows as the gap between
// two shares) and from the card behind it, the muted per-host card
// included. Two shares are told apart by that gap, not by their hues: the
// fills are close in lightness, and a forced palette paints them alike.
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
    it(`${scheme}: every share passes 4.5:1 on the track and the card`, () => {
      for (const [i, fill] of fills.entries()) {
        for (const bg of [track, "--ps-surface", "--ps-bg-secondary"]) {
          expect(contrastRatio(t[fill], t[bg]), `${order[i]} (${fill}) on ${bg}`).toBeGreaterThanOrEqual(AA);
        }
      }
    });
    it(`${scheme}: no share is drawn in the text colour, and no two share a fill`, () => {
      expect(fills).not.toContain("--ps-text");
      expect(new Set(fills.map((fill) => t[fill])).size).toBe(fills.length);
    });
  }
  it("separates two shares with a gap in the track colour", () => {
    expect(savings).toMatch(/\.split-bar\s*\{[^}]*\bgap:\s*[1-9]/);
  });
  it("draws every share as a .bar-fill, the mark the forced-colours rules keep painted", () => {
    expect(savings).toMatch(/class="bar-fill split-\{seg\.key\}"/);
    expect(readFileSync(join(SRC, "lib/theme.css"), "utf8")).toMatch(
      /@media \(forced-colors: active\)[\s\S]*\.bar-fill[^{]*\{[^}]*forced-color-adjust:\s*none/,
    );
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
