// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Logs: the module's messages and the optimizer's log on one timeline,
// every state mocked end to end. Data states run on the whole-server
// console unless a case says otherwise; on the per-vhost console the
// optimizer's leaf answers 403 (allReplies()). Live-ring cases use
// F.logsDaemon(), which follows the optimizer's documented contract; their
// entries are Info lines, so those cases open the raw optimizer view (all
// levels, no grouping), which keeps the old Optimizer Logs page's behaviour:
// oldest to newest, follow the tail, jump to newest, inline markers, 3 s
// poll. Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import { EMPTY_MESSAGE_TEXT } from "../src/lib/utils/message-groups";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

/** The raw optimizer stream: the optimizer's entries only, every level, one row per entry, oldest first. */
const RAW = "#/logs?source=optimizer&level=debug&group=0";

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

const globalReplies = (logs: Responder | undefined, messages: Responder = F.MESSAGES_EMPTY) => ({
  ...F.allReplies(),
  config: F.CONFIG_GLOBAL,
  message_history: messages,
  "v1/daemon/logs": logs,
});

const noSideways = (page: Page) =>
  page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1);

test.describe("the timeline", () => {
  test("both logs on one timeline, newest first, each row naming its source", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    const rows = page.getByTestId("log-group");
    await expect(rows).toHaveCount(7);
    await expect(rows.first()).toHaveAttribute("data-source", "module");
    await expect(rows.first()).toContainText("Fetch of https://www.example.test/a.css failed");
    await expect(rows.last()).toContainText("parsed 42 directives");
    await expect(rows.last().locator(".source-badge")).toHaveText("Optimizer");
    // The source choice is written into the address.
    await page.getByRole("radio", { name: "Optimizer" }).click();
    await expect(page).toHaveURL(/#\/logs\?level=debug&source=optimizer$/);
    await expect(page.getByRole("radio", { name: "Optimizer" })).toBeChecked();
    await expect(rows).toHaveCount(4);
    await page.getByRole("radio", { name: "Module" }).click();
    await expect(page).toHaveURL(/#\/logs\?level=debug&source=module$/);
    await expect(rows).toHaveCount(3);
  });

  test("one poll reads both logs, each from its own cursor", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream(), F.MESSAGES));
    const first = page.waitForRequest((r) => r.url().includes("/v1/daemon/logs"));
    // Both sources, one row per entry (the raw optimizer view reads only the optimizer).
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug&group=0");
    const firstUrl = new URL((await first).url());
    expect(firstUrl.searchParams.get("since")).toBeNull();
    expect(firstUrl.searchParams.get("limit")).toBe("500");
    await ready(page);
    await expect(page.getByText("stream entry 0", { exact: true })).toBeVisible();
    const later = await page.waitForRequest((r) => r.url().includes("/v1/daemon/logs?since="), { timeout: 10000 });
    expect(new URL(later.url()).searchParams.get("since")).toBe("2");
    await expect(page.getByText("stream entry 3", { exact: true })).toBeVisible();
    expect(mock.calls("message_history")).toBeGreaterThanOrEqual(2);
  });

  test("an old Messages link keeps its level and shows the module's messages", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/messages?level=error");
    await expect(page).toHaveURL(/#\/logs\?level=error&source=module$/);
    await ready(page);
    await expect(page.getByRole("radio", { name: "Module" })).toBeChecked();
    await expect(page.getByRole("checkbox", { name: /^error/ })).toBeChecked();
    await expect(page.getByRole("checkbox", { name: /^warning/ })).not.toBeChecked();
    await expect(page.getByTestId("logs-filter-note")).toContainText("error");
    await expect(page.getByText("Fetch of https://www.example.test/a.css failed")).toBeVisible();
    await expect(page.getByText("analysis timed out", { exact: false })).toHaveCount(0);
    await expect(page.locator(".nav-item.active")).toHaveText("Logs");
  });

  test("defaults to warnings and worse; Info is one tick away", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    await expect(page.getByRole("checkbox", { name: /^warning/ })).toBeChecked();
    await expect(page.getByRole("checkbox", { name: /^info/ })).not.toBeChecked();
    await expect(page.getByText("Fetch of https://www.example.test/a.css failed")).toBeVisible();
    await expect(page.getByText("CycloneCache enabled", { exact: false })).toHaveCount(0);
    await page.getByRole("checkbox", { name: /^info/ }).check();
    await expect(page.getByText("CycloneCache enabled", { exact: false })).toBeVisible();
  });

  test("repeats are one row with a count; the entries, with their dates, are a click away", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_REPEATS });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    const rows = page.getByTestId("log-group");
    await expect(rows).toHaveCount(2);
    await expect(rows.first().getByTestId("log-repeats")).toHaveText("×3");
    await expect(rows.first().locator(".log-time")).toHaveText("2026-10-02 10:25:51.000");
    await expect(rows.first().locator(".log-text")).toHaveText("No permission to rewrite 'https://umami.example.test/script.js'");
    const show = rows.first().getByRole("button", { name: "Show 3 entries" });
    await expect(show).toHaveAttribute("aria-expanded", "false");
    await show.click();
    const entries = rows.first().getByTestId("log-entries").locator("li");
    await expect(entries).toHaveCount(3);
    await expect(entries.first()).toContainText("2026-10-02 10:25:51");
    await expect(entries.nth(1)).toContainText("script.js?v=2");
    await expect(rows.first().getByRole("button", { name: "Hide entries" })).toHaveAttribute("aria-expanded", "true");
  });

  test("grouping is one tick away, and group=0 presets it off", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_REPEATS });
    await openConsole(page, "pagespeed_admin", "#/logs?level=info");
    await ready(page);
    await expect(page.getByTestId("log-group")).toHaveCount(3);
    // The toggle is written into the address.
    await page.getByRole("checkbox", { name: "Group repeats" }).click();
    await expect(page).toHaveURL(/#\/logs\?level=info&group=0$/);
    await expect(page.getByTestId("log-group")).toHaveCount(5);
    await openConsole(page, "pagespeed_admin", "#/logs?level=info&group=0");
    await ready(page);
    await expect(page.getByRole("checkbox", { name: "Group repeats" })).not.toBeChecked();
    await expect(page.getByTestId("log-group")).toHaveCount(5);
  });

  test("the level is shown once, not repeated inside the text", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_REPEATS });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    const text = page.getByTestId("log-group").first().locator(".log-text");
    await expect(text).not.toContainText("[Warning]");
    await expect(text).not.toContainText("[531]");
    await expect(text).not.toContainText("GMT]");
  });

  test("both sources share one level palette", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    await expect(page.getByTestId("log-group").filter({ hasText: "Fetch of" }).locator(".level-badge-error")).toBeVisible();
    await expect(page.getByTestId("log-group").filter({ hasText: "analysis timed out" }).locator(".level-badge-error")).toBeVisible();
    await expect(page.getByTestId("log-group").filter({ hasText: "parsed 42 directives" }).locator(".level-badge-debug")).toBeVisible();
  });

  test("the level deep link and the text filter narrow the optimizer's entries", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS));
    await openConsole(page, "pagespeed_global_admin", "#/logs?source=optimizer&level=error");
    await ready(page);
    await expect(page.getByTestId("logs-filter-note")).toContainText("error");
    await expect(page.getByText("analysis timed out", { exact: false })).toBeVisible();
    await expect(page.getByText("cache flush complete")).toHaveCount(0);
    await page.getByRole("checkbox", { name: /^info/ }).check();
    await expect(page.getByText("cache flush complete")).toBeVisible();
    await page.getByTestId("logs-search").fill("SLOW");
    await expect(page.getByText("cache flush complete")).toHaveCount(0);
    await page.getByRole("checkbox", { name: /^warning/ }).check();
    await expect(page.getByText("origin fetch slow")).toBeVisible();
  });

  test("filters that hide everything offer to show every level", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_INFO_ONLY });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    const empty = page.getByTestId("logs-none-match");
    await expect(empty).toContainText("No entries match the filters");
    await empty.getByRole("link", { name: "Show all levels" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=debug$/);
    await expect(page.getByText("CycloneCache enabled", { exact: false })).toBeVisible();
  });

  test("nothing logged anywhere says so", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_EMPTY));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("logs-empty")).toContainText("No log entries yet");
  });

  test("an entry without a message text is one empty row, and the rest still shows", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_MALFORMED });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    const rows = page.getByTestId("log-group");
    await expect(rows).toHaveCount(2);
    await expect(rows.first().locator(".log-text")).toHaveText("Fetch of https://www.example.test/b.css failed");
    await expect(rows.nth(1).locator(".log-text")).toHaveText(EMPTY_MESSAGE_TEXT);
    await expect(rows.nth(1).getByTestId("log-repeats")).toHaveText("×3");
    expect(errors).toEqual([]);
  });
});

