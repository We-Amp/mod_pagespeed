// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Self-contained: serves the committed bundle and answers every admin
// request from fixtures (see support/mock-admin.ts). Run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Admin, type Leaf, type Responder } from "./support/mock-admin";

// Grouped figures ("3,122") and timestamps must not depend on the machine's locale.
test.use({ locale: "en-US", timezoneId: "UTC" });

function healthy(extra: Partial<Record<Leaf, Responder>> = {}): Partial<Record<Leaf, Responder>> {
  return {
    stats_json: F.STATS_GLOBAL,
    config: F.CONFIG_GLOBAL,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": F.DAEMON_STATS,
    message_history: F.messageGroups([]),
    ...extra,
  };
}

async function open(page: Page, admin: Admin, replies: Partial<Record<Leaf, Responder>>) {
  const mock = await mockAdmin(page, admin, replies);
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await openConsole(page, admin);
  await expect(page.getByTestId("module-card")).toBeVisible();
  return { mock, errors };
}

async function expectNoJunk(page: Page) {
  const text = await page.locator("main").innerText();
  expect(text).not.toMatch(/NaN|undefined|null|\[object Object\]/);
}

test.describe("Overview", () => {
  test("is the landing page and the first navigation item", async ({ page }) => {
    const { errors } = await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST }));
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await expect(page.locator(".nav-item").first()).toHaveText("Overview");
    await expect(page.locator(".nav-item").nth(1)).toHaveText("Savings");
    await expect(page.locator(".nav-item").nth(2)).toHaveText("Statistics");
    expect(errors).toEqual([]);
  });

  test("global admin: whole-server scope, module and optimizer figures", async ({ page }) => {
    await open(page, "pagespeed_global_admin", healthy());
    await expect(page.getByTestId("overview-scope")).toHaveText("All virtual hosts (the whole server)");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("47% of 26.4 MB");
    await expect(page.getByTestId("module-served")).toContainText("3,122");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-card")).toContainText("2.0.41");
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    await expectNoJunk(page);
  });

  test("per-vhost admin: names the host and shows its own figures", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST }));
    await expect(page.getByTestId("overview-scope")).toContainText("This virtual host (www.example.test:80)");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("281 KB");
    await expect(page.getByTestId("module-served")).toHaveText(/^7/);
    await expect(page.getByTestId("module-served")).toContainText("of 1,651 in-place requests");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expectNoJunk(page);
  });

  test("an older module build: unreported counters read as a dash", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_OLD_BUILD }));
    await expect(page.getByTestId("module-served")).toHaveText("—");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("281 KB");
    await expectNoJunk(page);
  });

  test("scope falls back to the admin path when the configuration cannot be read", async ({ page }) => {
    await open(page, "pagespeed_global_admin", healthy({ config: { status: 500, body: { error: "x" } } }));
    await expect(page.getByTestId("overview-scope")).toHaveText("All virtual hosts (the whole server)");
  });

  test("daemon absent (unreachable): the module card keeps working", async ({ page }) => {
    const { errors } = await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": F.UNREACHABLE }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Unreachable");
    await expect(page.getByTestId("optimizer-card")).toContainText("configured but not answering");
    await expectNoJunk(page);
    expect(errors).toEqual([]);
  });

  test("daemon absent (not configured): an ordinary module-only server", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      stats_json: F.STATS_MODULE_ONLY,
      "v1/daemon/health": F.NOT_CONFIGURED,
      "v1/daemon/stats": F.NOT_CONFIGURED,
    }));
    await expect(page.getByTestId("optimizer-state")).toHaveText("Not configured");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("module-served")).toHaveCount(0);
  });

  test("a non-string health version does not render junk", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BAD_VERSION }));
    await expect(page.getByTestId("optimizer-card")).not.toContainText("[object Object]");
    await expectNoJunk(page);
  });

  test("empty statistics: onboarding card, no figures", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: F.STATS_EMPTY, "v1/daemon/stats": F.DAEMON_STATS_EMPTY }));
    await expect(page.getByTestId("overview-empty")).toContainText("The module is running and ready.");
    await expect(page.getByTestId("module-bytes-saved")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-savings")).toHaveCount(0);
    await expectNoJunk(page);
  });

  test("optimizer below the floor (endpoint not provided): module card intact", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.UNSUPPORTED }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-outdated")).toBeVisible();
    await expect(page.getByTestId("optimizer-below-floor")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-savings")).toHaveCount(0);
  });

  test("optimizer below the floor (version older than the minimum): module card intact", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BELOW_FLOOR }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-card")).toContainText("2.0.3");
    await expect(page.getByTestId("optimizer-below-floor")).toContainText("2.0.41");
    await expect(page.getByTestId("optimizer-outdated")).toHaveCount(0);
  });

  test("a busy statistics read keeps the optimizer figures", async ({ page }) => {
    // Deterministic by construction: the flag is armed right before the
    // action that triggers the next poll, and the mock answers 429 to the
    // next stats request while it is armed -- not tied to a call index,
    // which a slow run could shift past the click.
    let busy = false;
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/stats": () => {
        if (busy) {
          busy = false;
          return F.BUSY;
        }
        return F.DAEMON_STATS;
      },
    }));
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    busy = true;
    await Promise.all([
      page.waitForResponse((r) => r.url().endsWith("/v1/daemon/stats") && r.status() === 429),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
    await page.waitForTimeout(300);
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
  });

  test("all worker threads busy is a note, not a start-up state", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BUSY }));
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-busy")).toHaveText("All worker threads busy.");
  });

  test("module statistics unavailable: an error, and the optimizer card still renders", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", healthy({ stats_json: F.MODULE_FAILURE }));
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-error")).toContainText("Could not load the module statistics.");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
  });

  test("links lead to the detail pages", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy());
    // A figure is plain text; its drill-down is a link of its own in the tile.
    await expect(page.getByTestId("module-fetch-failures").getByRole("link")).toHaveCount(0);
    await expect(page.getByTestId("module-bytes-saved").getByRole("link")).toHaveCount(0);
    await page.getByTestId("module-fetch-failures-link").click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=warning$/);
  });

  test("one sample per refresh interval: no request storm", async ({ page }) => {
    const { mock } = await open(page, "pagespeed_admin", healthy());
    await page.waitForTimeout(1500);
    expect(mock.calls("stats_json")).toBeLessThanOrEqual(2);
    await page.waitForTimeout(11_000);
    const n = mock.calls("stats_json");
    expect(n).toBeGreaterThanOrEqual(2);
    expect(n).toBeLessThanOrEqual(5);
    // One sample = one read of each source (allow one sample in flight).
    expect(Math.abs(mock.calls("v1/daemon/health") - n)).toBeLessThanOrEqual(1);
    expect(Math.abs(mock.calls("v1/daemon/stats") - n)).toBeLessThanOrEqual(1);
    expect(mock.calls("config")).toBe(1);
  });
});

