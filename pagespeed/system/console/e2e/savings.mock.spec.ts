// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, test } from "@playwright/test";
import { LABEL_INSET, valueAxisFit } from "./support/chart-axis";
import { mockAdmin, openConsole } from "./support/mock-admin";
import * as F from "./support/fixtures";
import {
  DAEMON_STATS,
  DAEMON_STATS_EMPTY,
  DAEMON_STATS_MALFORMED,
  DAEMON_STATS_REGRESSION,
  HEALTH_BELOW_FLOOR,
  HEALTH_OK,
  NOT_CONFIGURED,
  STATS_EMPTY,
  STATS_GLOBAL,
  STATS_VHOST,
  statsWithSavings,
  UNREACHABLE,
} from "./support/fixtures";

test.use({ locale: "en-US", timezoneId: "UTC" });

const HEALTHY = {
  stats_json: STATS_GLOBAL,
  "v1/daemon/health": HEALTH_OK,
  "v1/daemon/stats": DAEMON_STATS,
} as const;

test("shows module and optimizer savings on the global console", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", HEALTHY);
  await openConsole(page, "pagespeed_global_admin", "#/savings");

  await expect(page.getByTestId("savings-module-total")).toContainText("12.3 MB");
  await expect(page.getByTestId("savings-module-total")).toContainText("47% of 26.4 MB");

  const table = page.getByTestId("savings-module-table");
  await expect(table.getByRole("row", { name: /CSS/ })).toContainText("488 KB");
  await expect(table.getByRole("row", { name: /JavaScript/ })).toContainText("11.5 MB");
  await expect(table.getByRole("row", { name: /Images/ })).toContainText("296 KB");
  await expect(table.getByRole("row", { name: /Images/ })).toContainText("27%");

  // The hit rate lives on the optimizer card; this module predates the
  // per-class counters, so it is the all-requests rate, labelled as such.
  const optimizerCard = page.getByTestId("savings-optimizer-card");
  await expect(optimizerCard.getByTestId("savings-hit-rate")).toContainText("26%");
  await expect(optimizerCard.getByTestId("savings-hit-rate")).toContainText("3,122 of 12,006");
  await expect(optimizerCard).toContainText("of all in-place requests");

  await expect(page.getByTestId("savings-optimizer-total")).toContainText("673 KB");
  await expect(page.getByTestId("savings-optimizer-total")).toContainText("3% of 21.4 MB");
  await expect(page.getByTestId("savings-optimizer-not-served")).toContainText("HTML, JavaScript");
  // The two sources are labelled for what they are, never summed.
  await expect(page.getByTestId("savings-module-card")).toContainText("Module rewrites");
  await expect(page.getByTestId("savings-optimizer-card")).toContainText("Optimizer cache serves");
});

test("shows the vhost's own figures on a per-vhost console", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    stats_json: STATS_VHOST,
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS,
  });
  await openConsole(page, "pagespeed_admin", "#/savings");
  await expect(page.getByTestId("savings-scope")).toContainText("This virtual host");
  await expect(page.getByTestId("savings-module-total")).toContainText("281 KB");
  await expect(page.getByTestId("savings-module-total")).toContainText("19% of 1.46 MB");
  // 7 of 1651 in-place requests reads as "<1%", never a bare "0%".
  await expect(page.getByTestId("savings-hit-rate")).toContainText("<1%");
});

test("keeps the module figures when the optimizer is unreachable", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_GLOBAL,
    "v1/daemon/health": UNREACHABLE,
    "v1/daemon/stats": UNREACHABLE,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-module-total")).toContainText("12.3 MB");
  await expect(page.getByTestId("savings-optimizer-unavailable")).toContainText("Unreachable");
});

test("says so when no optimizer is configured", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_GLOBAL,
    "v1/daemon/health": NOT_CONFIGURED,
    "v1/daemon/stats": NOT_CONFIGURED,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-unavailable")).toContainText("Not configured");
  await expect(page.getByTestId("savings-module-total")).toContainText("12.3 MB");
});

