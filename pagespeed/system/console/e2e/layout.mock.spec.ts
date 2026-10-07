// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The shell's geometry: the sidebar band, the top bar's spacing, the page
// toolbars, and how pages use a wide window. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";
import { box, noHorizontalScroll, pageReady, perHostReplies, scrollTo, sidewaysScrollers, viewport, wholeServerReplies, widenText } from "./support/layout";

test.use({ locale: "en-US", timezoneId: "UTC" });

const DESKTOPS = [
  { width: 1440, height: 900 },
  { width: 1920, height: 1080 },
] as const;

/** The sidebar runs from the top bar's bottom edge to the viewport's bottom edge. */
async function expectFullHeightSidebar(page: Page): Promise<void> {
  const bar = await box(page.locator(".topbar"));
  const nav = await box(page.locator("#console-nav"));
  const { height } = await viewport(page);
  expect(Math.abs(nav.top - bar.bottom), `sidebar top ${nav.top}, top bar bottom ${bar.bottom}`).toBeLessThanOrEqual(1);
  expect(Math.abs(nav.bottom - height), `sidebar bottom ${nav.bottom}, viewport ${height}`).toBeLessThanOrEqual(1);
}

test.describe("the sidebar band", () => {
  for (const size of DESKTOPS) {
    test(`fills the height below the top bar on a short page at ${size.width}`, async ({ page }) => {
      await page.setViewportSize(size);
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", "#/about");
      await pageReady(page);
      await expectFullHeightSidebar(page);
    });

    test(`fills the height below the top bar anywhere on a long page at ${size.width}`, async ({ page }) => {
      await page.setViewportSize(size);
      // Many counters: a page several windows long at any width.
      const variables: Record<string, number> = { ...(F.STATS_GLOBAL.body as { variables: Record<string, number> }).variables };
      for (let i = 0; i < 120; i++) variables[`num_cache_counter_${i}`] = i;
      await mockAdmin(page, "pagespeed_global_admin", { ...wholeServerReplies(), stats_json: F.ok({ variables, maxlength: 60, timestamp_ms: 1_790_000_000_000 }) });
      await openConsole(page, "pagespeed_global_admin", "#/statistics");
      await pageReady(page);
      await expect(page.locator("main details.stat-group").first()).toBeVisible();
      const scrollable = await page.evaluate(
        () => document.scrollingElement!.scrollHeight - document.scrollingElement!.clientHeight,
      );
      expect(scrollable, "the page must be longer than the window").toBeGreaterThan(100);
      for (const fraction of [0, 0.5, 1]) {
        await scrollTo(page, fraction);
        await expectFullHeightSidebar(page);
      }
    });
  }

  test("fills the height while a page is still loading", async ({ page }) => {
    await page.setViewportSize(DESKTOPS[0]);
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies(), { delayMs: { stats_json: 3000 } });
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await expect(page.locator("main .loading").first()).toBeVisible();
    await expectFullHeightSidebar(page);
  });

  test("the phone drawer reaches the bottom of the screen when open", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/about");
    await pageReady(page);
    await page.getByRole("button", { name: "Toggle menu" }).click();
    await expect(page.locator("#console-nav")).toHaveCSS("transform", "matrix(1, 0, 0, 1, 0, 0)");
    await expectFullHeightSidebar(page);
  });
});

