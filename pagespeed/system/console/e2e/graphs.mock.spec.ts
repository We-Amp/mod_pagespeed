// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, test } from "@playwright/test";
import { LABEL_INSET, valueAxisFit } from "./support/chart-axis";
import { mockAdmin, openConsole } from "./support/mock-admin";
import * as F from "./support/fixtures";

test.use({ locale: "en-US", timezoneId: "UTC" });

/** stats_json whose num_flushes the caller sequences; carries the gauges field. */
const flushing = (n: number) => ({
  status: 200,
  body: { variables: { num_flushes: n }, timestamp_ms: 1_790_000_000_000, gauges: [] },
});

/** stats_json without the gauges field, like a module that predates it. */
const flushingWithoutGauges = (n: number) => ({
  status: 200,
  body: { variables: { num_flushes: n }, timestamp_ms: 1_790_000_000_000 },
});

test("a fresh page draws the logged history immediately", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing(100 + n * 10),
    graphs: F.graphsReply(["num_flushes", "cache_hits"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  // Log points on the first paint: the one-minute cadence cannot come from
  // the five-second live poll, and no second statistics sample was needed.
  await expect(page.getByRole("img", { name: /num_flushes \(per second\)/ })).toBeVisible();
  await expect(page.getByRole("img", { name: /cache_hits \(per second\)/ })).toBeVisible();
  expect(mock.calls("graphs")).toBe(1);
});

test("the statistics log is reread at most once per minute", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing(100 + n * 10),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  // Several statistics polls later the log has still been read exactly once:
  // it gains one sample per logging interval, so faster rereads buy nothing.
  await expect.poll(() => mock.calls("stats_json"), { timeout: 20_000 }).toBeGreaterThanOrEqual(3);
  expect(mock.calls("graphs")).toBe(1);
});

test("the live tail merges into the logged series", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing(100 + n * 10),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
  // The newest live value is a rate per second: finite, with the unit. The
  // exact figure depends on poll spacing, so the shape is asserted, never
  // the value.
  const chart = page.getByRole("img", { name: /num_flushes \(per second\)/ });
  await expect(chart).toHaveAccessibleName(/latest [0-9.]+\/s/);
  await expect(
    page.getByTestId("graph-card-num_flushes").getByTestId("graph-live-since"),
  ).toContainText(/live since/i);
});

test("only samples inside the selected range are drawn", async ({ page }) => {
  // One logged sample older than the default 15-minute range and one inside
  // it, both stamped at request time: the out-of-range one must not count.
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: flushingWithoutGauges(200),
    graphs: () => {
      const now = Date.now();
      return {
        status: 200,
        body: { timestamps: [now - 20 * 60_000, now - 60_000], variables: { num_flushes: [100, 150] } },
      };
    },
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  const chart = page.getByRole("img", { name: /^num_flushes:/ });
  await expect(chart).toBeVisible();
  await page.getByRole("button", { name: "Auto-refresh · 5 s" }).click();
  // The text alternative counts the points: one in-range log sample plus the
  // one live sample — the 20-minute-old log sample is not drawn.
  await expect(chart).toHaveAccessibleName(/latest 200, 2 points/);
});

test("a counter reset shows a gap, never a negative spike", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing([100, 150, 50][Math.min(n, 2)]),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect.poll(() => mock.calls("stats_json"), { timeout: 20_000 }).toBeGreaterThanOrEqual(3);
  await page.getByRole("button", { name: "Auto-refresh · 5 s" }).click();
  const card = page.getByTestId("graph-card-num_flushes");
  // The rate right after the reset is a gap in the series: the raw table
  // draws it as a dash, while "Latest" and the chart's text alternative
  // still name the newest rate before the gap.
  await card.getByText("Raw data").click();
  const rows = card.getByRole("row");
  await expect(rows.last()).toContainText("—");
  await expect(rows.nth(-2)).toContainText("/s");
  const latest = card.getByTestId("graph-latest");
  await expect(latest).toHaveText(/^[0-9.]+\/s$/);
  const latestText = (await latest.textContent())?.trim() ?? "";
  const escaped = latestText.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
  await expect(card.getByRole("img")).toHaveAccessibleName(new RegExp(`latest ${escaped}`));
});