test("flags an optimizer below the version floor and still shows its numbers", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_GLOBAL,
    "v1/daemon/health": HEALTH_BELOW_FLOOR,
    "v1/daemon/stats": DAEMON_STATS,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-below-floor")).toBeVisible();
  await expect(page.getByTestId("savings-optimizer-total")).toContainText("673 KB");
});

test("empty statistics read as 'no traffic yet', not zeros", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_EMPTY,
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS_EMPTY,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-empty")).toBeVisible();
  await expect(page.getByTestId("savings-optimizer-none")).toBeVisible();
  await expect(page.getByTestId("savings-module-table")).toBeHidden();
});

test("a net regression shows as no net savings, never a negative byte count", async ({ page }) => {
  const errors: Error[] = [];
  page.on("pageerror", (e) => errors.push(e));
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_GLOBAL,
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS_REGRESSION,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-regression")).toBeVisible();
  await expect(page.getByTestId("savings-optimizer-total")).toContainText("0 B");
  await expect(page.getByTestId("savings-optimizer-total")).not.toContainText("-");
  expect(errors).toEqual([]);
});

test("malformed serve_savings never reaches the UI as NaN", async ({ page }) => {
  const errors: Error[] = [];
  page.on("pageerror", (e) => errors.push(e));
  await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: STATS_GLOBAL,
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS_MALFORMED,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-none")).toBeVisible();
  await expect(page.getByTestId("savings-module-card")).not.toContainText("NaN");
  await expect(page.getByTestId("savings-optimizer-card")).not.toContainText("NaN");
  expect(errors).toEqual([]);
});

test("the chart fills in as samples arrive and announces its series", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_global_admin", {
    stats_json: (n: number) => statsWithSavings(1000 + n * 100),
    "v1/daemon/health": HEALTH_OK,
    "v1/daemon/stats": DAEMON_STATS,
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-chart-warming")).toBeVisible();
  // One sample per refresh: wait for the second before a line exists.
  await expect
    .poll(() => mock.calls("stats_json"), { timeout: 15000 })
    .toBeGreaterThanOrEqual(2);
  const chart = page.getByRole("img", { name: /Savings rate \(bytes per second\)/ });
  await expect(chart).toBeVisible();
  // The latest value is a byte rate: a finite number with its byte unit — the
  // exact figure depends on poll spacing, so the shape is asserted, not the value.
  await expect(chart).toHaveAccessibleName(/Module rewrites latest [0-9.]+ (B|KB|MB|GB|TB)\/s/);
});

for (const [name, viewport] of [
  ["desktop", { width: 1280, height: 900 }],
  ["phone", { width: 390, height: 844 }],
] as const) {
  test(`${name}: the chart's value axis shows its byte-rate numbers in full`, async ({ page }) => {
    await page.setViewportSize(viewport);
    // About 10 KB saved per second: labels like "9.77 KB/s", wider than the
    // chart library's default axis width.
    const mock = await mockAdmin(page, "pagespeed_global_admin", {
      stats_json: (n: number) => statsWithSavings(1000 + n * 50_000),
      "v1/daemon/health": HEALTH_OK,
      "v1/daemon/stats": DAEMON_STATS,
    });
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
    const fit = await valueAxisFit(page.getByTestId("savings-chart-card"));
    expect(fit.labels.some((s) => /^\d+(\.\d+)? KB\/s$/.test(s))).toBe(true);
    expect(fit.plotLeft).toBeGreaterThanOrEqual(Math.ceil(fit.widest));
    expect(fit.plotLeft).toBeGreaterThanOrEqual(fit.widest + LABEL_INSET);
  });
}

test("navigating away and back keeps one clean live series", async ({ page }) => {
  const errors: Error[] = [];
  page.on("pageerror", (e) => errors.push(e));
  const mock = await mockAdmin(page, "pagespeed_global_admin", HEALTHY);
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect.poll(() => mock.calls("stats_json")).toBeGreaterThanOrEqual(1);
  await page.goto("http://console.test/pagespeed_global_admin/#/statistics");
  await page.goto("http://console.test/pagespeed_global_admin/#/savings");
  await expect(page.getByTestId("savings-module-total")).toContainText("12.3 MB");
  expect(errors).toEqual([]);
});