test.describe("links and inert text", () => {
  test("URLs in either log are links built from the parsed URL", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    for (const href of ["https://www.example.test/a.css", "https://www.example.test/heavy.png"]) {
      const link = page.getByRole("link", { name: href });
      await expect(link).toHaveAttribute("href", href);
      await expect(link).toHaveAttribute("target", "_blank");
      await expect(link).toHaveAttribute("rel", /noopener/);
      await expect(link).toHaveAttribute("referrerpolicy", "no-referrer");
    }
  });

  test("markup and script URLs in a module message stay inert text", async ({ page }) => {
    const dialogs: string[] = [];
    page.on("dialog", (d) => {
      dialogs.push(d.message());
      void d.dismiss();
    });
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_ATTACK });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    const list = page.getByTestId("logs-timeline");
    await expect(list).toContainText("<img src=x onerror=alert(1)> javascript:alert(2) data:text/html,<b>x</b>");
    await expect(list.locator("img")).toHaveCount(0);
    await expect(list.locator("b")).toHaveCount(0);
    await expect(list.locator("a")).toHaveCount(1);
    await expect(list.locator("a")).toHaveAttribute("href", "https://ok.test/a");
    await expect(list.locator("a")).not.toHaveAttribute("onmouseover", /.*/);
    expect(dialogs).toEqual([]);
  });

  test("markup-shaped optimizer lines render as plain text", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_ATTACK));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    const list = page.getByTestId("logs-timeline");
    await expect(list).toContainText("<img src=x onerror=alert(1)>");
    await expect(list).toContainText("javascript:alert(2)");
    await expect(list).toContainText("<script>alert(3)</script>");
    await expect(page.locator("main img")).toHaveCount(0);
    await expect(page.locator("main script")).toHaveCount(0);
    await expect(page.locator('main a[href^="javascript:"]')).toHaveCount(0);
  });

  test("an embedded newline cannot forge a second row", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_ATTACK));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    const list = page.getByTestId("logs-timeline");
    await expect(list).toContainText("12:00:01.234 error worker cache purged");
    await expect(list).toContainText("⏎");
    await expect(list.getByTestId("log-group")).toHaveCount(2);
  });
});