test.describe("the top bar's spacing", () => {
  const GROUP = 16; // between groups
  const ITEM = 8; // between items of a group, and the least anywhere

  /** The left-to-right distance between two neighbours. */
  async function gap(page: Page, left: string, right: string): Promise<number> {
    const a = await box(page.locator(left));
    const b = await box(page.locator(right));
    return b.left - a.right;
  }

  for (const width of [1440, 1024]) {
    test(`separates groups and items by the spacing scale at ${width}`, async ({ page }) => {
      await page.setViewportSize({ width, height: 800 });
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", "#/overview");
      await pageReady(page);
      expect(await gap(page, ".topbar-logo-link", ".topbar-subtitle"), "logo to the version line").toBeCloseTo(GROUP, 0);
      expect(await gap(page, ".topbar-subtitle-link", ".topbar-badge"), "host name to the console label").toBeCloseTo(GROUP, 0);
      expect(await gap(page, ".topbar-badge", ".shortcuts-button"), "console label to the shortcuts button").toBeCloseTo(ITEM, 0);
      const line = await page.locator(".topbar-subtitle > *").evaluateAll((els) =>
        els.slice(1).map((el, i) => el.getBoundingClientRect().left - els[i].getBoundingClientRect().right),
      );
      for (const g of line) expect(g, `gaps in the version line: ${line.join(", ")}`).toBeGreaterThanOrEqual(ITEM - 1);
      const lens = page.getByTestId("host-lens");
      expect(await gap(page, '[data-testid="host-lens"] label', '[data-testid="host-lens"] select'), "Host label to its select").toBeCloseTo(ITEM, 0);
      // The lens ends at the top bar's padding, and that padding is the content's.
      const [barPad, contentPad] = await page.evaluate(() => [
        parseFloat(getComputedStyle(document.querySelector(".topbar")!).paddingRight),
        parseFloat(getComputedStyle(document.querySelector("main.content")!).paddingRight),
      ]);
      expect(barPad).toBe(contentPad);
      const bar = await box(page.locator(".topbar"));
      expect(Math.abs(bar.right - barPad - (await box(lens)).right)).toBeLessThanOrEqual(1);
    });
  }

  test("nothing overlaps or is cut short at the narrowest desktop width", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await page.setViewportSize({ width: 1440, height: 800 });
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await pageReady(page);
    const select = page.getByTestId("host-lens").locator("select");
    const wide = (await box(select)).width;
    await page.setViewportSize({ width: 769, height: 800 });
    const parts = await page
      .locator(".topbar-logo-link, .topbar-subtitle > *, .topbar-badge, .shortcuts-button, .topbar-lens")
      .evaluateAll((els) =>
        els
          .filter((el) => el.getClientRects().length > 0)
          .map((el) => {
            const r = el.getBoundingClientRect();
            return { name: el.className, left: r.left, right: r.right, top: r.top, bottom: r.bottom };
          }),
      );
    for (let i = 1; i < parts.length; i++) {
      expect(parts[i].left, `${parts[i - 1].name} | ${parts[i].name}`).toBeGreaterThanOrEqual(parts[i - 1].right + ITEM - 1);
      expect(parts[i].top, `${parts[i].name} stays on the line`).toBeLessThan(parts[0].bottom);
    }
    const badge = page.locator(".topbar-badge");
    expect(await badge.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    expect((await box(select)).width).toBeCloseTo(wide, 0);
  });
});

interface Item {
  what: string;
  left: number;
  right: number;
  top: number;
  height: number;
  badge: boolean;
  group: boolean;
  /** For a group: its own items. */
  items: Item[];
}

/** The visible items of a row or group: controls, or groups of controls (legends are labels, not items). */
async function itemsOf(page: Page, selector: string): Promise<Item[]> {
  const loc = page.locator(selector).first();
  await expect(loc).toBeVisible();
  return loc.evaluate((el) => {
    const read = (parent: Element): Item[] =>
      [...parent.children]
        .filter((c) => c.getClientRects().length > 0 && getComputedStyle(c).position !== "absolute" && c.tagName !== "LEGEND")
        .map((c) => {
          const r = c.getBoundingClientRect();
          const group = c.classList.contains("control-group");
          return {
            what: `${c.tagName.toLowerCase()}.${c.className}`.trim(),
            left: r.left,
            right: r.right,
            top: r.top,
            height: r.height,
            badge: /badge/.test(c.className),
            group,
            items: group ? read(c) : [],
          };
        });
    return read(el);
  });
}

/** Neighbours on one line are `gap` apart. */
/**
 * Neighbours on one line are `gap` apart; when the items wrap, each line
 * starts `gap` below the bottom of the line above it.
 */
function expectGaps(where: string, items: Item[], gap: number): void {
  const lines: Item[][] = [];
  for (const item of items) {
    const line = lines[lines.length - 1];
    if (line && Math.abs(line[0].top - item.top) <= Math.min(line[0].height, item.height) / 2) line.push(item);
    else lines.push([item]);
  }
  for (const line of lines) {
    for (let k = 1; k < line.length; k++) {
      const [a, b] = [line[k - 1], line[k]];
      expect(Math.abs(b.left - a.right - gap), `${where}: ${a.what} | ${b.what} gap ${b.left - a.right}, expected ${gap}`).toBeLessThanOrEqual(1);
    }
  }
  for (let k = 1; k < lines.length; k++) {
    const above = Math.max(...lines[k - 1].map((i) => i.top + i.height));
    const below = Math.min(...lines[k].map((i) => i.top));
    expect(Math.abs(below - above - gap), `${where}: line ${k} starts ${below - above} below the line above, expected ${gap}`).toBeLessThanOrEqual(1);
  }
}

const GROUP_GAP = 16;
const ITEM_GAP = 8;

/**
 * A row of controls: groups (a label and its controls) sit GROUP_GAP
 * apart, the items of a group ITEM_GAP apart, and every control in the row
 * has one height (status badges keep their own).
 */
async function expectGroupedRow(page: Page, row: string): Promise<void> {
  const items = await itemsOf(page, row);
  expect(items.length, `${row} has controls`).toBeGreaterThan(0);
  expectGaps(row, items, GROUP_GAP);
  for (const g of items.filter((i) => i.group)) expectGaps(`${row} > ${g.what}`, g.items, ITEM_GAP);
  const leaves = items.flatMap((i) => (i.group ? i.items : [i])).filter((i) => !i.badge);
  const heights = leaves.map((c) => `${c.what}=${Math.round(c.height)}`).join(", ");
  for (const i of leaves) {
    expect(Math.abs(i.height - leaves[0].height), `${row} heights: ${heights}`).toBeLessThanOrEqual(1);
  }
}

