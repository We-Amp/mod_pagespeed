// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Every page, in both colour schemes, checked with axe-core: no WCAG 2.0/2.1
// A or AA violation, and headings, landmarks and regions in order.
// Self-contained (support/mock-admin.ts); run after build.sh.

import AxeBuilder from "@axe-core/playwright";
import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { NETWORK_ERROR, mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const ROUTES = [
  "#/overview", "#/savings", "#/statistics", "#/configuration", "#/histograms", "#/caches", "#/console", "#/messages",
  "#/graphs", "#/graphs?counters=all", "#/optimizer", "#/optimizer?section=cache", "#/urls", "#/logs", "#/support", "#/about",
  // The "Show all" link only renders once a severity filter is present.
  "#/messages?level=warning",
  "#/messages?level=info",
];

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

async function expectNoViolations(page: Page) {
  const wcag = await new AxeBuilder({ page }).withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"]).analyze();
  const structure = await new AxeBuilder({ page })
    .withRules(["heading-order", "page-has-heading-one", "landmark-one-main", "landmark-unique", "region"])
    .analyze();
  const report = [...wcag.violations, ...structure.violations].map(
    (v) => `${v.id}: ${v.nodes.map((n) => n.target.join(" ")).join(" | ")}`,
  );
  expect(report).toEqual([]);
}

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Accessibility (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    for (const hash of ROUTES) {
      test(`${hash}`, async ({ page }) => {
        await mockAdmin(page, "pagespeed_admin", F.allReplies());
        await openConsole(page, "pagespeed_admin", hash);
        await ready(page);
        await expectNoViolations(page);
      });
    }

    test("the shortcut list", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", F.allReplies());
      await openConsole(page, "pagespeed_admin", "#/statistics");
      await ready(page);
      await page.keyboard.press("?");
      await expect(page.getByRole("dialog", { name: "Keyboard shortcuts" })).toBeVisible();
      await expectNoViolations(page);
    });

    test("the sidebar at phone width, drawer open", async ({ page }) => {
      await page.setViewportSize({ width: 390, height: 844 });
      await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
      await openConsole(page, "pagespeed_global_admin", "#/statistics");
      await ready(page);
      await page.getByRole("button", { name: "Toggle menu" }).click();
      await expect(page.getByRole("navigation", { name: "Console pages" })).toBeVisible();
      await expectNoViolations(page);
    });

    test("the whole-server sidebar at desktop width", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
      await openConsole(page, "pagespeed_global_admin", "#/graphs");
      await ready(page);
      await expectNoViolations(page);
    });

    test("the host lens selected, on the URL index and on a page it does not apply to", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/cache/urls": F.URLS_TWO_PAGES,
      });
      await openConsole(page, "pagespeed_global_admin", "#/urls?lens=www.example.test");
      await ready(page);
      await expectNoViolations(page);
      await openConsole(page, "pagespeed_global_admin", "#/statistics?lens=www.example.test");
      await ready(page);
      await expect(page.getByTestId("lens-note")).toBeVisible();
      await expectNoViolations(page);
    });

    test("an alert on the overview", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), "v1/daemon/health": F.HEALTH_CHECK_FAILING });
      await openConsole(page, "pagespeed_admin");
      await ready(page);
      await expect(page.getByTestId("alert-daemon-check-failed")).toBeVisible();
      await expectNoViolations(page);
    });

    test("findings, with an acknowledged one", async ({ page }) => {
      const now = Date.now();
      await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        message_history: F.messageGroups([
          { level: "warning", template: F.ACL_TEMPLATE, count: 1, last_ms: now - 60_000 },
          { level: "warning", template: F.VOLUME_TEMPLATE, count: 1, recent: 1, last_ms: now - 60_000 },
          { level: "warning", template: "Slow origin response for URL", count: 3, recent: 3, last_ms: now - 60_000 },
        ]),
      });
      await openConsole(page, "pagespeed_admin");
      await ready(page);
      await page.getByRole("button", { name: "Acknowledge Admin console reachable from the network" }).click();
      await page.getByTestId("findings-acknowledged").locator("summary").click();
      await expect(page.getByTestId("acknowledged-admin-exposed")).toBeVisible();
      await expectNoViolations(page);
    });

    test("messages with repeats expanded", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: F.MESSAGES_REPEATS });
      await openConsole(page, "pagespeed_admin", "#/messages");
      await ready(page);
      await page.getByRole("button", { name: "Show 3 entries" }).click();
      await expect(page.getByTestId("log-entries")).toBeVisible();
      await expectNoViolations(page);
    });

    test("support with the diagnostics shown to copy by hand", async ({ page }) => {
      await page.addInitScript(() => {
        Object.defineProperty(navigator, "clipboard", {
          configurable: true,
          value: { writeText: async () => Promise.reject(new Error("denied")) },
        });
        document.execCommand = (() => false) as typeof document.execCommand;
      });
      await mockAdmin(page, "pagespeed_admin", F.allReplies());
      await openConsole(page, "pagespeed_admin", "#/support");
      await ready(page);
      await page.getByRole("button", { name: "Copy diagnostics" }).click();
      await expect(page.getByLabel("Diagnostics text")).toBeVisible();
      await expectNoViolations(page);
    });

    test("the savings split over real optimizer and module statistics", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        stats_json: F.STATS_JSON_GLOBAL,
        "v1/daemon/stats": F.OPT_STATS_PAGE,
      });
      await openConsole(page, "pagespeed_global_admin", "#/savings");
      await ready(page);
      await expect(page.getByTestId("savings-split-css")).toBeVisible();
      await expectNoViolations(page);
    });

    for (const hash of ["#/overview", "#/savings"]) {
      test(`${hash} with a warming optimizer and its time-base stamps`, async ({ page }) => {
        await mockAdmin(page, "pagespeed_global_admin", {
          ...F.allReplies(),
          config: F.CONFIG_GLOBAL,
          stats_json: F.STATS_JSON_GLOBAL,
          "v1/daemon/stats": F.optStatsStarted(2 * 60_000),
        });
        await openConsole(page, "pagespeed_global_admin", hash);
        await ready(page);
        await expect(page.getByTestId("warming-pill").first()).toBeVisible();
        await expectNoViolations(page);
      });
    }

    test("the savings chart with its window title and in-card legend", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        stats_json: F.STATS_JSON_GLOBAL,
        "v1/daemon/stats": F.OPT_STATS_PAGE,
      });
      await openConsole(page, "pagespeed_global_admin", "#/savings");
      await ready(page);
      await expect(page.getByTestId("savings-chart-legend")).toBeVisible({ timeout: 15_000 });
      await expectNoViolations(page);
    });

    test("the savings tables with wider glyphs, on a desktop and where a table scrolls on a phone", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        stats_json: F.STATS_JSON_GLOBAL,
        "v1/daemon/stats": F.OPT_STATS_PAGE,
      });
      for (const size of [
        { width: 1280, height: 720 },
        { width: 390, height: 844 },
      ]) {
        await page.setViewportSize(size);
        await openConsole(page, "pagespeed_global_admin", "#/savings");
        await ready(page);
        await expect(page.getByTestId("savings-by-host")).toBeVisible();
        // Wider glyphs, like some systems' fonts.
        await page.addStyleTag({ content: "body { letter-spacing: 0.05em; }" });
        if (size.width === 390) {
          // The by-host table does scroll here: the case the scrollable region must handle.
          const scrolls = await page.locator(".table-scroll").first().evaluate((el) => el.scrollWidth > el.clientWidth);
          expect(scrolls).toBe(true);
        }
        await expectNoViolations(page);
      }
    });

    test("statistics once counters have moved (rising and falling deltas)", async ({ page }) => {
      const firstVars = (F.STATS_VHOST.body as { variables: Record<string, number> }).variables;
      const secondReply = {
        status: 200,
        body: {
          variables: {
            ...firstVars,
            num_flushes: firstVars.num_flushes + 25,
            ipro_daemon_served: Math.max(0, firstVars.ipro_daemon_served - 3),
          },
          maxlength: 60,
        },
      };
      const mock = await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        stats_json: (call) => (call === 0 ? F.STATS_VHOST : secondReply),
      });
      await openConsole(page, "pagespeed_admin", "#/statistics");
      await ready(page);
      await page.getByRole("checkbox", { name: "Δ since open" }).check();
      await page.getByRole("button", { name: "Refresh", exact: true }).click();
      await expect.poll(() => mock.calls("stats_json")).toBeGreaterThanOrEqual(2);
      await expect(page.locator(".delta-pos").first()).toBeVisible();
      await expect(page.locator(".delta-neg").first()).toBeVisible();
      await expectNoViolations(page);
    });

    test("the reconnect banner", async ({ page }) => {
      // config must fail too: if it alone answers, its success can settle
      // after stats_json's failure and clear the banner (a race, not a
      // scheme difference), so the fixture takes the whole server down.
      await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: NETWORK_ERROR, stats_json: NETWORK_ERROR });
      await openConsole(page, "pagespeed_admin", "#/statistics");
      await expect(page.getByTestId("connection-banner")).toBeVisible();
      await expectNoViolations(page);
    });
  });
}

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Accessibility, whole-server URLs (${scheme})`, () => {
    test.use({ colorScheme: scheme });
    test("#/urls populated", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/cache/urls": F.URLS_TWO_PAGES,
      });
      await openConsole(page, "pagespeed_global_admin", "#/urls");
      await ready(page);
      await expectNoViolations(page);
    });

    test("#/urls/detail populated", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/cache/alternates": F.ALTERNATES_HERO,
        "v1/daemon/cache/content": F.CONTENT_HERO,
      });
      await openConsole(
        page,
        "pagespeed_global_admin",
        "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https",
      );
      await ready(page);
      await expectNoViolations(page);
    });
  });
}

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Accessibility, whole-server Optimizer Logs (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    test("#/logs populated", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/logs": F.LOGS_PAGE,
      });
      await openConsole(page, "pagespeed_global_admin", "#/logs");
      await ready(page);
      await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
      await expectNoViolations(page);
    });

    test("#/logs filtered to errors", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/logs": F.LOGS_LEVELS,
      });
      await openConsole(page, "pagespeed_global_admin", "#/logs?level=error");
      await ready(page);
      await expect(page.getByTestId("logs-filter-note")).toBeVisible();
      await expectNoViolations(page);
    });

    // axe's scrollable-region-focusable rule only fires when the region
    // actually overflows: 80 rows do, so the keyboard focus the timeline
    // needs is checked for real.
    test("#/logs with a timeline that overflows its box", async ({ page }) => {
      await mockAdmin(page, "pagespeed_global_admin", {
        ...F.allReplies(),
        config: F.CONFIG_GLOBAL,
        "v1/daemon/logs": F.logsStream(80),
      });
      await openConsole(page, "pagespeed_global_admin", "#/logs?source=optimizer&level=debug&group=0");
      await ready(page);
      const timeline = page.getByTestId("logs-timeline");
      await expect(page.getByText("stream entry 79", { exact: true })).toBeVisible();
      expect(await timeline.evaluate((el) => el.scrollHeight > el.clientHeight)).toBe(true);
      await expectNoViolations(page);
    });
  });
}