test.describe("the optimizer's log as a source", () => {
  test("per-vhost console: the module's messages, and the whole-server console for the optimizer's log", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: F.MESSAGES });
    await openConsole(page, "pagespeed_admin", "#/logs?level=error");
    await ready(page);
    await expect(page.getByTestId("logs-per-vhost")).toBeVisible();
    // The subtitle follows the scope: it does not promise the optimizer's log the note below says is elsewhere.
    await expect(page.getByTestId("logs-scope-note")).toHaveText(
      "The module's messages (every virtual host of this server), newest first.",
    );
    await expect(page.getByRole("link", { name: "whole-server console" })).toHaveAttribute(
      "href",
      "/pagespeed_global_admin/#/logs?level=error",
    );
    await expect(page.getByTestId("logs-source-filter")).toHaveCount(0);
    await expect(page.getByText("Fetch of https://www.example.test/a.css failed")).toBeVisible();
    await expect(page.locator(".nav-item")).toHaveCount(11);
    await expect(page.getByRole("link", { name: "Logs", exact: true })).toBeVisible();
  });

  test("a per-vhost console stops asking the optimizer's log after its one whole_server_console_only answer", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: F.MESSAGES });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("logs-per-vhost")).toBeVisible();
    await page.waitForTimeout(11000);
    expect(mock.calls("v1/daemon/logs")).toBeLessThanOrEqual(1);
    expect(mock.calls("message_history")).toBeGreaterThanOrEqual(2);
  });

  test("a console that already knows it is per-vhost never asks the optimizer's log", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/overview");
    await expect.poll(() => mock.calls("config")).toBeGreaterThan(0);
    await page.evaluate(() => {
      location.hash = "#/logs";
    });
    await ready(page);
    await expect(page.getByTestId("logs-per-vhost")).toBeVisible();
    await page.waitForTimeout(7000);
    expect(mock.calls("v1/daemon/logs")).toBe(0);
  });

  test("a module without the logs leaf explains, and the module's messages still show", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(undefined, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("logs-no-support")).toContainText("This module version cannot show");
    await expect(page.getByText("Fetch of https://www.example.test/a.css failed")).toBeVisible();
  });

  test("an optimizer without the endpoint is told to update, with no version number", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.UNSUPPORTED));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    const state = page.getByTestId("logs-no-support");
    await expect(state).toContainText("This optimizer version does not provide logs. Update the optimizer package.");
    await expect(state).not.toContainText(/\d+\.\d+\.\d+/);
  });

  test("whole-server console: the subtitle names both sources", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream(), F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("logs-scope-note")).toHaveText(
      "The module's messages (every virtual host of this server) and the optimizer's own recent log, newest first.",
    );
  });

  test("an optimizer that answers 503 itself is not called unreachable", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies({ status: 503, body: { error: "busy" } }, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=info");
    await ready(page);
    const note = page.getByTestId("logs-unreachable");
    await expect(note).toHaveText(
      "The optimizer's log is not shown. The optimizer did not answer this refresh; the next refresh tries again.",
    );
    await expect(note).not.toContainText(/unreachable/i);
  });

  test("an unreachable optimizer is a note; the module's messages are unaffected", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.UNREACHABLE, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=info");
    await ready(page);
    await expect(page.getByTestId("logs-unreachable")).toHaveText(
      "The optimizer's log is not shown. The optimizer is configured but not answering. Its data appears here once it answers.",
    );
    await expect(page.getByText("CycloneCache enabled", { exact: false })).toBeVisible();
  });

  test("an answer over the proxy's cap is named as such, not as an unreachable optimizer", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_TOO_LARGE));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("logs-too-large")).toContainText("larger than");
    await expect(page.getByTestId("logs-unreachable")).toHaveCount(0);
  });

  test("dropped entries are counted in a note", async ({ page }) => {
    const wrap = F.logsDaemon({ initial: 2, ring: 50, perPoll: (poll) => (poll === 1 ? 60 : 0) });
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(wrap));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByText("stream entry 1", { exact: true })).toBeVisible();
    await expect(page.getByTestId("logs-gap")).toContainText("10 entries dropped", { timeout: 10000 });
    await expect(page.getByText("stream entry 12", { exact: true })).toBeVisible();
    await expect(page.getByText("stream entry 1", { exact: true })).toBeVisible();
  });

  test("an optimizer restart is noted, and the new stream continues", async ({ page }) => {
    const restarting = F.logsDaemon({ initial: 3, restartAtCall: 1, afterRestart: 2 });
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(restarting));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByText("stream entry 2", { exact: true })).toBeVisible();
    await expect(page.getByTestId("logs-restart")).toBeVisible({ timeout: 10000 });
    await expect(page.getByText("after restart entry 0", { exact: true })).toBeVisible();
    await expect(page.getByText("stream entry 2", { exact: true })).toBeVisible();
  });

  test("keeps up with an optimizer that logs faster than one page per poll", async ({ page }) => {
    const busy = F.logsDaemon({ initial: 3, perPoll: (poll) => (poll === 1 ? 1200 : 0) });
    const mock = await mockAdmin(page, "pagespeed_global_admin", globalReplies(busy));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByText("stream entry 2", { exact: true })).toBeVisible();
    await expect(page.getByText("stream entry 1202", { exact: true })).toBeVisible({ timeout: 12000 });
    expect(mock.calls("v1/daemon/logs")).toBeGreaterThanOrEqual(4);
    await expect(page.getByTestId("logs-gap")).toHaveCount(0);
  });

  test("the pause button stops polling; resume continues from the cursor", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream()));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByText("stream entry 0", { exact: true })).toBeVisible();
    await page.getByRole("button", { name: "Auto-refresh · 3 s" }).click();
    const calls = mock.calls("v1/daemon/logs");
    await page.waitForTimeout(3500); // longer than the raw view's 3 s poll
    expect(mock.calls("v1/daemon/logs")).toBe(calls);
    await page.getByRole("button", { name: "Auto-refresh · 3 s" }).click();
    await expect.poll(() => mock.calls("v1/daemon/logs"), { timeout: 5000 }).toBeGreaterThan(calls);
    await expect(page.getByText("stream entry 1", { exact: true })).toHaveCount(1);
  });

  test("a busy (429) optimizer read keeps the current view", async ({ page }) => {
    let busy = false;
    const stream = F.logsStream();
    await mockAdmin(page, "pagespeed_global_admin", {
      ...globalReplies(undefined),
      "v1/daemon/logs": (call, request) => (busy ? F.BUSY : stream(call, request)),
    });
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByText("stream entry 0", { exact: true })).toBeVisible();
    busy = true;
    await Promise.all([
      page.waitForResponse((r) => r.url().includes("/v1/daemon/logs") && r.status() === 429),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
    await page.waitForTimeout(150);
    await expect(page.getByText("stream entry 0", { exact: true })).toBeVisible();
    await expect(page.locator("main [role='alert']")).toHaveCount(0);
    await expect(page.getByText("Last refresh failed")).toHaveCount(0);
  });

  test("the overview's optimizer-errors alert links to the optimizer's error entries", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      "v1/daemon/stats": (call) => (call === 0 ? F.daemonStats({ errorsTotal: 0 }) : F.daemonStats({ errorsTotal: 3 })),
    });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    await expect.poll(() => mock.calls("v1/daemon/stats"), { timeout: 15000 }).toBeGreaterThanOrEqual(2);
    const alert = page.getByTestId("alert-error-rate");
    await expect(alert).toBeVisible();
    await expect(alert.getByRole("link", { name: "Details" })).toHaveAttribute("href", "#/logs?source=optimizer&level=error");
  });
});