/** A single group of controls: its items ITEM_GAP apart, one height. */
async function expectGroup(page: Page, group: string): Promise<void> {
  const items = await itemsOf(page, group);
  expectGaps(group, items, ITEM_GAP);
  const leaves = items.filter((i) => !i.badge);
  for (const i of leaves) expect(Math.abs(i.height - leaves[0].height), `${group} heights`).toBeLessThanOrEqual(1);
}

test.describe("rows of controls", () => {
  // Every page's rows of controls: the header toolbar, and the filter and
  // pager rows below it.
  const ROWS: Array<[string, string, string[], string[]]> = [
    ["Overview", "#/overview", [".page-toolbar"], []],
    ["Savings", "#/savings", [".page-toolbar"], []],
    ["URLs", "#/urls", [".page-toolbar"], ['[data-testid="urls-pager"]']],
    ["URL Detail", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https", [".page-toolbar"], []],
    ["Statistics", "#/statistics", [".page-toolbar"], []],
    ["Graphs", "#/graphs", [".page-toolbar"], []],
    ["Histograms", "#/histograms", [".page-toolbar"], []],
    ["Caches", "#/caches", [".page-toolbar"], []],
    ["Configuration", "#/configuration", [".page-toolbar"], []],
    ["Optimizer status", "#/optimizer", [".page-toolbar"], []],
    ["Logs", "#/logs", [".page-toolbar", "main .controls"], ['[data-testid="logs-source-filter"]', '[data-testid="logs-level-filter"]']],
  ];

  for (const width of [1440, 1920]) for (const [name, hash, rows, groups] of ROWS) {
    test(`${name} at ${width}: groups of controls are a step apart, controls within a group half a step, one height`, async ({ page }) => {
      await page.setViewportSize({ width, height: 900 });
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", hash);
      await pageReady(page);
      for (const row of rows) await expectGroupedRow(page, row);
      for (const group of groups) await expectGroup(page, group);
    });
  }

  test("on a phone, a group that wraps keeps its lines close and groups stack a step apart", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await pageReady(page);
    const level = await itemsOf(page, '[data-testid="logs-level-filter"]');
    const lines = [...new Set(level.map((i) => Math.round(i.top)))].sort((a, b) => a - b);
    expect(lines.length, "the level filter wraps on a phone").toBeGreaterThan(1);
    const lineBottom = (top: number) => Math.max(...level.filter((i) => Math.round(i.top) === top).map((i) => i.top + i.height));
    for (let k = 1; k < lines.length; k++) {
      expect(Math.abs(lines[k] - lineBottom(lines[k - 1]) - ITEM_GAP), `line gap ${lines[k] - lineBottom(lines[k - 1])}`).toBeLessThanOrEqual(1);
    }
    const groups = await itemsOf(page, "main .controls");
    for (let k = 1; k < groups.length; k++) {
      const [a, b] = [groups[k - 1], groups[k]];
      if (b.top > a.top + a.height / 2) {
        expect(Math.abs(b.top - (a.top + a.height) - GROUP_GAP), `${a.what} / ${b.what} stack ${b.top - a.top - a.height} apart`).toBeLessThanOrEqual(1);
      }
    }
  });

  test("the purge row and the comparison modes follow the same rhythm", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", { ...wholeServerReplies(), cache: F.CACHE_STRUCTURE_PURGE_ON });
    await openConsole(page, "pagespeed_global_admin", "#/caches");
    await pageReady(page);
    await page.getByRole("tab", { name: "Purge", exact: true }).click();
    await expectGroup(page, ".purge-all-row");
    await openConsole(page, "pagespeed_global_admin", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https");
    await pageReady(page);
    await page.getByTestId("compare-1").click();
    await expectGroup(page, ".compare-modes");
  });
});