test("a gauge that goes down is drawn raw: no delta, no gap", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => ({
      status: 200,
      body: {
        variables: { inflight: [100, 50, 25][Math.min(n, 2)], num_flushes: 100 + n * 10 },
        timestamp_ms: 1_790_000_000_000,
        gauges: ["inflight"],
      },
    }),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect.poll(() => mock.calls("stats_json"), { timeout: 20_000 }).toBeGreaterThanOrEqual(3);
  // Raw falling values: never a difference, never a gap, title unmarked.
  const card = page.getByTestId("graph-card-inflight");
  const chart = card.getByRole("img", { name: /^inflight:/ });
  await expect(chart).toHaveAccessibleName(/latest 25/);
  await expect(card.getByTestId("graph-latest")).toHaveText("25");
  // The plain counter next to it is still rated.
  await expect(page.getByRole("img", { name: /num_flushes \(per second\)/ })).toBeVisible();
});

test("a counter the log does not have is marked live-only", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing(100 + n * 10),
    graphs: F.graphsReply(["cache_hits"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  const card = page.getByTestId("graph-card-num_flushes");
  await expect(card.getByTestId("graph-live-only")).toHaveText("No logged history for this counter.");
  // The logged counter carries no such note.
  await expect(page.getByTestId("graph-card-cache_hits").getByTestId("graph-live-only")).toHaveCount(0);
});

test("more than 48 counters page through a show-more button", async ({ page }) => {
  const many = {
    status: 200,
    body: {
      variables: Object.fromEntries(
        Array.from({ length: 50 }, (_, i) => [`var_${String(i).padStart(2, "0")}`, i + 1]),
      ),
      timestamp_ms: 1_790_000_000_000,
    },
  };
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: many,
    graphs: F.graphsReply(["var_00"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect(page.getByTestId("graph-card-var_47")).toBeVisible();
  expect(await page.getByRole("img").count()).toBe(48);
  const more = page.getByTestId("graphs-show-more");
  await expect(more).toHaveText("Show 48 more (2 remaining)");
  await more.click();
  expect(await page.getByRole("img").count()).toBe(50);
  await expect(page.getByTestId("graphs-show-more")).toHaveCount(0);
  // Changing the filter resets the paging.
  await page.getByLabel("Filter graphs").fill("var_4");
  await expect(page.getByTestId("graph-card-var_49")).toBeVisible();
  await expect(page.getByTestId("graphs-show-more")).toHaveCount(0);
});

test("no graphs endpoint: the absent notice shows and the live charts still draw", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: F.STATS_VHOST,
    // graphs leaf unmocked: the harness answers 404.
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect(page.getByTestId("graphs-absent")).toBeVisible();
  // A page with live samples is never a full-page empty state: the counters
  // from the live poll draw, each marked live-only.
  await expect(page.getByRole("img").first()).toBeVisible();
  await expect(page.getByTestId("graph-live-only").first()).toBeVisible();
});

test("an empty log and an empty range are different sentences, and the live charts still draw", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: F.STATS_VHOST,
    graphs: (n) =>
      n === 0
        ? { status: 200, body: {} }
        : { status: 200, body: { timestamps: [], variables: {} } },
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect(page.getByTestId("graphs-no-log")).toBeVisible();
  await expect(page.getByRole("img").first()).toBeVisible();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByTestId("graphs-empty-range")).toBeVisible();
  await expect(page.getByRole("img").first()).toBeVisible();
});

test("the global console points at a per-host view when the range is empty, with live charts", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    config: F.CONFIG_GLOBAL,
    stats_json: F.STATS_GLOBAL,
    graphs: { status: 200, body: { timestamps: [], variables: {} } },
  });
  await openConsole(page, "pagespeed_global_admin", "#/graphs?counters=all");
  await expect(page.getByTestId("graphs-empty-range")).toBeVisible();
  await expect(page.getByTestId("graphs-global-idle")).toBeVisible();
  await expect(page.getByRole("img").first()).toBeVisible();
});

test("whole-server console, empty log, live counters: charts, the idle-log notice and the per-host link", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    config: F.CONFIG_GLOBAL,
    stats_json: F.STATS_GLOBAL,
    graphs: { status: 200, body: {} },
  });
  await openConsole(page, "pagespeed_global_admin", "#/graphs?counters=all");
  // The whole-server log is idle (per-vhost statistics on a module without
  // the backend fix), but the counters are not: they draw within one poll.
  await expect(page.getByTestId("graphs-no-log")).toBeVisible();
  const idle = page.getByTestId("graphs-global-idle");
  await expect(idle).toBeVisible();
  await expect(idle.getByRole("link")).toHaveAttribute("href", /pagespeed_admin/);
  await expect(page.getByRole("img").first()).toBeVisible();
});

