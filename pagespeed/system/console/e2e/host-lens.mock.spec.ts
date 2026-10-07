// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The host lens on the whole-server console: which hosts it offers, where
// it applies, how it persists, and that a host is only ever text.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { ORIGIN, mockAdmin, openConsole, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const ALL_URLS = [
  { url: "/", hostname: "www.example.test", scheme: "https", alternate_count: 3 },
  { url: "/app.js", hostname: "cdn.example.test", scheme: "https", alternate_count: 1 },
  { url: "/x.css", hostname: "example.test", scheme: "https", alternate_count: 1 },
];

/** The optimizer's index, filtered by hostname= the way the optimizer filters it (exactly). */
const urlsByHost: Responder = (_call, request) => {
  const host = request.url.searchParams.get("hostname");
  const urls = host === null ? ALL_URLS : ALL_URLS.filter((u) => u.hostname === host);
  return F.ok({ urls, offset: 0, limit: 50, next_offset: urls.length, has_more: false, total: urls.length });
};

const MESSAGES_HOSTS = F.ok({
  scope: "process",
  next: 3,
  messages: [
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:41 GMT] [Warning] [531] Fetch of https://www.example.test/a.css failed" },
    { severity: "warning", message: "[Fri, 02 Oct 2026 10:25:42 GMT] [Warning] [531] Fetch of https://example.test/b.css failed" },
    { severity: "warning", message: '[Fri, 02 Oct 2026 10:25:43 GMT] [Warning] [531] Rejected https://evil.test"><img src=x onerror=alert(1)>/' },
  ],
});

const globalReplies = () => ({
  ...F.allReplies(),
  config: F.CONFIG_GLOBAL,
  "v1/daemon/cache/urls": urlsByHost,
  message_history: MESSAGES_HOSTS,
});

const lensSelect = (page: Page) => page.getByTestId("host-lens").getByRole("combobox", { name: "Host" });

async function urlsReady(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "URLs" })).toBeVisible();
  await expect(page.getByTestId("urls-table")).toBeVisible();
}

test("the lens lists the hosts the console has seen and narrows the URL index on the optimizer", async ({ page }) => {
  const hostParams: string[] = [];
  page.on("request", (r) => {
    const u = new URL(r.url());
    if (u.pathname.endsWith("/v1/daemon/cache/urls")) hostParams.push(u.searchParams.get("hostname") ?? "");
  });
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await expect(lensSelect(page).locator("option")).toHaveText(["All hosts", "cdn.example.test", "example.test", "www.example.test"]);
  await lensSelect(page).selectOption("example.test");
  await expect(page).toHaveURL(/#\/urls\?lens=example\.test$/);
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  await expect(page.getByText("https://example.test/x.css")).toBeVisible();
  await expect(page.getByText("https://www.example.test/", { exact: true })).toHaveCount(0);
  await expect(page.getByTestId("urls-lens")).toContainText("example.test");
  expect(hostParams).toContain("example.test");
  await expect(page.getByTestId("lens-note")).toHaveCount(0);
});

test("under a lens the URL table shows that host's rows only, before the new answer and from any answer", async ({ page }) => {
  // An index that ignores hostname= and answers slowly: the all-hosts page
  // is still the latest answer when the lens changes.
  const ignoresHost: Responder = () =>
    F.ok({ urls: ALL_URLS, offset: 0, limit: 50, next_offset: ALL_URLS.length, has_more: false, total: ALL_URLS.length });
  await mockAdmin(page, "pagespeed_global_admin", { ...globalReplies(), "v1/daemon/cache/urls": ignoresHost }, { delayMs: { "v1/daemon/cache/urls": 1500 } });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(3);
  await lensSelect(page).selectOption("www.example.test");
  await expect(page.getByTestId("urls-lens")).toContainText("www.example.test");
  // At once, under the new note: no other host's row.
  await expect(page.getByText("https://example.test/x.css")).toHaveCount(0, { timeout: 500 });
  await expect(page.getByText("https://cdn.example.test/app.js")).toHaveCount(0, { timeout: 500 });
  // And once the (host-ignoring) answer arrives, still only that host's.
  await expect.poll(() => page.getByTestId("urls-table").locator("tbody tr").count(), { timeout: 5_000 }).toBe(1);
  await page.waitForTimeout(2_000);
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  await expect(page.getByText("https://www.example.test/", { exact: true })).toBeVisible();
  await expect(page.getByText("https://cdn.example.test/app.js")).toHaveCount(0);
});

test("a shared link selects the lens, and the sidebar's links keep it", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=cdn.example.test");
  await urlsReady(page);
  await expect(lensSelect(page)).toHaveValue("cdn.example.test");
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  await expect(page.getByRole("link", { name: "Statistics", exact: true })).toHaveAttribute("href", "#/statistics?lens=cdn.example.test");
});