test.describe("wide windows", () => {
  const PAGES: Array<[string, string]> = [
    ["Overview", "#/overview"],
    ["Savings", "#/savings"],
    ["URLs", "#/urls"],
    ["Statistics", "#/statistics"],
    ["Graphs", "#/graphs"],
    ["Logs", "#/logs"],
    ["Optimizer status", "#/optimizer"],
    ["Configuration", "#/configuration"],
  ];

  for (const size of [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ]) {
    for (const [name, hash] of PAGES) {
      test(`${name} uses the full width at ${size.width}`, async ({ page }) => {
        await page.setViewportSize(size);
        await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
        await openConsole(page, "pagespeed_global_admin", hash);
        await pageReady(page);
        const lens = await box(page.getByTestId("host-lens"));
        const content = await box(page.locator("main .page").first());
        expect(Math.abs(content.right - lens.right), `page ends at ${content.right}, the Host selector at ${lens.right}`).toBeLessThanOrEqual(1);
        const lastControl = await box(page.locator(".page-toolbar > :last-child").first());
        expect(Math.abs(lastControl.right - lens.right), "the toolbar's last control lines up with the Host selector").toBeLessThanOrEqual(1);
        expect(await noHorizontalScroll(page)).toBe(true);
        // Running text keeps a readable measure: at most 80 characters of its font.
        const wide = await page.locator("main p").evaluateAll((ps) =>
          ps
            .filter((p) => p.getClientRects().length > 0)
            .map((p) => {
              const probe = document.createElement("span");
              probe.style.cssText = "position:absolute;visibility:hidden;width:80ch";
              p.appendChild(probe);
              const limit = probe.getBoundingClientRect().width;
              probe.remove();
              return { text: (p.textContent ?? "").trim().slice(0, 40), width: p.getBoundingClientRect().width, limit };
            })
            .filter((p) => p.width > p.limit + 1),
        );
        expect(wide, "paragraphs wider than 80 characters").toEqual([]);
      });
    }
  }
  for (const width of [1440, 1920, 2560]) {
    for (const hash of ["#/overview", "#/savings"]) {
      test(`the card row of ${hash} ends at the Host selector at ${width}`, async ({ page }) => {
        await page.setViewportSize({ width, height: 900 });
        await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
        await openConsole(page, "pagespeed_global_admin", hash);
        await pageReady(page);
        const lens = await box(page.getByTestId("host-lens"));
        const rights = await page.locator("main .cards > .card, main .chart-card").evaluateAll((els) => els.map((el) => el.getBoundingClientRect().right));
        expect(rights.length).toBeGreaterThan(1);
        const rowRight = Math.max(...rights);
        expect(Math.abs(rowRight - lens.right), `cards end at ${rowRight}, the Host selector at ${lens.right}`).toBeLessThanOrEqual(1);
        const chart = page.locator("main .chart-card");
        if ((await chart.count()) > 0) expect(Math.abs((await box(chart)).right - lens.right), "the chart spans the width").toBeLessThanOrEqual(1);
      });
    }
  }

  // A chart's plot keeps sensible proportions: width over height within this range.
  const PLOT_RATIO = { min: 1.8, max: 4.5 };

  for (const size of [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ]) {
    test(`the default charts on Graphs fill the row and keep their proportions at ${size.width}`, async ({ page }) => {
      await page.setViewportSize(size);
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", "#/graphs");
      await pageReady(page);
      const lens = await box(page.getByTestId("host-lens"));
      const cards = page.locator("main .curated .charts > .card");
      await expect(cards.first().locator(".ts-chart .u-over")).toHaveCount(1);
      const right = Math.max(...(await cards.evaluateAll((els) => els.map((el) => el.getBoundingClientRect().right))));
      expect(Math.abs(right - lens.right), `charts end at ${right}, the Host selector at ${lens.right}`).toBeLessThanOrEqual(1);
      await expect
        .poll(async () =>
          (await page.locator("main .curated .ts-chart").evaluateAll((els) =>
            els.map((el) => el.getBoundingClientRect().width / el.getBoundingClientRect().height),
          )).every((r) => r >= PLOT_RATIO.min && r <= PLOT_RATIO.max),
        )
        .toBe(true);
    });
  }

  test("a wide window holds at most three charts per row", async ({ page }) => {
    await page.setViewportSize({ width: 2560, height: 1440 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/graphs?counters=all");
    await pageReady(page);
    const cards = page.locator("main .charts > .card");
    await expect(cards.nth(3)).toBeVisible();
    const tops = await cards.evaluateAll((els) => els.map((el) => Math.round(el.getBoundingClientRect().top)));
    const perRow = Math.max(...[...new Set(tops)].map((t) => tops.filter((x) => x === t).length));
    expect(perRow).toBeLessThanOrEqual(3);
  });

  for (const size of [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ]) test(`the cache cards leave no hole between them at ${size.width}`, async ({ page }) => {
    await page.setViewportSize(size);
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/caches");
    await pageReady(page);
    const list = await box(page.locator("main .cache-list"));
    const cards = await page.locator("main .cache-list > .cache-card").evaluateAll((els) =>
      els.map((el) => {
        const r = el.getBoundingClientRect();
        return { left: Math.round(r.left), top: r.top, bottom: r.bottom };
      }),
    );
    expect(cards.length).toBeGreaterThan(2);
    const GAP = 16;
    for (const left of new Set(cards.map((c) => c.left))) {
      const column = cards.filter((c) => c.left === left).sort((a, b) => a.top - b.top);
      expect(Math.abs(column[0].top - list.top), "each column starts at the top").toBeLessThanOrEqual(1);
      for (let k = 1; k < column.length; k++) {
        expect(column[k].top - column[k - 1].bottom, "gap to the next card").toBeLessThanOrEqual(GAP + 1);
      }
    }
  });

  test("a wide card puts its headline figures side by side, and cards keep their own height", async ({ page }) => {
    await page.setViewportSize({ width: 2560, height: 1440 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await pageReady(page);
    const figures = await page.getByTestId("module-card").locator(".figure").evaluateAll((els) => els.map((el) => el.getBoundingClientRect().top));
    expect(figures.length).toBeGreaterThan(1);
    expect(Math.abs(figures[0] - figures[1]), "the first two figures share a row").toBeLessThanOrEqual(1);
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await pageReady(page);
    const [cardBottom, contentBottom, pad] = await page.getByTestId("savings-module-card").evaluate((card) => {
      const kids = [...card.children].filter((c) => c.getClientRects().length > 0);
      const cs = getComputedStyle(card);
      return [
        card.getBoundingClientRect().bottom,
        Math.max(...kids.map((k) => k.getBoundingClientRect().bottom + parseFloat(getComputedStyle(k).marginBottom))),
        parseFloat(cs.paddingBottom) + parseFloat(cs.borderBottomWidth),
      ];
    });
    expect(cardBottom - contentBottom - pad, "the module card is no taller than its content").toBeLessThanOrEqual(1);
  });

  for (const hash of ["#/overview", "#/savings"]) {
    test(`the two cards of ${hash} sit side by side on a laptop window`, async ({ page }) => {
      await page.setViewportSize({ width: 1440, height: 900 });
      await mockAdmin(page, "pagespeed_admin", perHostReplies());
      await openConsole(page, "pagespeed_admin", hash);
      await pageReady(page);
      const cards = await page.locator("main .cards > .card").all();
      expect(cards).toHaveLength(2);
      const [a, b] = [await box(cards[0]), await box(cards[1])];
      expect(Math.abs(a.top - b.top)).toBeLessThanOrEqual(1);
      expect(b.left).toBeGreaterThan(a.right);
    });
  }
});

test.describe("grouped tables", () => {
  for (const size of [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ]) test(`statistics groups flow into columns at ${size.width}, values aligned under their header`, async ({ page }) => {
    await page.setViewportSize(size);
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await pageReady(page);
    const groups = await page.locator("main details.stat-group").evaluateAll((els) =>
      els.map((el) => {
        const r = el.getBoundingClientRect();
        const th = el.querySelectorAll("thead th")[1]!;
        const header = th.querySelector("button")!.getBoundingClientRect();
        const cell = el.querySelector("td.value-cell")!;
        const range = document.createRange();
        range.selectNodeContents(cell);
        const text = range.getBoundingClientRect();
        return { left: r.left, top: r.top, thLeft: th.getBoundingClientRect().left, headerRight: header.right, cellRight: text.right };
      }),
    );
    expect(groups.length).toBeGreaterThan(2);
    const sharesRow = groups.some((g, i) => groups.some((h, j) => i !== j && Math.abs(g.top - h.top) <= 1));
    expect(sharesRow, "at least two groups share a row").toBe(true);
    for (const g of groups) {
      expect(Math.abs(g.headerRight - g.cellRight), `Value header ends at ${g.headerRight}, its values at ${g.cellRight}`).toBeLessThanOrEqual(1);
    }
    // The Value column has one width: its header sits at one x per column of groups.
    for (const g of groups) {
      for (const h of groups.filter((o) => Math.abs(o.left - g.left) <= 1)) {
        expect(Math.abs(h.thLeft - g.thLeft)).toBeLessThanOrEqual(1);
      }
    }
  });
});

/**
 * In every column whose values are right-aligned, the header ends where
 * the values end (±1 px): a header sits over its numbers.
 */
async function expectHeadersOverNumbers(page: Page, table: string): Promise<void> {
  const misaligned = await page.locator(table).first().evaluate((t) => {
    const textRight = (el: Element) => {
      const range = document.createRange();
      range.selectNodeContents(el);
      return range.getBoundingClientRect().right;
    };
    const heads = [...t.querySelectorAll("thead th")];
    const row = t.querySelector("tbody tr");
    const cells = row ? [...row.querySelectorAll("td, th")] : [];
    const out: string[] = [];
    cells.forEach((cell, i) => {
      const head = heads[i];
      if (!head || getComputedStyle(cell).textAlign !== "right" || (cell.textContent ?? "").trim() === "") return;
      const [h, c] = [textRight(head), textRight(cell)];
      if (Math.abs(h - c) > 1) out.push(`${(head.textContent ?? "").trim()}: header ${h.toFixed(1)}, values ${c.toFixed(1)}`);
    });
    return out;
  });
  expect(misaligned, `${table}: headers not over their numbers`).toEqual([]);
}

test.describe("numeric column headers", () => {
  for (const width of [1440, 2560]) {
    test(`sit over their numbers on Histograms and URL detail at ${width}`, async ({ page }) => {
      await page.setViewportSize({ width, height: 1000 });
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", "#/histograms");
      await pageReady(page);
      await expectHeadersOverNumbers(page, "main table.histogram-table");
      await expectHeadersOverNumbers(page, "main table.detail-table");
      await openConsole(page, "pagespeed_global_admin", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https");
      await pageReady(page);
      await expectHeadersOverNumbers(page, 'main [data-testid="variants-table"]');
    });
  }
});

test.describe("grouped tables on a phone", () => {
  test("the statistics name column keeps most of a phone's width", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await pageReady(page);
    const [name, table] = await page.locator("main details.stat-group table").first().evaluate((t) => [
      t.querySelector("thead th")!.getBoundingClientRect().width,
      t.getBoundingClientRect().width,
    ]);
    expect(name / table, `name column ${name} of ${table}`).toBeGreaterThanOrEqual(0.6);
  });
});

test.describe("the sidebar's rows", () => {
  test("the active row and a hovered row are highlighted across the whole row, shortcut hint included", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await pageReady(page);
    const nav = page.locator("#console-nav");
    const inner = await nav.evaluate((el) => {
      const r = el.getBoundingClientRect();
      return { left: r.left, right: r.right - parseFloat(getComputedStyle(el).borderRightWidth) };
    });
    for (const link of [page.locator(".nav-item.active"), page.locator(".nav-item", { hasText: "Histograms" })]) {
      await link.hover();
      const area = await box(link);
      expect(Math.abs(area.right - inner.right), `highlight ends at ${area.right}, the sidebar at ${inner.right}`).toBeLessThanOrEqual(1);
      expect(Math.abs(area.left - inner.left)).toBeLessThanOrEqual(1);
      const hint = await box(link.locator("xpath=following-sibling::span[contains(@class,'nav-key')]"));
      expect(hint.left).toBeGreaterThanOrEqual(area.left);
      expect(hint.right).toBeLessThanOrEqual(area.right);
      await expect.poll(() => link.evaluate((el) => getComputedStyle(el).backgroundColor)).not.toBe("rgba(0, 0, 0, 0)");
    }
    // The hint is not part of the link's name and is hidden from assistive technology.
    await expect(page.getByRole("link", { name: "Statistics", exact: true })).toBeVisible();
    await expect(page.locator(".nav-key").first()).toHaveAttribute("aria-hidden", "true");
  });
});

test.describe("buttons inside list and table rows", () => {
  const COMPACT = 28; // 1.75rem

  test("the findings' and the previews' row buttons share one compact size", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", { ...wholeServerReplies(), "v1/daemon/health": F.HEALTH_CHECK_FAILING });
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await pageReady(page);
    const ack = page.getByRole("button", { name: /^Acknowledge / }).first();
    await expect(ack).toBeVisible();
    const sizes = [await box(ack)];
    const fonts = [await ack.evaluate((el) => getComputedStyle(el).fontSize)];
    await openConsole(page, "pagespeed_global_admin", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https");
    await pageReady(page);
    sizes.push(await box(page.getByTestId("compare-1")));
    for (const b of sizes) expect(Math.abs(b.height - COMPACT), `row button height ${b.height}`).toBeLessThanOrEqual(1);
    fonts.push(await page.getByTestId("compare-1").evaluate((el) => getComputedStyle(el).fontSize));
    expect(new Set(fonts).size, `font sizes ${fonts.join(", ")}`).toBe(1);
  });
});

test.describe("findings", () => {
  test("a finding's button follows its text, and its log excerpt keeps the text's measure", async ({ page }) => {
    await page.setViewportSize({ width: 2560, height: 1440 });
    await mockAdmin(page, "pagespeed_global_admin", {
      ...wholeServerReplies(),
      message_history: F.messageGroups([
        { level: "warning", template: F.VOLUME_TEMPLATE, count: 1, recent: 1, last_ms: Date.now() - 60_000 },
      ]),
    });
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await pageReady(page);
    const findings = page.locator('[data-testid="alerts"] li.finding');
    await expect(findings.first()).toBeVisible();
    const cards = await box(page.locator("main .cards"));
    for (const finding of await findings.all()) {
      const row = await box(finding);
      expect(Math.abs(row.right - cards.right), "the finding bar lines up with the cards").toBeLessThanOrEqual(1);
      // The text block ends where its widest paragraph or list ends.
      const body = await finding.locator(".finding-body").evaluate((el) => ({
        right: Math.max(...[...el.children].map((c) => c.getBoundingClientRect().right)),
      }));
      const button = await box(finding.getByRole("button"));
      expect(button.left - body.right, "the button sits right after the text").toBeLessThanOrEqual(16 + 1);
      expect(button.left).toBeGreaterThanOrEqual(body.right);
      for (const line of await finding.locator(".finding-details li").all()) {
        expect((await box(line)).right).toBeLessThanOrEqual(body.right + 1);
      }
    }
    await expect(page.locator('[data-testid="alerts"] .finding-details li').first()).toBeVisible();
  });
});

/**
 * A table reads across: each column's text is followed closely by the next
 * column's (the spare width goes after the last column).
 */
async function expectReadsAcross(page: Page, table: string): Promise<void> {
  const gaps = await page.locator(table).first().evaluate((t) => {
    const textBox = (el: Element) => {
      const range = document.createRange();
      range.selectNodeContents(el);
      return range.getBoundingClientRect();
    };
    const rows = [...t.querySelectorAll("tr")].map((tr) => [...tr.children].filter((c) => (c.textContent ?? "").trim() !== ""));
    const cols = Math.max(...rows.map((r) => r.length));
    const out: Array<{ col: number; gap: number }> = [];
    for (let i = 0; i < cols - 1; i++) {
      const rights = rows.filter((r) => r.length === cols).map((r) => textBox(r[i]).right);
      const lefts = rows.filter((r) => r.length === cols).map((r) => textBox(r[i + 1]).left);
      if (rights.length === 0) continue;
      out.push({ col: i, gap: Math.min(...lefts) - Math.max(...rights) });
    }
    return out;
  });
  for (const g of gaps) expect(g.gap, `${table}: column ${g.col} to ${g.col + 1}`).toBeLessThanOrEqual(48);
}

test.describe("tables read across", () => {
  test("on a wide window short columns stay next to the text they belong to", async ({ page }) => {
    await page.setViewportSize({ width: 2560, height: 1440 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/urls");
    await pageReady(page);
    await expectReadsAcross(page, 'main [data-testid="urls-table"]');
    // ...without squeezing the URL itself: on a wide window each URL fits on one line.
    const lines = await page.locator('main [data-testid="urls-table"] tbody .url-cell a').evaluateAll((as) => as.map((a) => a.getClientRects().length));
    expect(lines.every((n) => n === 1), `URL lines: ${lines.join(", ")}`).toBe(true);
    // The note under the pager has room to breathe.
    const pager = await box(page.getByTestId("urls-pager"));
    const note = await box(page.getByTestId("urls-filter-note"));
    expect(note.top - pager.bottom).toBeGreaterThanOrEqual(16 - 1);
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await pageReady(page);
    await expectReadsAcross(page, 'main [data-testid="savings-module-table"]');
    await expectReadsAcross(page, 'main [data-testid="savings-optimizer-table"]');
    await expectReadsAcross(page, 'main [data-testid="savings-by-host"]');
  });
});

test.describe("a wrapped toolbar", () => {
  test("stays flush right under the Host selector", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await pageReady(page);
    const title = await box(page.locator("main .page-title"));
    const toolbar = await box(page.locator("main .page-toolbar"));
    expect(toolbar.top, "the Statistics toolbar wraps below the title at 1440").toBeGreaterThanOrEqual(title.bottom - 1);
    const lens = await box(page.getByTestId("host-lens"));
    const last = await box(page.locator("main .page-toolbar > :last-child"));
    expect(Math.abs(last.right - lens.right), `toolbar ends at ${last.right}, the Host selector at ${lens.right}`).toBeLessThanOrEqual(1);
  });
});

test.describe("small fixes", () => {
  test("the Host selector keeps one width on every page", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    const widths: number[] = [];
    for (const hash of ["#/about", "#/overview", "#/logs"]) {
      await openConsole(page, "pagespeed_global_admin", hash);
      await pageReady(page);
      await page.waitForTimeout(300);
      widths.push((await box(page.getByTestId("host-lens").locator("select"))).width);
    }
    expect(Math.max(...widths) - Math.min(...widths), `select widths ${widths.join(", ")}`).toBeLessThanOrEqual(1);
  });

  test("the hit rate's qualifier is a separate word, and the previews heading has room above it", async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await pageReady(page);
    await expect(page.getByText(/Optimized-copy hit rate \(of all in-place requests\)/)).toBeVisible();
    await openConsole(page, "pagespeed_global_admin", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https");
    await pageReady(page);
    const above = await page.locator("#previews-heading").evaluate((h) => {
      const section = h.closest("section")!;
      const prev = section.previousElementSibling!;
      return h.getBoundingClientRect().top - prev.getBoundingClientRect().bottom;
    });
    expect(above).toBeGreaterThanOrEqual(24 - 1);
  });
});

/** The table's frame spans its container: its right edge is the container's inner right edge. */
async function expectFullWidthTable(page: Page, table: string, container: string): Promise<void> {
  const [tableRight, inner] = await page.locator(table).first().evaluate((t, sel) => {
    const c = t.closest(sel)!;
    const cs = getComputedStyle(c);
    return [
      t.getBoundingClientRect().right,
      c.getBoundingClientRect().right - parseFloat(cs.paddingRight) - parseFloat(cs.borderRightWidth),
    ];
  }, container);
  expect(Math.abs(tableRight - inner), `${table} ends at ${tableRight}, its container at ${inner}`).toBeLessThanOrEqual(1);
}

/** A URL of 120 characters, for the long-URL checks. */
const LONG_URL = `/assets/${"very-long-path-segment-".repeat(4)}${"x".repeat(120 - 8 - 23 * 4 - 4)}.css`;

test.describe("full-width tables whose columns stay together", () => {
  test("at 2560 the frames span their container, the columns read across", async ({ page }) => {
    await page.setViewportSize({ width: 2560, height: 1440 });
    await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/urls");
    await pageReady(page);
    await expectFullWidthTable(page, 'main [data-testid="urls-table"]', ".table-wrapper");
    const wrapper = await box(page.locator("main .table-wrapper").first());
    const lens = await box(page.getByTestId("host-lens"));
    expect(Math.abs(wrapper.right - lens.right), "the URL table's frame ends under the Host selector").toBeLessThanOrEqual(1);
    await expectReadsAcross(page, 'main [data-testid="urls-table"]');
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await pageReady(page);
    for (const t of ["savings-module-table", "savings-optimizer-table", "savings-by-host"]) {
      await expectFullWidthTable(page, `main [data-testid="${t}"]`, ".card");
      await expectReadsAcross(page, `main [data-testid="${t}"]`);
      // With room to spare, a header stays on one line.
      const lines = await page.locator(`main [data-testid="${t}"] thead th`).evaluateAll((ths) =>
        ths.map((th) => {
          const range = document.createRange();
          range.selectNodeContents(th);
          return new Set([...range.getClientRects()].map((r) => Math.round(r.top))).size;
        }),
      );
      expect(lines.every((n) => n === 1), `${t} header lines: ${lines.join(", ")}`).toBe(true);
    }
  });

  for (const size of [
    { width: 1440, height: 900 },
    { width: 390, height: 844 },
  ]) {
    test(`a long URL neither overflows nor collapses at ${size.width}`, async ({ page }) => {
      await page.setViewportSize(size);
      expect(LONG_URL.length).toBe(120);
      await mockAdmin(page, "pagespeed_global_admin", {
        ...wholeServerReplies(),
        "v1/daemon/cache/urls": F.ok({
          urls: [
            { url: LONG_URL, hostname: "www.example.test", scheme: "https", alternate_count: 2 },
            { url: "/", hostname: "www.example.test", scheme: "https", alternate_count: 3 },
          ],
          offset: 0,
          limit: 50,
          next_offset: 2,
          has_more: false,
          total: 2,
        }),
      });
      await openConsole(page, "pagespeed_global_admin", "#/urls");
      await pageReady(page);
      expect(await noHorizontalScroll(page)).toBe(true);
      const link = page.locator('main [data-testid="urls-table"] tbody .url-cell a', { hasText: "very-long" });
      const b = await box(link);
      expect(b.width, "the URL keeps a readable width").toBeGreaterThanOrEqual(200);
      expect(b.right).toBeLessThanOrEqual(size.width);
    });
  }
});

test.describe("card heights", () => {
  for (const width of [1440, 2560]) {
    test(`Overview's two cards share one height at ${width}`, async ({ page }) => {
      await page.setViewportSize({ width, height: 1000 });
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      await openConsole(page, "pagespeed_global_admin", "#/overview");
      await pageReady(page);
      const bottoms = await page.locator("main .cards > .card").evaluateAll((els) => els.map((el) => el.getBoundingClientRect().bottom));
      expect(bottoms).toHaveLength(2);
      expect(Math.abs(bottoms[0] - bottoms[1]), `card bottoms ${bottoms.join(", ")}`).toBeLessThanOrEqual(1);
    });
  }
});

test.describe("tables fit their frame on a desktop", () => {
  for (const size of [
    { width: 1280, height: 720 },
    { width: 1440, height: 900 },
  ]) {
    test(`no table scrolls sideways at ${size.width}, even with wider glyphs`, async ({ page }) => {
      await page.setViewportSize(size);
      await mockAdmin(page, "pagespeed_global_admin", wholeServerReplies());
      for (const hash of ["#/savings", "#/overview", "#/urls", "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https"]) {
        await openConsole(page, "pagespeed_global_admin", hash);
        await pageReady(page);
        await widenText(page);
        expect(await sidewaysScrollers(page), `${hash}: wrappers that scroll sideways`).toEqual([]);
      }
    });
  }
});