test("g then v opens the savings page", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", HEALTHY);
  await openConsole(page, "pagespeed_global_admin", "#/statistics");
  await page.keyboard.press("g");
  await page.keyboard.press("v");
  await expect(page).toHaveURL(/#\/savings$/);
  await expect(page.getByRole("heading", { level: 1, name: "Savings" })).toBeVisible();
});

// The whole-server console with the two golden captures wired in: the
// optimizer's real stats page and the module's real statistics JSON.
function goldenReplies() {
  return {
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    stats_json: F.STATS_JSON_GLOBAL,
    "v1/daemon/stats": F.OPT_STATS_PAGE,
  };
}

test("explains an already-optimal type with the split and the verdict", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const css = page.getByTestId("savings-split-css");
  await expect(css).toContainText("Already optimal");
  await expect(css).toContainText("1 resource"); // the golden's one judged CSS resource
  await expect(css).toContainText("108 KB"); // its original size: 110554 bytes, three significant digits
  await expect(css.locator(".split-bar")).toHaveCount(1);
  await expect(page.getByTestId("savings-encodings-note")).toContainText("not served by this server");
  // No compressed copy was served, so the bytes are the optimized copies' own.
  await expect(page.getByTestId("savings-split-image")).toContainText("optimized bytes");
});

test("keys every split figure to its bar colour with a swatch", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const split = page.getByTestId("savings-split-css");
  const keys = ["already-optimal", "optimized-served", "served-encoded"];
  const labels = ["Already optimal", "Optimized and served", "Served compressed"];
  const terms = split.locator(".split-figures dt");
  await expect(terms).toHaveCount(3);
  for (const [i, key] of keys.entries()) {
    await expect(terms.nth(i)).toHaveText(labels[i]);
    const swatch = terms.nth(i).locator(`.split-swatch.split-${key}`);
    await expect(swatch).toHaveCount(1);
    await expect(swatch).toHaveAttribute("aria-hidden", "true");
    // The swatch paints the same fill as its bar segment.
    const fill = await swatch.evaluate((el) => getComputedStyle(el).backgroundColor);
    const seg = split.locator(`.split-bar .split-${key}`);
    if ((await seg.count()) > 0) {
      expect(await seg.evaluate((el) => getComputedStyle(el).backgroundColor)).toBe(fill);
    }
  }
});

test("shows the optimized-copy hit rate on the optimizer card with the excluded rest", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const card = page.getByTestId("savings-optimizer-card");
  await expect(card.getByTestId("savings-hit-rate")).toContainText("%");
  await expect(card).toContainText("Optimized-copy hit rate");
  await expect(card).toContainText("conditional and HEAD requests count as served");
  await expect(page.getByTestId("savings-not-optimizable")).toContainText(
    "requests not optimizable or not recorded",
  );
  // The module card no longer owns the rate.
  await expect(page.getByTestId("savings-module-card")).not.toContainText("Optimizer cache hit rate");
  await expect(page.getByTestId("savings-module-card").getByTestId("savings-hit-rate")).toHaveCount(0);
});