test("the lens is remembered across a reload; a page without per-host data says the lens does not apply", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await lensSelect(page).selectOption("example.test");
  await page.goto(`${ORIGIN}/pagespeed_global_admin/#/statistics`);
  await page.reload();
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await expect(lensSelect(page)).toHaveValue("example.test");
  await expect(page.getByTestId("lens-note")).toContainText("example.test");
  await expect(page.getByTestId("lens-note")).toContainText("whole server");
  await lensSelect(page).selectOption("");
  await expect(page.getByTestId("lens-note")).toHaveCount(0);
  await expect(page).toHaveURL(/#\/statistics$/);
});

test("a whole-server console on a renamed admin path remembers the lens too", async ({ page }) => {
  // The path does not say whole-server; the configuration's answer does.
  await mockAdmin(page, "renamed_admin", globalReplies());
  await openConsole(page, "renamed_admin", "#/urls");
  await urlsReady(page);
  await lensSelect(page).selectOption("example.test");
  await page.goto(`${ORIGIN}/renamed_admin/#/statistics`);
  await page.reload();
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await expect(lensSelect(page)).toHaveValue("example.test");
  await expect(page.getByTestId("lens-note")).toContainText("example.test");
});

test("a store that refuses: the lens still works for the visit", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.addInitScript(() => {
    Object.defineProperty(window, "localStorage", {
      configurable: true,
      get() {
        throw new Error("blocked");
      },
    });
  });
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await lensSelect(page).selectOption("example.test");
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  expect(errors).toEqual([]);
});

test("a malformed lens parameter is ignored", async ({ page }) => {
  const hostParams: string[] = [];
  page.on("request", (r) => {
    const u = new URL(r.url());
    if (u.pathname.endsWith("/v1/daemon/cache/urls")) hostParams.push(u.searchParams.get("hostname") ?? "");
  });
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=%3Cimg%20src%3Dx%3E");
  await urlsReady(page);
  await expect(lensSelect(page)).toHaveValue("");
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(3);
  expect(hostParams.every((h) => h === "")).toBe(true);
});

test("the timeline narrows to entries that name the host exactly", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/logs?lens=example.test");
  await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  await expect(page.getByTestId("log-group")).toHaveCount(1);
  await expect(page.getByTestId("log-group")).toContainText("https://example.test/b.css");
  await expect(page.getByTestId("logs-lens")).toContainText("example.test");
  await expect(page.getByTestId("lens-note")).toHaveCount(0);
});

test("a host from the log is text in the selector, never markup", async ({ page }) => {
  const dialogs: string[] = [];
  page.on("dialog", (d) => {
    dialogs.push(d.message());
    void d.dismiss();
  });
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/logs");
  await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  await expect(lensSelect(page).locator("option", { hasText: "evil.test" })).toHaveCount(1);
  await expect(page.locator("header img")).toHaveCount(0);
  for (const text of await lensSelect(page).locator("option").allTextContents()) {
    expect(text).toMatch(/^(All hosts|[a-z0-9.:[\]-]+)$/);
  }
  expect(dialogs).toEqual([]);
});

test("a per-vhost console shows no lens and never applies one", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: MESSAGES_HOSTS });
  await openConsole(page, "pagespeed_admin", "#/logs?lens=example.test");
  await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  await expect(page.getByTestId("host-lens")).toHaveCount(0);
  await expect(page.getByTestId("logs-lens")).toHaveCount(0);
  await expect(page.getByTestId("lens-note")).toHaveCount(0);
  // Unfiltered: the two "Fetch of URL failed" lines share a row, the rejection is the other.
  await expect(page.getByTestId("log-group")).toHaveCount(2);
});