test("before the first statistics sample the page says it is warming up", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    // No counters at all: a reply with counters (even all-zero ones, like
    // F.STATS_EMPTY) carries samples the live tail would draw.
    stats_json: { status: 200, body: { variables: {}, timestamp_ms: 1_790_000_000_000 } },
    graphs: { status: 200, body: {} },
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  // Neither source has anything to draw: the only full-page empty state.
  await expect(page.getByTestId("graphs-live-warming")).toBeVisible();
  await expect(page.getByRole("img")).toHaveCount(0);
});

test("the rate default follows the reply's gauges field, and the toggle wins", async ({ page }) => {
  // Without a gauges field the module predates it: the default stays off.
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushingWithoutGauges(100 + n * 10),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  await expect(page.getByLabel("Rates per second")).not.toBeChecked();
  await expect(page.getByRole("img", { name: /^num_flushes:/ })).toBeVisible();
  await page.getByLabel("Rates per second").check();
  await expect(page.getByRole("img", { name: /num_flushes \(per second\)/ })).toBeVisible();
});

test("with a gauges field the rate view defaults on and the toggle turns it off", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    stats_json: (n: number) => flushing(100 + n * 10),
    graphs: F.graphsReply(["num_flushes"]),
  });
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  const toggle = page.getByLabel("Rates per second");
  await expect(toggle).toBeChecked();
  await expect(page.getByRole("img", { name: /num_flushes \(per second\)/ })).toBeVisible();
  await toggle.uncheck();
  // Cumulative view now: the marked title is gone and the latest value is
  // the raw counter, not a rate.
  await expect(page.getByRole("img", { name: /num_flushes \(per second\)/ })).toHaveCount(0);
  await expect(page.getByRole("img", { name: /^num_flushes:/ })).toBeVisible();
});

test("the toolbar controls are the console's, not the browser's", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
  const range = page.getByLabel("Time range");
  await expect(range).toHaveCSS("padding", "8px 16px");
  await expect(range).toHaveCSS("border-radius", "4px");
  const search = page.getByLabel("Filter graphs");
  await expect(search).toHaveCSS("padding", "8px 16px");
  await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
  // Behaviour is unchanged: choosing a range invalidates the poll.
  await range.selectOption("60");
  await expect(page.getByTestId("graph-card-num_flushes").or(page.getByTestId("graphs-no-log"))).toBeVisible();
});