test("an older module keeps the old rate, labelled for what it divides by", async ({ page }) => {
  const oldModuleStats = JSON.parse(F.STATS_JSON_GLOBAL_BYTES);
  delete oldModuleStats.variables.ipro_daemon_fallthrough_css;
  delete oldModuleStats.variables.ipro_daemon_fallthrough_js;
  delete oldModuleStats.variables.ipro_daemon_fallthrough_image;
  delete oldModuleStats.variables.process_start_ms;
  await mockAdmin(page, "pagespeed_global_admin", {
    ...goldenReplies(),
    stats_json: F.ok(oldModuleStats),
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-card")).toContainText("of all in-place requests");
  await expect(page.getByTestId("savings-not-optimizable")).toHaveCount(0);
});

test("an older optimizer shows stored encodings as no data", async ({ page }) => {
  const oldOptimizerStats = {
    thread_pool: { size: 4 },
    serve_savings: { css: { original_bytes: 1000, optimized_bytes: 900, hits: 5 } },
  };
  await mockAdmin(page, "pagespeed_global_admin", {
    ...goldenReplies(),
    "v1/daemon/stats": F.ok(oldOptimizerStats),
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("savings-encodings-note")).toContainText("no data");
});

// The golden optimizer capture, with CSS served only as stored compressed
// copies: 4 requests, 64.2 KB sent of a 432 KB original.
function compressedCssReplies() {
  const golden = JSON.parse(F.OPT_STATS_PAGE_BYTES) as Record<string, Record<string, unknown>>;
  return {
    ...goldenReplies(),
    "v1/daemon/stats": F.ok({
      ...golden,
      serve_savings: {
        ...golden.serve_savings,
        css: {
          by_encoding: { br: { bytes: 50_741, hits: 3 }, gzip: { bytes: 15_000, hits: 1 }, identity: { bytes: 0, hits: 0 } },
          hits: 4,
          optimized_bytes: 65_741,
          original_bytes: 442_368,
        },
      },
    }),
  };
}

test("a type's saving is stated once, under its bar, and matches the table", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", compressedCssReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const css = page.getByTestId("savings-split-css");
  const summary = css.getByTestId("savings-split-summary");
  await expect(summary).toHaveCount(1);
  await expect(summary).toHaveText(
    "Saved 368 KB of 432 KB (85%), measured in transfer bytes, because compressed copies were served",
  );
  // Directly under the bar, above the rows.
  expect(
    await summary.evaluate(
      (el) => el.previousElementSibling?.classList.contains("split-bar") && el.nextElementSibling?.tagName === "DL",
    ),
  ).toBe(true);
  // The same figures as the per-type table's CSS row.
  const cells = page.getByTestId("savings-optimizer-table").getByRole("row", { name: /^CSS/ }).getByRole("cell");
  const [saved, original, percent] = await Promise.all([1, 2, 3].map((i) => cells.nth(i).innerText()));
  await expect(summary).toContainText(`Saved ${saved} of ${original} (${percent})`);
  // The rows: requests only on "Optimized and served"; bytes sent on "Served compressed".
  const rows = css.locator(".split-figures dd");
  await expect(rows.nth(1)).toHaveText("0 requests");
  await expect(rows.nth(1)).not.toContainText("saved");
  await expect(rows.nth(2)).toHaveText("4 requests · 64.2 KB sent");
  // No nested parentheses anywhere in the block.
  expect(await css.innerText()).not.toMatch(/\([^)]*\(/);
});

test("the already-optimal row separates its figures like the other rows", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  // The golden capture: 3 CSS requests, one judged resource of 110554 bytes.
  await expect(page.getByTestId("savings-split-css").locator(".split-figures dd").nth(0)).toHaveText(
    "3 requests · 1 resource, 108 KB original",
  );
});

test("a type served uncompressed states its saving in optimized bytes", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const image = page.getByTestId("savings-split-image");
  await expect(image.getByTestId("savings-split-summary")).toHaveText(
    "Saved 391 KB of 488 KB (80%), measured in optimized bytes",
  );
  await expect(image.locator(".split-figures dd").nth(1)).toHaveText("2 requests");
});

test("a type with no traffic says so instead of showing zeroes", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const js = page.getByTestId("savings-split-js");
  await expect(js).toContainText("No JavaScript seen yet.");
});

test("the module table heads its percentage column", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", HEALTHY);
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(
    page.getByTestId("savings-module-table").getByRole("columnheader", { name: "Saved %" }),
  ).toBeVisible();
});

test("stamps each card with its own time base", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("since-module")).toContainText("(web server restart)");
  await expect(page.getByTestId("since-optimizer")).toContainText("(optimizer restart)");
  // Each stamp carries its own base: the module's and the optimizer's
  // start times differ in the goldens, and so do their tooltips.
  const moduleIso = await page.getByTestId("since-module").getAttribute("title");
  const optimizerIso = await page.getByTestId("since-optimizer").getAttribute("title");
  expect(moduleIso).toBeTruthy();
  expect(optimizerIso).toBeTruthy();
  expect(moduleIso).not.toEqual(optimizerIso);
});