test("a per-vhost link with lens= leaves the whole-server console's remembered lens alone", async ({ page, context }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await lensSelect(page).selectOption("example.test");
  // A per-host console in another tab of the same browser opens a link with its own lens=.
  const vhost = await context.newPage();
  await mockAdmin(vhost, "pagespeed_admin", { ...F.allReplies(), message_history: MESSAGES_HOSTS });
  await openConsole(vhost, "pagespeed_admin", "#/logs?lens=other.test");
  await expect(vhost.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  await vhost.close();
  // The whole-server console, opened without lens=, still has its own.
  await openConsole(page, "pagespeed_global_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await expect(lensSelect(page)).toHaveValue("example.test");
  const keys = await page.evaluate(() => Object.keys(localStorage).filter((k) => k.startsWith("pagespeed.console.lens")));
  expect(keys).toHaveLength(1);
});

test("phone: the lens sits in the drawer, and nothing scrolls sideways", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await urlsReady(page);
  await expect(page.getByTestId("host-lens")).toBeHidden();
  await page.getByRole("button", { name: "Toggle menu" }).click();
  const drawer = page.getByTestId("host-lens-drawer").getByRole("combobox", { name: "Host" });
  await drawer.selectOption("www.example.test");
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  expect(
    await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
  ).toBe(true);
});

// ── The optimizer's host names, through the address ─────────────────

const grammarReplies = () => ({
  ...F.allReplies(),
  config: F.CONFIG_GLOBAL,
  "v1/daemon/stats": F.OPT_STATS_HOSTS_GRAMMAR,
});

test("a site configured by IPv6 address can be chosen through the address", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", grammarReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings?lens=%5B2001%3Adb8%3A%3A1%5D");
  await expect(lensSelect(page)).toHaveValue("[2001:db8::1]");
  const figures = page.getByTestId("savings-lens-host");
  await expect(figures).toContainText("[2001:db8::1]");
  await expect(figures).toContainText("75%");
  await expect(figures).toContainText("4 responses");
});

test("a lens= is read the way the optimizer names a host", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", grammarReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings?lens=WWW.Example.COM.%3A443");
  await expect(lensSelect(page)).toHaveValue("www.example.com");
  await expect(page.getByTestId("savings-lens-host")).toContainText("6 responses");
});

test("a lens= the optimizer would refuse is ignored", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", grammarReplies());
  for (const value of ["%5B%3A%3A1%5Dx", "%5Bzz%3A%3A1%5D", "%5B%3A%3A1", "a%5Bb%5D.test", "-"]) {
    await openConsole(page, "pagespeed_global_admin", `#/savings?lens=${value}`);
    await expect(page.getByTestId("savings-by-host"), value).toBeVisible();
    await expect(lensSelect(page), value).toHaveValue("");
    await expect(page.getByTestId("savings-lens-host"), value).toHaveCount(0);
  }
});

test("every row the optimizer reports is listed as text, none folded into other", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", grammarReplies());
  await openConsole(page, "pagespeed_global_admin", "#/savings");
  const rows = page.getByTestId("savings-by-host").locator("tbody tr");
  await expect(rows).toHaveCount(5);
  await expect(rows.nth(1)).toContainText("[2001:db8::1]");
  await expect(rows.nth(2)).toContainText("a_b.test");
  await expect(rows.nth(3)).toContainText("10.1.2.3");
  await expect(page.getByTestId("savings-by-host-other").locator("td").nth(1)).toHaveText("1");
  await expect(lensSelect(page).locator("option")).toHaveText(["All hosts", "10.1.2.3", "[2001:db8::1]", "a_b.test", "www.example.com"]);
});

// ── The address carries the active lens ─────────────────────────────

/** Counts the hashchange events the page sees (an in-place rewrite fires none). */
async function countHashChanges(page: Page): Promise<void> {
  await page.addInitScript(() => {
    const w = window as unknown as { __hashchanges: number };
    w.__hashchanges = 0;
    window.addEventListener("hashchange", () => {
      w.__hashchanges += 1;
    });
  });
}
const hashChanges = (page: Page) => page.evaluate(() => (window as unknown as { __hashchanges: number }).__hashchanges);
const historyLength = (page: Page) => page.evaluate(() => history.length);
/** A typed address: the hash changes the way the address bar changes it. */
const typeAddress = (page: Page, hash: string) =>
  page.evaluate((h) => {
    window.location.hash = h;
  }, hash);