test.describe("the raw optimizer stream (source=optimizer, group=0)", () => {
  test("reads only the optimizer's log, lists it oldest to newest, and polls every 3 s", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream(), F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    const rows = page.getByTestId("log-group");
    await expect(rows.first()).toContainText("stream entry 0");
    await expect(rows.last()).toContainText("stream entry 2");
    await expect(page.getByRole("button", { name: "Auto-refresh · 3 s" })).toBeVisible();
    // The next poll appends at the bottom.
    await expect(rows.last()).toContainText("stream entry 3", { timeout: 6000 });
    await expect(rows.first()).toContainText("stream entry 0");
    expect(mock.calls("message_history")).toBe(0);
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
  });

  for (const viewport of [
    { name: "desktop", width: 1280, height: 800 },
    { name: "phone", width: 390, height: 844 },
  ]) {
    test(`${viewport.name}: scrolling up breaks follow; jump to newest re-pins; the stream takes keyboard focus`, async ({ page }) => {
      await page.setViewportSize({ width: viewport.width, height: viewport.height });
      await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream(80)));
      await openConsole(page, "pagespeed_global_admin", RAW);
      await ready(page);
      await expect(page.getByText("stream entry 0", { exact: true })).toBeVisible();
      const stream = page.getByTestId("logs-timeline");
      await expect(stream).toHaveAttribute("tabindex", "0");
      await expect(stream).toHaveAttribute("aria-label", "Optimizer log entries");
      await stream.focus();
      await expect(stream).toBeFocused();
      const pinned = () => stream.evaluate((el) => el.scrollHeight - el.scrollTop - el.clientHeight < 40);
      await expect.poll(pinned).toBe(true); // follow pins to the newest entry
      await stream.evaluate((el) => {
        el.scrollTop = 0;
        el.dispatchEvent(new Event("scroll"));
      });
      await expect(page.getByTestId("logs-jump")).toBeVisible();
      // New entries keep arriving while unfollowed, and the button counts them.
      await expect(page.getByTestId("logs-jump")).toContainText("(1 new)", { timeout: 10000 });
      expect(await pinned()).toBe(false);
      await page.getByTestId("logs-jump").click();
      await expect(page.getByTestId("logs-jump")).toHaveCount(0);
      await expect.poll(pinned).toBe(true);
      expect(
        await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
      ).toBe(true);
    });
  }

  test("outside the raw view the optimizer's restarts are a counted note, newest first, with nothing to follow", async ({ page }) => {
    const restarting = F.logsDaemon({ initial: 3, restartAtCall: 1, afterRestart: 2 });
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(restarting));
    await openConsole(page, "pagespeed_global_admin", "#/logs?source=optimizer&level=debug");
    await ready(page);
    await expect(page.getByTestId("logs-restart")).toContainText("restarted once", { timeout: 12000 });
    await expect(page.getByTestId("logs-timeline").getByTestId("logs-restart")).toHaveCount(0);
    await expect(page.getByTestId("logs-jump")).toHaveCount(0);
    await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
  });

  test("\"Show all\" in the raw view keeps the raw view and only changes the level", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream()));
    await openConsole(page, "pagespeed_global_admin", "#/logs?source=optimizer&level=error&group=0");
    await ready(page);
    await page.getByTestId("logs-filter-note").getByRole("link", { name: "Show all" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=optimizer&level=debug&group=0$/);
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
    await expect(page.getByTestId("log-group").first()).toContainText("stream entry 0");
  });

  test("an unreachable optimizer: the raw view says so, and only so", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.UNREACHABLE));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByTestId("logs-unreachable")).toContainText("configured but not answering");
    await expect(page.getByText("Nothing has been logged")).toHaveCount(0);
  });

  test("turning grouping on leaves the raw view for the newest-first timeline", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream()));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
    await page.getByRole("checkbox", { name: "Group repeats" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=optimizer&level=debug$/);
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "region");
    await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
  });
});