test("the hit rate carries the module's time base, not the optimizer's", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const card = page.getByTestId("savings-optimizer-card");
  await expect(card.getByTestId("savings-hit-rate")).toContainText("%");
  // The rate is computed from module counters, so its stamp is the module's.
  const stamp = card.getByTestId("since-hit-rate");
  await expect(stamp).toContainText("(web server restart)");
  await expect(stamp).toHaveText(await page.getByTestId("since-module").innerText());
  expect(await stamp.getAttribute("title")).toEqual(
    await page.getByTestId("since-module").getAttribute("title"),
  );
  // The stamp sits directly beneath the hit-rate figure.
  const follows = await stamp.evaluate((el) =>
    Boolean(el.previousElementSibling?.querySelector("[data-testid='savings-hit-rate']")),
  );
  expect(follows).toBe(true);
});

test("an older module and optimizer read 'since restart'", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", HEALTHY);
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("since-module")).toHaveText(/^\s*since restart \(web server restart\)\s*$/);
  await expect(page.getByTestId("since-optimizer")).toHaveText(/^\s*since restart \(optimizer restart\)\s*$/);
  await expect(page.getByTestId("warming-pill")).toHaveCount(0);
});

test("a two-minute-old optimizer shows warming up, not a percentage", async ({ page }) => {
  // The module golden carries no rewrite-savings counters, so the module
  // total gets the realistic rewrite counters of the existing whole-server
  // fixture next to the golden's own (unchanged) values.
  const golden = JSON.parse(F.STATS_JSON_GLOBAL_BYTES);
  const withRewrites = {
    ...golden,
    variables: { ...(STATS_GLOBAL.body as { variables: Record<string, number> }).variables, ...golden.variables },
  };
  await mockAdmin(page, "pagespeed_global_admin", {
    ...goldenReplies(),
    stats_json: F.ok(withRewrites),
    "v1/daemon/stats": F.optStatsStarted(2 * 60_000),
  });
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByTestId("warming-pill").first()).toContainText("warming up");
  await expect(page.getByTestId("savings-optimizer-total")).toContainText("—");
  await expect(page.getByTestId("savings-optimizer-total")).not.toContainText("%");
  // The module base is days old in the golden; its percentage stays.
  await expect(page.getByTestId("savings-module-total")).toContainText("%");
});

test("the per-host console mutes the server-wide optimizer card and links on", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    ...goldenReplies(),
    config: F.CONFIG_VHOST,
  });
  await openConsole(page, "pagespeed_admin", "#/savings");
  const note = page.getByTestId("optimizer-scope-note");
  await expect(note).toContainText("Server-wide numbers");
  await expect(note.locator("a")).toHaveAttribute("href", /pagespeed_global_admin\/#\/savings$/);
  await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
  await expect(page.getByTestId("savings-module-card").locator(".scope-chip")).toHaveCount(0);
  await expect(page.getByTestId("savings-optimizer-card").locator(".scope-chip")).toHaveText("whole server");
});

// The muting must show in both schemes: in dark, the card's background
// token equals the surface, so a background change alone is invisible.
for (const scheme of ["light", "dark"] as const) {
  test.describe(`the muted per-host optimizer card (${scheme})`, () => {
    test.use({ colorScheme: scheme });
    test("is visibly set apart from the host's own card", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", { ...goldenReplies(), config: F.CONFIG_VHOST });
      await openConsole(page, "pagespeed_admin", "#/savings");
      const muted = page.getByTestId("savings-optimizer-card");
      const own = page.getByTestId("savings-module-card");
      await expect(muted).toHaveClass(/card--muted/);
      await expect(muted).toHaveCSS("border-top-style", "dashed");
      await expect(own).toHaveCSS("border-top-style", "solid");
      if (scheme === "dark") {
        const opacity = Number(await muted.evaluate((el) => getComputedStyle(el).opacity));
        expect(opacity).toBeLessThan(1);
      }
    });
  });
}