// The page answers first: one sentence with the state and the figures behind
// it, then the tiles. Each number carries the scope and the time base of its
// own source; the two savings figures are never added up.
test.describe("Overview verdict and tiles", () => {
  const HOUR = 3_600_000;
  const vars = (reply: typeof F.STATS_GLOBAL) => (reply.body as { variables: Record<string, number> }).variables;
  const withBody = (reply: typeof F.STATS_GLOBAL, patch: Record<string, unknown>) => ({
    ...reply,
    body: { ...(reply.body as Record<string, unknown>), ...patch },
  });
  /** Statistics whose web server started `ageMs` ago. */
  const statsStarted = (reply: typeof F.STATS_GLOBAL, ageMs: number): Responder => () =>
    withBody(reply, { variables: { ...vars(reply), process_start_ms: Date.now() - ageMs } });
  /** Optimizer statistics whose optimizer started `ageMs` ago. */
  const optimizerStarted = (ageMs: number): Responder => () => withBody(F.DAEMON_STATS, { started_at_ms: Date.now() - ageMs });
  const steady = (extra: Partial<Record<Leaf, Responder>> = {}) =>
    healthy({ stats_json: statsStarted(F.STATS_GLOBAL, 26 * HOUR), "v1/daemon/stats": optimizerStarted(3 * HOUR), ...extra });

  /** The element starts inside the first viewport: nothing to scroll for. */
  async function inFirstViewport(page: Page, testId: string): Promise<boolean> {
    const box = await page.getByTestId(testId).first().boundingBox();
    const viewport = page.viewportSize();
    return box !== null && viewport !== null && box.y >= 0 && box.y + Math.min(box.height, 40) <= viewport.height;
  }

  test("whole server: the optimizer's serves and savings since it started, and that figure leads", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady());
    await expect(page.getByTestId("health-summary")).toHaveText(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago. No findings.",
    );
    await expect(page.getByTestId("health-strip")).toHaveClass(/health-strip-ok/);
    await expect(page.getByTestId("health-dot")).toHaveClass(/status-dot-ok/);
    // One leading figure: the optimizer's transfer savings, in the second tile.
    const lead = page.getByTestId("overview-lead-figure");
    await expect(lead).toHaveCount(1);
    await expect(lead).toHaveText("673 KB");
    await expect(page.getByTestId("tile-cache").getByTestId("overview-lead-figure")).toHaveCount(1);
    await expect(page.getByTestId("tile-cache").getByTestId("optimizer-savings")).toContainText("3% of 21.4 MB");
    // One line of context in the tile; how the figure is measured sits with the by-type table.
    await expect(page.getByTestId("tile-cache").getByTestId("optimizer-savings").locator("p")).toHaveCount(2);
    await expect(page.getByTestId("tile-cache").getByTestId("savings-qualifier")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-card").getByTestId("savings-qualifier")).toHaveText(
      "Measured in transfer bytes, includes compression.",
    );
    // The two figures are related in words, never added up.
    await expect(page.getByTestId("savings-relation")).toHaveText(
      "Separate from the rewriting figure: these are responses served from the optimizer's cache.",
    );
    await expect(page.getByTestId("tile-rewriting").getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    const main = await page.locator("main").innerText();
    expect(main).not.toContain("12.9 MB"); // 12.3 MB + 673 KB
    // Each tile names its own time base.
    await expect(page.getByTestId("tile-rewriting").getByTestId("since-module")).toHaveText("since the web server started 1 d ago");
    await expect(page.getByTestId("tile-cache").getByTestId("since-optimizer")).toHaveText("since the optimizer started 3 h ago");
    // Once per source: the other module tile does not repeat it.
    await expect(page.getByTestId("since-module")).toHaveCount(1);
    await expect(page.getByTestId("tile-fetch-failures").locator(".stat-tile-context")).toHaveCount(0);
    // Below the tiles: both sources by type.
    await expect(page.getByTestId("module-by-type").locator("tbody tr")).toHaveCount(3);
    await expect(page.getByTestId("optimizer-by-type").locator("tbody tr")).toHaveText([/CSS\s*197/, /Images\s*45/]);
    await expectNoJunk(page);
  });

  test("a figure is plain text in the text colour; a zero is not a link and not an alert", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady());
    const failures = page.getByTestId("module-fetch-failures");
    await expect(failures).toHaveText("0");
    const text = await page.locator("h1").evaluate((el) => getComputedStyle(el).color);
    for (const id of ["module-fetch-failures", "overview-lead-figure"]) {
      const figure = page.getByTestId(id);
      expect(await figure.evaluate((el) => getComputedStyle(el).color), id).toBe(text);
      expect(await figure.evaluate((el) => getComputedStyle(el).textDecorationLine), id).toBe("none");
      expect(await figure.evaluate((el) => el.closest("a") === null && el.querySelector("a") === null), id).toBe(true);
    }
    // The drill-down is its own link, at least 24 px tall.
    for (const link of await page.getByTestId("overview-tiles").locator("a.stat-tile-link").all()) {
      expect((await link.boundingBox())!.height).toBeGreaterThanOrEqual(24);
    }
    await expect(page.getByTestId("overview-tiles").locator("a.stat-tile-link")).toHaveCount(4);
  });

  test("per host: this site's rewriting leads; the optimizer is named by state, its figures stay whole-server", async ({ page }) => {
    await open(page, "pagespeed_admin", steady({ config: F.CONFIG_VHOST, stats_json: statsStarted(F.STATS_VHOST, 26 * HOUR) }));
    const verdict = page.getByTestId("health-summary");
    await expect(verdict).toHaveText(
      "Working. This site: 281 KB saved by rewriting since the web server started 1 d ago. Optimizer (whole server): running. No findings.",
    );
    // The optimizer's whole-server figures are not in the sentence and not in a tile.
    await expect(verdict).not.toContainText("673 KB");
    await expect(verdict).not.toContainText("242");
    await expect(page.getByTestId("tile-rewriting").getByTestId("overview-lead-figure")).toHaveText("281 KB");
    await expect(page.getByTestId("overview-tiles").getByTestId("optimizer-savings")).toHaveCount(0);
    // They stay in the muted whole-server card, labelled as such.
    const card = page.getByTestId("optimizer-card");
    await expect(card).toHaveClass(/card-muted/);
    await expect(card.getByTestId("optimizer-savings")).toContainText("673 KB");
    await expect(card.getByTestId("optimizer-scope-note")).toContainText("Server-wide numbers");
    // This site's own count of serves from the optimizer's cache is a tile, with how to read a low share.
    await expect(page.getByTestId("tile-cache").getByTestId("module-served")).toContainText("of 1,651 in-place requests");
    await expect(page.getByTestId("serve-share-judgement")).toHaveText(
      "Normal when most requests are pages: the optimizer's cache answers CSS, JavaScript and images.",
    );
    await expect(page.getByTestId("tile-optimizer").locator(".scope-chip")).toHaveText("whole server");
  });

  test("module only: the rewriting figure leads, there is no optimizer-cache tile, and the optimizer can be added", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      stats_json: statsStarted(F.STATS_MODULE_ONLY, 26 * HOUR),
      "v1/daemon/health": F.NOT_CONFIGURED,
      "v1/daemon/stats": F.NOT_CONFIGURED,
    }));
    await expect(page.getByTestId("health-summary")).toHaveText(
      "Working. 12.3 MB saved by rewriting since the web server started 1 d ago. No findings.",
    );
    await expect(page.getByTestId("overview-lead-figure")).toHaveText("12.3 MB");
    await expect(page.getByTestId("tile-cache")).toHaveCount(0);
    await expect(page.getByTestId("overview-tiles").locator(".stat-tile")).toHaveCount(3);
    await expect(page.getByTestId("tile-optimizer").getByTestId("optimizer-state")).toHaveText("Not configured");
    const add = page.getByTestId("add-optimizer");
    await expect(add).toHaveText("Add the optimizer");
    await expect(add).toHaveAttribute("href", "https://modpagespeed.com/1.1/docs/configuration/#area-optimizer-worker");
    await expect(add).toHaveAttribute("rel", "noopener noreferrer");
  });

  test("warm-up applies per source: a restarted optimizer does not hide the module's figure", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady({ "v1/daemon/stats": optimizerStarted(4 * 60_000) }));
    await expect(page.getByTestId("health-summary")).toContainText(
      "Working. 12.3 MB saved by rewriting since the web server started 1 d ago. Optimizer restarted 4 min ago; its figures are not meaningful yet.",
    );
    // No share is printed for the warming source, and no bare dash stands in for it.
    const savings = page.getByTestId("optimizer-savings");
    await expect(savings).toContainText("of 21.4 MB so far, since the optimizer started 4 min ago");
    await expect(savings).not.toContainText("%");
    await expect(savings).not.toContainText("—");
    await expect(page.getByTestId("tile-cache").getByTestId("warming-pill")).toHaveCount(1);
    await expect(page.getByTestId("overview-tiles").getByTestId("warming-pill")).toHaveCount(1);
    await expect(page.getByTestId("tile-rewriting").getByTestId("warming-pill")).toHaveCount(0);
    await expect(page.getByTestId("module-bytes-saved")).toContainText("47% of 26.4 MB");
  });

  test("every source warming: no figure in the sentence", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady({
      stats_json: statsStarted(F.STATS_GLOBAL, 9 * 60_000),
      "v1/daemon/stats": optimizerStarted(4 * 60_000),
    }));
    const verdict = page.getByTestId("health-summary");
    await expect(verdict).toContainText("Warming up. Counters restarted 4 min ago; figures are not meaningful yet.");
    await expect(verdict).not.toContainText(/KB|MB/);
    await expect(page.getByTestId("health-strip")).toHaveClass(/health-strip-info/);
  });

  test("first run: running with no traffic, and three next steps instead of empty figures", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      stats_json: statsStarted(F.STATS_EMPTY, 2 * HOUR),
      "v1/daemon/health": F.NOT_CONFIGURED,
      "v1/daemon/stats": F.NOT_CONFIGURED,
    }));
    await expect(page.getByTestId("health-summary")).toHaveText(
      "Running, no traffic seen yet since the web server started 2 h ago. No findings.",
    );
    await expect(page.getByTestId("health-strip")).toHaveClass(/health-strip-info/);
    await expect(page.getByTestId("overview-tiles")).toHaveCount(0);
    await expect(page.getByTestId("overview-lead-figure")).toHaveCount(0);
    const steps = page.getByTestId("overview-next-steps").locator("li");
    await expect(steps).toHaveCount(3);
    await expect(steps.nth(0)).toContainText("Load a page of this site");
    await expect(page.getByTestId("overview-header-check")).toHaveText(
      "curl -sI http://example.com/ | grep -i -E 'x-mod-pagespeed|x-page-speed'",
    );
    await expect(steps.nth(2)).toContainText("Come back here");
    await expect(page.getByTestId("optimizer-card").getByTestId("optimizer-state")).toHaveText("Not configured");
    await expect(page.getByTestId("add-optimizer")).toBeVisible();
    await expectNoJunk(page);
  });

  test("one signal: the dot and the rule follow the word; findings are marked in words where they are counted", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady({ "v1/daemon/health": F.HEALTH_CHECK_FAILING }));
    await expect(page.getByTestId("health-summary")).toContainText(/^Working\. .* Error 1 finding · 1 needs action\.$/);
    // "Working" keeps the working colour: the dot never says something the word does not.
    await expect(page.getByTestId("health-strip")).toHaveClass(/health-strip-ok/);
    await expect(page.getByTestId("health-dot")).toHaveClass(/status-dot-ok/);
    await expect(page.getByTestId("health-dot")).not.toHaveClass(/status-dot-(error|warning)/);
    // The findings' level is a word beside the count, not a colour alone.
    const mark = page.getByTestId("findings-mark");
    await expect(mark).toHaveText("Error");
    await expect(mark).toHaveClass(/level-badge-error/);
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 1 needs action");
    // The count is a link to the findings: it scrolls there and takes focus, and the page stays.
    await page.setViewportSize({ width: 390, height: 844 });
    const link = page.getByTestId("findings-link");
    await expect(link).toHaveText("1 finding · 1 needs action");
    await link.click();
    await expect(page.getByTestId("findings-anchor")).toBeFocused();
    await expect(page.getByTestId("alert-daemon-check-failed")).toBeInViewport();
    await expect(page).toHaveURL(/pagespeed_global_admin\/(#\/overview)?$/);
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.setViewportSize({ width: 1280, height: 720 });
    // The findings follow the tiles.
    const tiles = (await page.getByTestId("overview-tiles").boundingBox())!;
    const findings = (await page.getByTestId("alerts").boundingBox())!;
    expect(findings.y).toBeGreaterThanOrEqual(tiles.y + tiles.height);
  });

  test("right after a web server restart the optimizer's figures still lead: tiles, no first-run steps", async ({ page }) => {
    // The module's counters are zero again; the optimizer has been serving for hours.
    await open(page, "pagespeed_global_admin", steady({ stats_json: statsStarted(F.STATS_EMPTY, 20_000) }));
    await expect(page.getByTestId("health-summary")).toContainText(
      "Working. 242 optimized responses served from the optimizer's cache, 673 KB saved since the optimizer started 3 h ago. " +
        "Web server restarted less than a minute ago; the module's figures are not meaningful yet.",
    );
    // The page agrees with the sentence: the tiles and the leading figure are there.
    await expect(page.getByTestId("overview-tiles")).toBeVisible();
    await expect(page.getByTestId("overview-lead-figure")).toHaveText("673 KB");
    await expect(page.getByTestId("tile-rewriting").getByTestId("warming-pill")).toBeVisible();
    // Not a first run: no onboarding, a plain line instead of a table of zeros.
    await expect(page.getByTestId("overview-empty")).toHaveCount(0);
    await expect(page.getByTestId("overview-next-steps")).toHaveCount(0);
    await expect(page.getByTestId("module-nothing-yet")).toHaveText(
      "The module has rewritten nothing since the web server started less than a minute ago.",
    );
    await expectNoJunk(page);
  });

  test("a failed module read over good figures: status unknown with the reason, and the notice above the stale tiles", async ({ page }) => {
    let fail = false;
    const good = statsStarted(F.STATS_GLOBAL, 26 * HOUR);
    await open(page, "pagespeed_global_admin", steady({
      stats_json: (call, request) => (fail ? { status: 500, body: { error: "internal" } } : good(call, request)),
    }));
    await expect(page.getByTestId("health-summary")).toContainText(/^Working\. /);
    await expect(page.locator("main .refresh-notice")).toHaveCount(0);
    fail = true;
    await Promise.all([
      page.waitForResponse((r) => r.url().includes("stats_json") && r.status() === 500),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
    const verdict = page.getByTestId("health-summary");
    await expect(verdict).toHaveText(
      /^Status unknown\. The module statistics could not be read; the figures are from \d\d:\d\d:\d\d\. Module statistics unavailable\.$/,
    );
    // Never "No findings" over figures that were not read, no green, and no figure in the sentence.
    await expect(verdict).not.toContainText("No findings");
    await expect(verdict).not.toContainText(/KB|MB/);
    await expect(page.getByTestId("health-strip")).not.toHaveClass(/health-strip-(ok|info)/);
    // The last good figures stay, under a notice that says why they are stale.
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    const notice = page.locator("main .refresh-notice");
    await expect(notice).toHaveCount(1);
    await expect(notice).toContainText("Last refresh failed");
    await expect(notice).toContainText("Showing the most recent data");
    const noticeBox = (await notice.boundingBox())!;
    const tilesBox = (await page.getByTestId("overview-tiles").boundingBox())!;
    expect(noticeBox.y + noticeBox.height, "the notice is above the tiles").toBeLessThanOrEqual(tilesBox.y);
    // It answers again: the verdict and the figures are current, the notice is gone.
    fail = false;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(verdict).toContainText(/^Working\. /);
    await expect(notice).toHaveCount(0);
  });

  test("the sentence's figures are not announced on every refresh: only the findings count is a status", async ({ page }) => {
    await open(page, "pagespeed_global_admin", steady());
    const verdict = page.getByTestId("health-summary");
    expect(await verdict.evaluate((el) => el.closest("[aria-live], [role=status], [role=alert], [role=log]") === null)).toBe(true);
    await expect(page.getByTestId("findings-summary")).toHaveAttribute("role", "status");
  });

  for (const [name, size] of [["desk", { width: 1440, height: 900 }], ["phone", { width: 390, height: 844 }]] as const) {
    const consoles = {
      "whole server": { admin: "pagespeed_global_admin", replies: () => steady() },
      "per host": {
        admin: "pagespeed_admin",
        replies: () => steady({ config: F.CONFIG_VHOST, stats_json: statsStarted(F.STATS_VHOST, 26 * HOUR) }),
      },
      "module only": {
        admin: "pagespeed_admin",
        replies: () =>
          steady({
            config: F.CONFIG_VHOST,
            stats_json: statsStarted(F.STATS_MODULE_ONLY, 26 * HOUR),
            "v1/daemon/health": F.NOT_CONFIGURED,
            "v1/daemon/stats": F.NOT_CONFIGURED,
          }),
      },
    } as const;
    for (const [consoleName, { admin, replies }] of Object.entries(consoles)) {
      test(`${name}, ${consoleName}: the verdict and the leading figure are in the first viewport`, async ({ page }) => {
        await page.setViewportSize(size);
        await open(page, admin, replies());
        await expect(page.getByTestId("health-summary")).toContainText(/^Working\. .*\d.* since /);
        expect(await inFirstViewport(page, "health-summary")).toBe(true);
        expect(await inFirstViewport(page, "overview-lead-figure")).toBe(true);
        await expect(page.getByTestId("overview-lead-figure")).toHaveText(/\d.*(B|KB|MB|GB)/);
      });
    }
  }
});