test("an address typed without lens= while the lens is on gains it in place", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=example.test");
  await urlsReady(page);
  const before = await historyLength(page);
  await typeAddress(page, "#/statistics");
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  expect(await historyLength(page)).toBe(before + 1);
  await expect(lensSelect(page)).toHaveValue("example.test");
});

test("a link without lens= followed while the lens is on gains it too", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", { ...globalReplies(), "v1/daemon/stats": F.OPT_STATS_PAGE });
  await openConsole(page, "pagespeed_global_admin", "#/savings?lens=example.test");
  await page.getByRole("link", { name: "All statistics" }).click();
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
});

test("Back and Forward move through the entries without adding or looping", async ({ page }) => {
  await countHashChanges(page);
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=example.test");
  await urlsReady(page);
  await typeAddress(page, "#/statistics");
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await typeAddress(page, "#/histograms");
  await expect(page).toHaveURL(/#\/histograms\?lens=example\.test$/);
  const entries = await historyLength(page);
  const changes = await hashChanges(page);
  await page.goBack();
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await page.goBack();
  await expect(page).toHaveURL(/#\/urls\?lens=example\.test$/);
  await page.goForward();
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  expect(await historyLength(page)).toBe(entries);
  expect(await hashChanges(page)).toBe(changes + 3);
});

test("an old address keeps its own parameters and gains the lens once", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=example.test");
  await urlsReady(page);
  await typeAddress(page, "#/messages?level=warning");
  await expect(page).toHaveURL(/#\/logs\?level=warning&source=module&lens=example\.test$/);
  expect(page.url().match(/lens=/g)?.length).toBe(1);
});

test("a cleared lens stays cleared when the configuration answers late", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(), { delayMs: { config: 2000 } });
  const configAnswered = page.waitForResponse((r) => r.url().includes("/config"));
  await openConsole(page, "pagespeed_global_admin", "#/statistics?lens=example.test");
  await expect(lensSelect(page)).toHaveValue("example.test");
  await lensSelect(page).selectOption("");
  await expect(page).toHaveURL(/#\/statistics$/);
  await configAnswered;
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
  await expect(page).toHaveURL(/#\/statistics$/);
  await expect(lensSelect(page)).toHaveValue("");
  await typeAddress(page, "#/histograms");
  await expect(page).toHaveURL(/#\/histograms$/);
  const stored = await page.evaluate(() => Object.keys(localStorage).filter((k) => k.startsWith("pagespeed.console.lens")));
  expect(stored).toEqual([]);
});

test("the same address typed twice gains the lens both times", async ({ page }) => {
  // After an in-place rewrite the router still holds the address as it was
  // typed; typing it again must still be rewritten.
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=example.test");
  await urlsReady(page);
  await typeAddress(page, "#/statistics");
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await typeAddress(page, "#/statistics");
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await expect(lensSelect(page)).toHaveValue("example.test");
});

test("a per-vhost console never writes lens= into its address", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("pagespeed.console.lens:/pagespeed_admin", "example.test");
    localStorage.setItem("pagespeed.console.lens:/pagespeed_global_admin", "example.test");
  });
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: MESSAGES_HOSTS });
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await typeAddress(page, "#/histograms");
  await expect(page.getByRole("heading", { level: 1, name: "Histograms" })).toBeVisible();
  await expect(page).toHaveURL(/#\/histograms$/);
  await expect(page.getByTestId("host-lens")).toHaveCount(0);
});

test("an address typed with several lens= carries exactly one, the active lens", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies());
  await openConsole(page, "pagespeed_global_admin", "#/urls?lens=example.test");
  await urlsReady(page);
  await typeAddress(page, "#/statistics?lens=example.test&lens=www.example.test");
  await expect(page).toHaveURL(/#\/statistics\?lens=example\.test$/);
  await expect(lensSelect(page)).toHaveValue("example.test");
});