test("the chart names its real window and shows latest values in the card", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const heading = page.getByTestId("savings-chart-window");
  // The chart first renders from the SECOND sample (chartView stays null
  // under two) and the poll interval is 5 s, while Playwright's default
  // expect timeout is also 5 s -- so these assertions get 15 s, keeping
  // the second poll inside the window instead of racing it.
  await expect(heading).toContainText(/Last \d+ (s|min)/, { timeout: 15_000 });
  await expect(page.getByTestId("savings-chart-legend")).toContainText("Module rewrites", { timeout: 15_000 });
  await expect(page.getByTestId("savings-chart-legend")).not.toContainText("--", { timeout: 15_000 });
  // The latest rate reads as bytes per second through the shared byte formatter, never a bare "N/s".
  await expect(page.getByTestId("savings-chart-legend").locator(".legend-value").first()).toHaveText(
    /^\d+(\.\d+)? (B|KB|MB|GB|TB)\/s$/,
    { timeout: 15_000 },
  );
});

test("the chart carries a text alternative and keeps its legend inside the card", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", goldenReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  await expect(page.getByRole("heading", { level: 1, name: "Savings" })).toBeVisible();
  const card = page.getByTestId("savings-chart-card");
  const summary = card.locator(".chart-summary-text");
  await expect(summary).toHaveCount(1, { timeout: 15_000 });
  await expect(summary).toContainText(/latest/);
  await expect(card).toHaveCSS("overflow", "hidden");
});

test.describe("serve savings per host", () => {
  test("the golden capture survives the mock unchanged", () => {
    expect(JSON.stringify(JSON.parse(F.OPT_STATS_PAGE_BYTES))).toBe(F.OPT_STATS_PAGE_BYTES);
  });

  test("the whole-server console lists the optimizer's savings per host, most served first", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    const table = page.getByTestId("savings-by-host");
    await expect(table.locator("tbody tr")).toHaveCount(2);
    await expect(table.locator("tbody tr").first()).toContainText("www.example.com");
    await expect(table.locator("tbody tr").nth(1)).toContainText("static.example.com");
    await expect(table.locator("tbody tr").nth(1)).toContainText("80%");
    await expect(page.getByTestId("savings-by-host-other")).toHaveCount(0);
  });

  test("with a lens, the optimizer card leads with that host's figures", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_global_admin", "#/savings?lens=static.example.com");
    const figures = page.getByTestId("savings-lens-host");
    await expect(figures).toContainText("static.example.com");
    await expect(figures).toContainText("80%");
    await expect(figures).toContainText("2 responses");
    await expect(page.getByTestId("savings-by-host")).toHaveCount(0);
    await expect(page.getByTestId("lens-note")).toHaveCount(0);
  });

  test("a lens host the optimizer has not served says so", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_global_admin", "#/savings?lens=nothing.example");
    await expect(page.getByTestId("savings-host-none")).toContainText("No serves recorded for nothing.example");
  });

  test("an optimizer that predates per-host savings: the lens says so, nothing breaks", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE_PRE_HOSTS });
    await openConsole(page, "pagespeed_global_admin", "#/savings?lens=www.example.com");
    await expect(page.getByTestId("savings-host-unsupported")).toContainText("does not report savings per host");
    await expect(page.getByTestId("savings-optimizer-total")).toBeVisible();
    await page.getByTestId("host-lens").getByRole("combobox", { name: "Host" }).selectOption("");
    await expect(page.getByTestId("savings-host-unsupported")).toHaveCount(0);
    await expect(page.getByTestId("savings-optimizer-total")).toBeVisible();
    await expect(page.getByTestId("savings-by-host")).toHaveCount(0);
    expect(errors).toEqual([]);
  });

  test("a per-vhost console shows its own host's optimizer savings", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST_STATIC, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_admin", "#/savings");
    const own = page.getByTestId("savings-this-host");
    await expect(own).toContainText("static.example.com");
    await expect(own).toContainText("80%");
    await expect(page.getByTestId("savings-optimizer-card")).not.toHaveClass(/card--muted/);
    await expect(page.getByTestId("host-lens")).toHaveCount(0);
  });

  test("a per-vhost console whose host the optimizer has no row for keeps the server-wide note", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), "v1/daemon/stats": F.OPT_STATS_PAGE_PRE_HOSTS });
    await openConsole(page, "pagespeed_admin", "#/savings");
    await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
    await expect(page.getByTestId("optimizer-scope-note")).toBeVisible();
    await expect(page.getByTestId("savings-this-host")).toHaveCount(0);
  });

  test("an optimizer host row with markup in its name is never shown", async ({ page }) => {
    const dialogs: string[] = [];
    page.on("dialog", (d) => {
      dialogs.push(d.message());
      void d.dismiss();
    });
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_HOSTS_HOSTILE });
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    const table = page.getByTestId("savings-by-host");
    await expect(table.locator("tbody tr")).toHaveCount(2);
    // The repeated host is one row with both rows' serves; the rows whose
    // names are not host names count under "Other hosts", so the table
    // still adds up.
    await expect(table.locator("tbody tr").first()).toContainText("www.example.com");
    await expect(table.locator("tbody tr").first().locator("td").nth(1)).toHaveText("7");
    await expect(page.getByTestId("savings-by-host-other")).toContainText("Other hosts");
    await expect(page.getByTestId("savings-by-host-other").locator("td").nth(1)).toHaveText("14");
    await expect(page.locator("main img")).toHaveCount(0);
    await expect(table).not.toContainText("NaN");
    await expect(table).not.toContainText("img");
    expect(dialogs).toEqual([]);
  });

  test("the hosts the optimizer served feed the lens", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await expect(page.getByTestId("savings-by-host")).toBeVisible();
    await expect(page.getByTestId("host-lens").locator("option")).toHaveText(["All hosts", "static.example.com", "www.example.com"]);
  });

  test("phone: the per-host table does not scroll the page sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await expect(page.getByTestId("savings-by-host")).toBeVisible();
    expect(
      await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
    ).toBe(true);
  });
});