test.describe("the view controls change the view in place", () => {
  /** Marks the mounted page, so a later check can tell whether it was mounted afresh. */
  const markPage = (page: Page) => page.evaluate(() => document.querySelector("main h1")!.setAttribute("data-mark", "1"));
  const pageMark = (page: Page) => page.locator("main h1[data-mark='1']");
  const fromStart = (requests: string[]) => requests.filter((url) => !new URL(url).searchParams.has("since")).length;
  const moduleReads = (page: Page) => {
    const urls: string[] = [];
    page.on("request", (r) => {
      if (new URL(r.url()).pathname.endsWith("/message_history")) urls.push(r.url());
    });
    return urls;
  };

  test("filter text, levels, an expanded row and the focus survive a Source change and a Group toggle", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES_REPEATS));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await markPage(page);
    await page.getByRole("checkbox", { name: /^info/ }).check();
    await page.getByTestId("logs-search").fill("umami");
    const row = page.getByTestId("log-group").first();
    await row.getByRole("button", { name: "Show 3 entries" }).click();
    await expect(row.getByTestId("log-entries").locator("li")).toHaveCount(3);

    const moduleRadio = page.getByRole("radio", { name: "Module" });
    await moduleRadio.check();
    await expect(page).toHaveURL(/#\/logs\?source=module$/);
    await expect(moduleRadio).toBeFocused();
    await expect(pageMark(page)).toHaveCount(1);
    await expect(page.getByTestId("logs-search")).toHaveValue("umami");
    await expect(page.getByRole("checkbox", { name: /^info/ })).toBeChecked();
    await expect(row.getByTestId("log-entries").locator("li")).toHaveCount(3);

    const group = page.getByRole("checkbox", { name: "Group repeats" });
    await group.uncheck();
    await expect(page).toHaveURL(/#\/logs\?source=module&group=0$/);
    await expect(group).toBeFocused();
    await expect(page.getByTestId("log-entries")).toHaveCount(0);
    await group.check();
    await expect(page).toHaveURL(/#\/logs\?source=module$/);
    await expect(group).toBeFocused();
    await expect(pageMark(page)).toHaveCount(1);
    await expect(page.getByTestId("logs-search")).toHaveValue("umami");
    await expect(page.getByRole("checkbox", { name: /^info/ })).toBeChecked();
    await expect(row.getByTestId("log-entries").locator("li")).toHaveCount(3);
  });

  test("the arrow keys step through the sources", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    await page.getByRole("radio", { name: "Both" }).focus();
    await page.keyboard.press("ArrowDown");
    await expect(page.getByRole("radio", { name: "Module" })).toBeChecked();
    await expect(page.getByRole("radio", { name: "Module" })).toBeFocused();
    await expect(page.getByTestId("log-group")).toHaveCount(3);
    await page.keyboard.press("ArrowDown");
    await expect(page.getByRole("radio", { name: "Optimizer" })).toBeChecked();
    await expect(page.getByRole("radio", { name: "Optimizer" })).toBeFocused();
    await expect(page.getByTestId("log-group")).toHaveCount(4);
    await expect(page).toHaveURL(/#\/logs\?level=debug&source=optimizer$/);
  });

  test("the arrow keys step into and out of the raw stream with the focus on the source", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug&group=0");
    await ready(page);
    await markPage(page);
    const timeline = page.getByTestId("logs-timeline");
    await expect(timeline).toHaveAttribute("role", "region");
    await page.getByRole("radio", { name: "Both" }).focus();
    await page.keyboard.press("ArrowDown");
    await page.keyboard.press("ArrowDown");
    const optimizer = page.getByRole("radio", { name: "Optimizer" });
    await expect(optimizer).toBeChecked();
    await expect(optimizer).toBeFocused();
    await expect(timeline).toHaveAttribute("role", "log");
    await expect(page.getByRole("button", { name: "Auto-refresh · 3 s" })).toBeVisible();
    await expect(page).toHaveURL(/#\/logs\?level=debug&group=0&source=optimizer$/);
    // Out again, in both directions.
    await page.keyboard.press("ArrowDown");
    await expect(page.getByRole("radio", { name: "Both" })).toBeFocused();
    await expect(timeline).toHaveAttribute("role", "region");
    await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
    await page.keyboard.press("ArrowUp");
    await expect(optimizer).toBeFocused();
    await expect(timeline).toHaveAttribute("role", "log");
    await page.keyboard.press("ArrowUp");
    await expect(page.getByRole("radio", { name: "Module" })).toBeFocused();
    await expect(timeline).toHaveAttribute("role", "region");
    await expect(page).toHaveURL(/#\/logs\?level=debug&group=0&source=module$/);
    await expect(pageMark(page)).toHaveCount(1);
  });

  test("the scroll position survives a Source change and a Group toggle", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 320 });
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    await expect(page.getByTestId("log-group")).toHaveCount(7);
    const scrollY = () => page.evaluate(() => window.scrollY);
    // Scrolled as far as the page still allows with the shortest view (the module's three rows).
    await page.getByRole("radio", { name: "Module" }).check();
    const y = await page.evaluate(() => {
      const top = Math.min(60, document.scrollingElement!.scrollHeight - window.innerHeight);
      window.scrollTo(0, top);
      return window.scrollY;
    });
    expect(y).toBeGreaterThan(0);
    await page.getByRole("radio", { name: "Both" }).check();
    await expect(page.getByTestId("log-group")).toHaveCount(7);
    expect(await scrollY()).toBe(y);
    await page.getByRole("radio", { name: "Optimizer" }).check();
    await expect(page.getByTestId("log-group")).toHaveCount(4);
    expect(await scrollY()).toBe(y);
    await page.getByRole("checkbox", { name: "Group repeats" }).uncheck();
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
    expect(await scrollY()).toBe(y);
    await page.getByRole("checkbox", { name: "Group repeats" }).check();
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "region");
    expect(await scrollY()).toBe(y);
  });

  test("changing the view outside the raw stream never reads the module's log from the start again", async ({ page }) => {
    const reads = moduleReads(page);
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    await expect(page.getByTestId("log-group")).toHaveCount(7);
    await page.getByRole("radio", { name: "Module" }).check();
    await expect(page.getByTestId("log-group")).toHaveCount(3);
    await page.getByRole("radio", { name: "Optimizer" }).check();
    await expect(page.getByTestId("log-group")).toHaveCount(4);
    await page.getByRole("radio", { name: "Both" }).check();
    await page.getByRole("checkbox", { name: "Group repeats" }).uncheck();
    await expect(page).toHaveURL(/#\/logs\?level=debug&group=0$/);
    await expect(page.getByTestId("log-group")).toHaveCount(7);
    // The poll that follows continues from the cursor.
    await page.waitForRequest((r) => r.url().includes("/message_history?since="), { timeout: 10000 });
    expect(fromStart(reads)).toBe(1);
  });

  test("entering the raw stream in place polls the optimizer alone every 3 s; leaving it resumes the module's log from its cursor", async ({ page }) => {
    const reads = moduleReads(page);
    const mock = await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.logsStream(), F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?level=debug");
    await ready(page);
    await markPage(page);
    await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
    await page.getByRole("radio", { name: "Optimizer" }).check();
    const group = page.getByRole("checkbox", { name: "Group repeats" });
    await group.uncheck();
    await expect(page).toHaveURL(/#\/logs\?level=debug&source=optimizer&group=0$/);
    await expect(group).toBeFocused();
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
    await expect(page.getByRole("button", { name: "Auto-refresh · 3 s" })).toBeVisible();
    const rows = page.getByTestId("log-group");
    await expect(rows.first()).toContainText("stream entry 0");
    // Two optimizer reads in the raw stream, and the module's log is left alone meanwhile.
    const moduleCalls = mock.calls("message_history");
    const optimizerCalls = mock.calls("v1/daemon/logs");
    await expect.poll(() => mock.calls("v1/daemon/logs"), { timeout: 10000 }).toBeGreaterThanOrEqual(optimizerCalls + 2);
    expect(mock.calls("message_history")).toBeLessThanOrEqual(moduleCalls + 1); // at most a read already in flight
    const settled = mock.calls("message_history");

    await group.check();
    await expect(page).toHaveURL(/#\/logs\?level=debug&source=optimizer$/);
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "region");
    await expect(page.getByRole("button", { name: "Auto-refresh · 5 s" })).toBeVisible();
    // The module's log is read at once, from where it was left.
    await expect.poll(() => mock.calls("message_history"), { timeout: 2000 }).toBeGreaterThan(settled);
    expect(fromStart(reads)).toBe(1);
    await expect(pageMark(page)).toHaveCount(1);
    await page.getByRole("radio", { name: "Module" }).check();
    await expect(rows).toHaveCount(3);
  });

  test("a link to the raw stream presets every level; entering it with the controls keeps the levels as set", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?source=optimizer&group=0");
    await ready(page);
    await expect(page.getByRole("checkbox", { name: /^debug/ })).toBeChecked();
    await expect(page.getByTestId("log-group")).toHaveCount(4);
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await page.getByRole("radio", { name: "Optimizer" }).check();
    await page.getByRole("checkbox", { name: "Group repeats" }).uncheck();
    await expect(page.getByTestId("logs-timeline")).toHaveAttribute("role", "log");
    await expect(page.getByRole("checkbox", { name: /^warning/ })).toBeChecked();
    await expect(page.getByRole("checkbox", { name: /^debug/ })).not.toBeChecked();
    await expect(page.getByTestId("log-group")).toHaveCount(2);
  });

  test("after Source is set to Module, the Logs shortcut opens the page afresh on both", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs");
    await ready(page);
    await markPage(page);
    await page.getByTestId("logs-search").fill("origin");
    await page.getByRole("radio", { name: "Module" }).check();
    await expect(page).toHaveURL(/#\/logs\?source=module$/);
    await page.locator("main h1").click(); // out of the form controls: shortcuts are off inside them
    await page.keyboard.press("g");
    await page.keyboard.press("l");
    await expect(page).toHaveURL(/#\/logs$/);
    await ready(page);
    await expect(page.getByRole("radio", { name: "Both" })).toBeChecked();
    await expect(pageMark(page)).toHaveCount(0);
    await expect(page.getByTestId("logs-search")).toHaveValue("");
    await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeFocused();
  });

  test("a link followed a second time opens its view again, whatever the controls changed since", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?source=module");
    await ready(page);
    await expect(page.getByRole("radio", { name: "Module" })).toBeChecked();
    await markPage(page);
    await page.getByRole("radio", { name: "Both" }).check();
    await expect(page).toHaveURL(/#\/logs$/);
    await expect(pageMark(page)).toHaveCount(1);
    // The same address again, as a finding's link or the "g m" shortcut gives it.
    await page.locator("main h1").click();
    await page.keyboard.press("g");
    await page.keyboard.press("m");
    await expect(page).toHaveURL(/#\/logs\?source=module$/);
    await ready(page);
    await expect(page.getByRole("radio", { name: "Module" })).toBeChecked();
    await expect(pageMark(page)).toHaveCount(0);
    await expect(page.getByTestId("log-group")).toHaveCount(2);
  });

  test("the host lens stays in the address through a Source change and a Group toggle", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_LEVELS, F.MESSAGES));
    await openConsole(page, "pagespeed_global_admin", "#/logs?lens=www.example.test");
    await ready(page);
    await page.getByRole("radio", { name: "Module" }).check();
    await expect(page).toHaveURL(/#\/logs\?lens=www\.example\.test&source=module$/);
    await page.getByRole("checkbox", { name: "Group repeats" }).uncheck();
    await expect(page).toHaveURL(/#\/logs\?lens=www\.example\.test&source=module&group=0$/);
    await page.getByRole("radio", { name: "Both" }).check();
    await expect(page).toHaveURL(/#\/logs\?lens=www\.example\.test&group=0$/);
    await expect(page.getByTestId("logs-lens")).toContainText("www.example.test");
  });

  test("a per-host console's Group toggle keeps the rest of its address", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_REPEATS });
    await openConsole(page, "pagespeed_admin", "#/logs?source=module&level=info");
    await ready(page);
    const group = page.getByRole("checkbox", { name: "Group repeats" });
    await group.uncheck();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=info&group=0$/);
    await expect(group).toBeFocused();
    await expect(page.getByTestId("log-group")).toHaveCount(5);
  });
});