test.describe("the default charts", () => {
  /** Module counters that move: 9 of every 10 optimizable requests served, one fetch failure per poll. */
  const moving = (n: number) => ({
    status: 200,
    body: {
      variables: {
        ipro_daemon_served: 100 + 9 * n,
        ipro_daemon_fallthrough: 50 + n,
        ipro_daemon_fallthrough_css: 10 + n,
        ipro_daemon_fallthrough_js: 0,
        ipro_daemon_fallthrough_image: 0,
        css_filter_total_bytes_saved: 1000 + 500 * n,
        num_resource_fetch_failures: n,
        num_flushes: 10 + n,
      },
      timestamp_ms: 1_790_000_000_000,
      gauges: [],
    },
  });

  test("six curated charts, drawn once they have data; the rest wait in one empty state", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      stats_json: (n: number) => moving(n),
      graphs: { status: 200, body: {} },
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect(page.getByRole("heading", { level: 1, name: "Graphs" })).toBeVisible();
    await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
    for (const id of ["requests", "module_saved", "optimizer_saved", "hit_rate", "fetch_failures", "queue"]) {
      await expect(page.getByTestId(`curated-${id}`)).toBeVisible();
    }
    await expect(page.getByTestId("curated-hit_rate").getByTestId("curated-latest")).toHaveText("90%");
    await expect(page.getByTestId("curated-hidden")).toHaveCount(0);
    // Only the curated set: a plain counter is not drawn until it is added.
    await expect(page.getByTestId("graph-card-num_flushes")).toHaveCount(0);
  });

  test("an optimizer that does not answer: its two charts wait in the empty state", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.UNREACHABLE,
      graphs: { status: 200, body: {} },
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
    const hidden = page.getByTestId("curated-hidden");
    await expect(hidden).toContainText("Optimizer serve savings");
    await expect(hidden).toContainText("Optimizer jobs in progress");
    await expect(page.getByTestId("curated-requests")).toBeVisible();
  });

  test("the module's charts draw the statistics log's history at once", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      stats_json: (n: number) => moving(n),
      graphs: () => {
        const now = Date.now();
        return {
          status: 200,
          body: {
            timestamps: [now - 120_000, now - 60_000],
            variables: { num_resource_fetch_failures: [0, 6], ipro_daemon_served: [10, 20], ipro_daemon_fallthrough: [5, 5] },
          },
        };
      },
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    // Two logged samples are a rate before the second live poll.
    await expect(page.getByTestId("curated-fetch_failures")).toBeVisible();
    await expect(page.getByTestId("curated-fetch_failures").getByTestId("curated-latest")).toHaveText(/\/s$/);
  });

  test("add a counter pins it next to the curated charts, and the address keeps it", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: (n: number) => moving(n) });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await page.getByLabel("Add a counter").fill("num_fl");
    await page.getByTestId("graphs-add").getByRole("button", { name: "Add num_flushes" }).click();
    await expect(page.getByTestId("graph-card-num_flushes")).toBeVisible();
    await expect(page).toHaveURL(/#\/graphs\?counters=num_flushes$/);
    await page.getByRole("button", { name: "Remove num_flushes" }).click();
    await expect(page.getByTestId("graph-card-num_flushes")).toHaveCount(0);
    await expect(page).toHaveURL(/#\/graphs$/);
  });

  test("a link with pinned counters shows them; a name outside the grammar is ignored", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: (n: number) => moving(n) });
    await openConsole(page, "pagespeed_admin", "#/graphs?counters=num_flushes,bogus%3Cb%3E");
    await expect(page.getByTestId("graph-card-num_flushes")).toBeVisible();
    // Only the pinned counter joins the default charts: a counter the link
    // does not name gets no card of its own.
    await expect(page.getByTestId("graph-card-num_resource_fetch_failures")).toHaveCount(0);
    await expect(page.locator("main b")).toHaveCount(0);
    expect(errors).toEqual([]);
  });

  test("every counter is one link away, and back", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: (n: number) => moving(n) });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await page.getByTestId("graphs-show-all").click();
    await expect(page).toHaveURL(/#\/graphs\?counters=all$/);
    await expect(page.getByLabel("Filter graphs")).toBeVisible();
    await expect(page.getByTestId("graph-card-num_flushes")).toBeVisible();
    await page.getByTestId("graphs-show-default").click();
    await expect(page).toHaveURL(/#\/graphs$/);
    await expect(page.getByLabel("Add a counter")).toBeVisible();
  });

  test("with a lens, the optimizer savings chart names and follows that host", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_PAGE,
    });
    await openConsole(page, "pagespeed_global_admin", "#/graphs?lens=www.example.com");
    await expect(page.getByTestId("curated-lens-note")).toContainText("www.example.com");
    await expect(page.getByRole("heading", { name: "Optimizer serve savings — www.example.com" })).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("lens-note")).toHaveCount(0);
  });

  test("a per-host console: the optimizer savings chart names the host it follows", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      config: F.CONFIG_VHOST_STATIC,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_PAGE,
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect(page.getByRole("heading", { name: "Optimizer serve savings — static.example.com" })).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("host-lens")).toHaveCount(0);
  });

  test("a per-host console: the optimizer savings chart follows the site the module gives", async ({ page }) => {
    // Opened as www.example.test; the module narrows the answer for example.test.
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      config: F.CONFIG_VHOST,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_SITE_ROW,
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect(page.getByRole("heading", { name: "Optimizer serve savings — example.test" })).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("curated-whole-server-note")).toHaveCount(0);
  });

  test("a per-host console without a usable site: the optimizer savings chart says it covers the whole server", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      config: F.CONFIG_VHOST,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_SITE_NONE,
    });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect(page.getByRole("heading", { name: "Optimizer serve savings", exact: true })).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("curated-optimizer_saved")).not.toContainText("www.example.test");
    await expect(page.getByTestId("curated-whole-server-note")).toHaveText("The optimizer savings chart covers the whole server.");
  });

  test("the whole-server console's optimizer savings chart carries no whole-server note", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_PAGE,
    });
    await openConsole(page, "pagespeed_global_admin", "#/graphs");
    await expect(page.getByRole("heading", { name: "Optimizer serve savings", exact: true })).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("curated-whole-server-note")).toHaveCount(0);
  });

  test("a lens change mid-session: the optimizer savings series restarts under the new host", async ({ page }) => {
    // The fixture's figures never move, so every honest rate is zero; the
    // whole server's saving (400 kB) minus www.example.com's (0) over one
    // poll would read as a rate in kB/s if a sample pair spanned the change.
    await page.clock.install();
    const mock = await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      stats_json: (n: number) => moving(n),
      "v1/daemon/stats": F.OPT_STATS_PAGE,
    });
    await openConsole(page, "pagespeed_global_admin", "#/graphs?lens=www.example.com");
    const chart = page.getByTestId("curated-optimizer_saved");
    // One more statistics read, answered and drawn.
    const poll = async () => {
      const before = mock.calls("stats_json");
      await expect
        .poll(async () => {
          await page.clock.runFor(1_000);
          return mock.calls("stats_json");
        })
        .toBeGreaterThan(before);
      await page.waitForTimeout(150);
    };
    await poll();
    await poll();
    await expect(chart.getByRole("heading", { name: "Optimizer serve savings — www.example.com" })).toBeVisible();
    await expect(chart.getByTestId("curated-latest")).toHaveText("0 B/s");

    await page.getByTestId("host-lens").getByRole("combobox", { name: "Host" }).selectOption("");
    // The host's history is not drawn under the whole server's title.
    await expect(page.getByTestId("curated-hidden")).toContainText("Optimizer serve savings");
    await expect(chart).toHaveCount(0);
    // One sample for the whole server: still no rate, never a spike.
    await poll();
    await expect(chart).toHaveCount(0);
    // The second whole-server sample starts the series again.
    await poll();
    await expect(chart.getByRole("heading", { name: "Optimizer serve savings", exact: true })).toBeVisible();
    await expect(chart.getByTestId("curated-latest")).toHaveText("0 B/s");
  });

  for (const [name, viewport] of [
    ["desktop", { width: 1280, height: 900 }],
    ["phone", { width: 390, height: 844 }],
  ] as const) {
    test(`${name}: the module savings chart's value axis shows its numbers in full`, async ({ page }) => {
      await page.setViewportSize(viewport);
      // About 10 KB of rewrite savings per second: KB/s labels.
      const fast = (n: number) => {
        const r = moving(n);
        r.body.variables.css_filter_total_bytes_saved = 1000 + 50_000 * n;
        return r;
      };
      const mock = await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        stats_json: (n: number) => fast(n),
        graphs: { status: 200, body: {} },
      });
      await openConsole(page, "pagespeed_admin", "#/graphs");
      await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
      const fit = await valueAxisFit(page.getByTestId("curated-module_saved"));
      expect(fit.labels.some((s) => /^\d+(\.\d+)? KB\/s$/.test(s))).toBe(true);
      expect(fit.plotLeft).toBeGreaterThanOrEqual(Math.ceil(fit.widest));
      expect(fit.plotLeft).toBeGreaterThanOrEqual(fit.widest + LABEL_INSET);
    });

    test(`${name}: a unit-less chart keeps its value axis wide enough`, async ({ page }) => {
      await page.setViewportSize(viewport);
      const mock = await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        stats_json: (n: number) => moving(n),
        graphs: { status: 200, body: {} },
      });
      await openConsole(page, "pagespeed_admin", "#/graphs");
      await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
      const fit = await valueAxisFit(page.getByTestId("curated-hit_rate"));
      expect(fit.plotLeft).toBeGreaterThanOrEqual(Math.ceil(fit.widest));
      expect(fit.plotLeft).toBeGreaterThanOrEqual(fit.widest + LABEL_INSET);
    });
  }

  test("phone: the default charts do not scroll the page sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    const mock = await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: (n: number) => moving(n) });
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect.poll(() => mock.calls("stats_json"), { timeout: 15_000 }).toBeGreaterThanOrEqual(2);
    await expect(page.getByTestId("curated-requests")).toBeVisible();
    expect(
      await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
    ).toBe(true);
  });
});