// ── A per-host console and the module's site marker ─────────────────
// The console is opened as www.example.test (F.CONFIG_VHOST); the site's
// primary name, which the module marks, is example.test.

test("a per-vhost console shows the row the module marks, under that row's own name", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST, "v1/daemon/stats": F.OPT_STATS_SITE_ROW });
  await openConsole(page, "pagespeed_admin", "#/savings");
  const own = page.getByTestId("savings-this-host");
  await expect(own).toContainText("Saved on responses served for example.test");
  await expect(own).toContainText("75%");
  await expect(own).toContainText("4 responses");
  await expect(page.getByTestId("savings-optimizer-card")).not.toHaveClass(/card--muted/);
  await expect(page.getByTestId("savings-host-none")).toHaveCount(0);
});

test("a marked block without that row stays muted and names the site", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST, "v1/daemon/stats": F.OPT_STATS_SITE_NO_ROW });
  await openConsole(page, "pagespeed_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
  await expect(page.getByTestId("savings-host-none")).toContainText("No serves recorded for example.test");
});

test("a block marked with no site stays muted and names none", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST, "v1/daemon/stats": F.OPT_STATS_SITE_NONE });
  await openConsole(page, "pagespeed_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
  await expect(page.getByTestId("optimizer-scope-note")).toBeVisible();
  await expect(page.getByTestId("savings-this-host")).toHaveCount(0);
  await expect(page.getByTestId("savings-optimizer-card")).not.toContainText("www.example.test");
});

test("an unusable marker never falls back to matching the console's name", async ({ page }) => {
  const dialogs: string[] = [];
  page.on("dialog", (d) => {
    dialogs.push(d.message());
    void d.dismiss();
  });
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST, "v1/daemon/stats": F.OPT_STATS_SITE_UNUSABLE });
  await openConsole(page, "pagespeed_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
  await expect(page.getByTestId("savings-this-host")).toHaveCount(0);
  await expect(page.locator("main img")).toHaveCount(0);
  expect(dialogs).toEqual([]);
});

test("against a module that sends every site's rows, a per-vhost console matches by name only", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST, "v1/daemon/stats": F.OPT_STATS_PAGE });
  await openConsole(page, "pagespeed_admin", "#/savings");
  await expect(page.getByTestId("savings-optimizer-card")).toHaveClass(/card--muted/);
  await expect(page.getByTestId("savings-host-none")).toContainText("No serves recorded for www.example.test");
  await expect(page.getByTestId("savings-optimizer-card")).not.toContainText("static.example.com");
});