test.describe("phone", () => {
  test("a long URL wraps instead of scrolling the page sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, message_history: F.MESSAGES_LONG_URL });
    await openConsole(page, "pagespeed_admin", "#/logs");
    await ready(page);
    await expect(page.getByTestId("log-group")).toHaveCount(1);
    expect(await noSideways(page)).toBe(true);
  });

  test("optimizer times carry the date, and the time column does not scroll the page sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.LOGS_PAGE));
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    await expect(page.getByTestId("log-group").filter({ hasText: "cache flush complete" }).locator(".log-time")).toHaveText(
      "2025-09-30 11:00:00.000",
    );
    expect(await noSideways(page)).toBe(true);
  });

  test("a long optimizer line wraps and can be read to its end", async ({ page }) => {
    const long = `Processing notification for https://www.example.test/${"a".repeat(4040)}…[truncated 4096 bytes]`;
    await mockAdmin(
      page,
      "pagespeed_global_admin",
      globalReplies(
        F.ok({
          entries: [{ level: "info", message: long, module: "worker", seq: 0, source: "worker", timestamp: 1759230000000, type: "log" }],
          gap: false,
          more: false,
          newest_seq: 0,
          next_since: 0,
          oldest_seq: 0,
          shed_total: 0,
          stream_id: "9f2c4e1a7b3d5c80",
        }),
      ),
    );
    await openConsole(page, "pagespeed_global_admin", RAW);
    await ready(page);
    const text = page.getByTestId("logs-timeline").locator(".log-text").first();
    await expect(text).toContainText("[truncated 4096 bytes]");
    expect(await text.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    expect(await page.getByTestId("logs-timeline").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  });
});