test.describe("Overview time bases and scope", () => {
  const golden = (): Partial<Record<Leaf, Responder>> => ({
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    stats_json: F.STATS_JSON_GLOBAL,
    "v1/daemon/stats": F.OPT_STATS_PAGE,
  });

  test("the cache-serve line carries its percentage and caveat, and the tile links to Savings", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", golden());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    const served = page.getByTestId("module-served");
    await expect(served).toContainText("%");
    // The same counter caveat as the Savings page: the rate leans upward.
    await expect(served).toContainText(
      "cacheable resource requests; conditional and HEAD requests count as served",
    );
    await expect(served.locator("[title]")).toHaveAttribute(
      "title",
      "Share of cacheable resource requests (CSS, JavaScript, images) answered from the optimizer's cache; conditional and HEAD requests count as served",
    );
    await expect(served.getByRole("link")).toHaveCount(0);
    const link = page.getByTestId("module-served-link");
    await expect(link).toHaveAttribute("href", "#/savings");
    await link.click();
    await expect(page).toHaveURL(/#\/savings$/);
  });

  test("both cards carry their own since stamps; the whole-server console needs no scope chips", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", golden());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("module-card").getByTestId("since-module")).toHaveCount(0);
    await expect(page.getByTestId("tile-rewriting").getByTestId("since-module")).toContainText("since the web server started");
    await expect(page.getByTestId("since-optimizer")).toContainText("since the optimizer started");
  });

  test("the per-host console mutes the optimizer card and names the host scope", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...golden(), config: F.CONFIG_VHOST });
    await openConsole(page, "pagespeed_admin", "#/overview");
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveText("whole server");
    await expect(page.getByTestId("optimizer-card")).toHaveClass(/card-muted/);
    await expect(page.getByTestId("optimizer-scope-note").locator("a")).toHaveAttribute(
      "href",
      /pagespeed_global_admin\/#\/savings$/,
    );
  });
});

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Overview muted per-host optimizer card (${scheme})`, () => {
    test.use({ colorScheme: scheme });
    test("is visibly set apart from the host's own card", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        config: F.CONFIG_VHOST,
        stats_json: F.STATS_JSON_GLOBAL,
        "v1/daemon/stats": F.OPT_STATS_PAGE,
      });
      await openConsole(page, "pagespeed_admin", "#/overview");
      const muted = page.getByTestId("optimizer-card");
      await expect(muted).toHaveClass(/card-muted/);
      await expect(muted).toHaveCSS("border-top-style", "dashed");
      await expect(page.getByTestId("module-card")).toHaveCSS("border-top-style", "solid");
      if (scheme === "dark") {
        const opacity = Number(await muted.evaluate((el) => getComputedStyle(el).opacity));
        expect(opacity).toBeLessThan(1);
      }
    });
  });
}

test.describe("Overview alerts", () => {
  // Take the next sample now and wait until its optimizer read was answered,
  // so consecutive refreshes are processed in order.
  const refresh = async (page: Page) => {
    await Promise.all([
      page.waitForResponse((r) => r.url().endsWith("/v1/daemon/stats")),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
  };

  test("healthy: no alerts, a healthy summary", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy());
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings");
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("daemon absent (unreachable): one warning linking to Optimizer status", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": F.UNREACHABLE }));
    const alert = page.getByTestId("alert-daemon-unreachable");
    await expect(alert).toContainText("configured but not answering");
    await expect(page.getByTestId("alerts").locator('[data-testid^="alert-"]')).toHaveCount(1);
    // The banner is one polite live region; individual alerts do not each
    // re-announce on every poll.
    await expect(page.getByTestId("alerts")).toHaveAttribute("role", "status");
    await expect(page.getByTestId("alerts")).toHaveAttribute("aria-live", "polite");
    await expect(alert).not.toHaveAttribute("role", "alert");
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 1 needs action");
    await alert.getByRole("link", { name: "Details" }).click();
    await expect(page).toHaveURL(/#\/optimizer$/);
  });

  test("daemon absent (not configured): nothing to alert", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.NOT_CONFIGURED, "v1/daemon/stats": F.NOT_CONFIGURED }));
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings");
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("per-vhost and global consoles raise the same optimizer alert", async ({ page }) => {
    for (const admin of ["pagespeed_admin", "pagespeed_global_admin"] as const) {
      await page.unrouteAll({ behavior: "ignoreErrors" });
      await open(page, admin, healthy({ "v1/daemon/health": F.HEALTH_CHECK_FAILING }));
      await expect(page.getByTestId("alert-daemon-check-failed")).toContainText("cache_open");
    }
  });

  test("empty statistics: nothing to alert", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: F.STATS_EMPTY, "v1/daemon/stats": F.DAEMON_STATS_EMPTY }));
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings");
  });

  test("optimizer below the floor (endpoint not provided): the update warning", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.UNSUPPORTED }));
    await expect(page.getByTestId("alert-daemon-outdated")).toContainText("(2.0.41) does not provide");
  });

  test("optimizer below the floor (version older than the minimum): the update warning", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BELOW_FLOOR }));
    await expect(page.getByTestId("alert-daemon-outdated")).toContainText("(2.0.3) is older than the oldest this console supports");
  });

  test("rate rule: fires on an increase between two samples, never on the first", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": (n) => F.daemonStats({ errorsTotal: n === 0 ? 5 : 8 }) }));
    await expect(page.getByTestId("alerts")).toHaveCount(0);
    await refresh(page);
    await expect(page.getByTestId("alert-error-rate")).toContainText("3 new errors (8 since it started)");
  });

  test("module fetch failures link to the module's messages in Logs", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: (n) => F.statsWithFetchFailures(n === 0 ? 0 : 2) }));
    await refresh(page);
    const alert = page.getByTestId("alert-fetch-failures");
    await expect(alert).toBeVisible();
    await alert.getByRole("link", { name: "Details" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=warning$/);
  });

  test("errors rank before warnings", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/health": F.HEALTH_CHECK_FAILING,
      "v1/daemon/stats": F.daemonStats({ originMisconfiguration: 2 }),
    }));
    const alerts = page.getByTestId("alerts").locator('[data-testid^="alert-"]');
    await expect(alerts).toHaveCount(2);
    await expect(alerts.first()).toHaveAttribute("data-testid", "alert-daemon-check-failed");
    await expect(page.getByTestId("findings-summary")).toHaveText("2 findings · 2 need action");
  });

  // Weakly ordered by design: a regular 5 s refresh can consume a responder
  // index, so the "still hidden" check may pass because the condition already
  // cleared. The acknowledgement semantics are pinned exactly by the alert
  // tracker's own unit tests; this test pins the wiring (button, re-arm,
  // final reappearance).
  test("acknowledgement: stays hidden while the condition holds, returns after it clears", async ({ page }) => {
    // Samples 0-1: condition holds; 2: clears; 3 and later: holds again.
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/stats": (n) => F.daemonStats({ originMisconfiguration: n === 2 ? 0 : 2 }),
    }));
    const alert = page.getByTestId("alert-origin-misconfiguration");
    await expect(alert).toBeVisible();
    await alert.getByRole("button", { name: /^Acknowledge / }).click();
    await expect(alert).toHaveCount(0);
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings · 1 acknowledged");
    await refresh(page);
    await expect(alert).toHaveCount(0);
    await refresh(page);
    await refresh(page);
    // A regular 5 s refresh may interleave; it only moves the sequence on.
    await expect(alert).toBeVisible({ timeout: 12_000 });
  });

  test("an acknowledgement survives moving to another page and back", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.daemonStats({ originMisconfiguration: 2 }) }));
    await page.getByTestId("alert-origin-misconfiguration").getByRole("button", { name: /^Acknowledge / }).click();
    await page.getByRole("link", { name: "About", exact: true }).click();
    await page.getByRole("link", { name: "Overview", exact: true }).click();
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.getByTestId("alert-origin-misconfiguration")).toHaveCount(0);
  });
});
