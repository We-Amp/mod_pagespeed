// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The URL detail view: every state, mocked end to end, including binary
// variant previews through the content leaf, the module's two content
// slots and busy (429) retries.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

// The route for https://www.example.test/hero.png: path, host, scheme.
const DETAIL_HASH = "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https";

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "URL Detail" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

const globalDetail = (alternates: Responder | undefined, content: Responder = F.CONTENT_HERO) => ({
  ...F.allReplies(),
  config: F.CONFIG_GLOBAL,
  "v1/daemon/cache/alternates": alternates,
  "v1/daemon/cache/content": content,
});

async function expectAllPreviewsLoaded(page: Page, timeout = 5_000) {
  const imgs = page.getByTestId("variant-previews").locator("img");
  await expect(imgs).toHaveCount(3, { timeout });
  for (const img of await imgs.all()) {
    await expect
      .poll(async () => img.evaluate((el: HTMLImageElement) => el.naturalWidth), { timeout })
      .toBe(1);
  }
}

test("a cooldown banner separates its reason and remaining time", async ({ page }) => {
  const hero = F.ALTERNATES_HERO.body as Record<string, unknown>;
  await mockAdmin(
    page,
    "pagespeed_global_admin",
    globalDetail(F.ok({ ...hero, cooldown: { reason: "processing", remaining_seconds: 42 } })),
  );
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-cooldown")).toHaveText("In cooldown (Processing), 42s remaining.");
});

test("renders the variants, badges and image previews", async ({ page }) => {
  const admin = await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-url")).toHaveText("https://www.example.test/hero.png");
  await expect(page.getByTestId("url-detail-status")).toHaveText("Complete");
  await expect(page.getByTestId("variants-table")).toContainText("WEBP");
  await expect(page.getByTestId("variants-table")).toContainText("92.5");
  await expect(page.getByTestId("variants-table")).toContainText("2x+");
  await expect(page.getByTestId("variants-table")).toContainText("Photo");
  await expect(page.getByTestId("variants-table")).toContainText("Content hash record");
  // Three image variants (the sentinel has no preview), each from the
  // content leaf, with alt text and the measured size.
  await expectAllPreviewsLoaded(page);
  const imgs = page.getByTestId("variant-previews").locator("img");
  await expect(imgs.first()).toHaveAttribute("alt", /https:\/\/www\.example\.test\/hero\.png/);
  await expect(imgs.first()).toHaveAttribute("src", /v1\/daemon\/cache\/content\?url=%2Fhero\.png&hostname=www\.example\.test&scheme=https&alternate_id=\d/);
  await expect(page.getByTestId("variant-previews")).toContainText("1×1");
  expect(admin.calls("v1/daemon/cache/content")).toBe(3);
});

test("previews load within the module's two content slots", async ({ page }) => {
  // The mock enforces the module's slots (two concurrent content reads,
  // a third gets 429) and holds each image 300 ms, so three parallel
  // loads would collide; the page's queue never lets them.
  const admin = await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO), {
    oneInFlight: true,
    delayMs: { "v1/daemon/cache/content": 300 },
  });
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expectAllPreviewsLoaded(page);
  expect(admin.busyReplies("v1/daemon/cache/content")).toBe(0);
});

test("a busy content leaf is retried and the previews still load", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO, F.contentBusyOnce()));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expectAllPreviewsLoaded(page, 10_000); // each retries once, after 1 s
});

test("a content leaf that stays busy says so, with a way to try again", async ({ page }) => {
  test.setTimeout(45_000); // three retries (1 s + 2 s + 4 s) per preview, two previews at a time
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO, F.CONTENT_ALWAYS_BUSY));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  const first = page.getByTestId("preview-0");
  await expect(first).toContainText("The server is busy.", { timeout: 20_000 });
  await expect(first.getByRole("button", { name: "Try again" })).toBeVisible();
  await expect(first.locator("img")).toHaveCount(0); // no broken-image icon
});

test("a busy (429) refresh keeps the variants table and the previews", async ({ page }) => {
  let busy = false;
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalDetail(F.ALTERNATES_HERO),
    "v1/daemon/cache/alternates": () => (busy ? F.BUSY : F.ALTERNATES_HERO),
  });
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("variants-table")).toBeVisible();
  await expectAllPreviewsLoaded(page);
  // Another tab holds the daemon's one in-flight alternates read: this
  // page's refresh gets a 429. The poller's busy rule keeps the last view.
  busy = true;
  await Promise.all([
    page.waitForResponse(
      (r) => r.url().includes("/v1/daemon/cache/alternates") && r.status() === 429,
    ),
    page.getByRole("button", { name: "Refresh", exact: true }).click(),
  ]);
  await page.waitForTimeout(150); // let the page process the answer
  await expect(page.getByTestId("variants-table")).toBeVisible();
  await expect(page.getByTestId("url-detail-url")).toHaveText("https://www.example.test/hero.png");
  await expect(page.getByTestId("variant-previews").locator("img")).toHaveCount(3);
  await expect(page.locator("main [role='alert']")).toHaveCount(0);
  await expect(page.getByText("Last refresh failed")).toHaveCount(0);
  // A busy answer is not a failure: the next refresh recovers on its own.
  busy = false;
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByTestId("variants-table")).toBeVisible();
});

test("an entry the optimizer holds nothing for says so", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_NOT_IN_INDEX));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-not-found")).toContainText("Not in the optimizer's index");
  await expect(page.getByTestId("url-detail-not-found")).toContainText("https://www.example.test/hero.png");
});

test("a module without the cache leaves explains instead of erroring", async ({ page }) => {
  // An unmocked leaf answers like a module that lacks it: 404 without a reason code.
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(undefined));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-no-support")).toContainText("This module version cannot show");
});

test("an optimizer below the floor is named with the minimum version", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalDetail(F.ALTERNATES_NOT_IN_INDEX),
    "v1/daemon/health": F.HEALTH_BELOW_FLOOR,
  });
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-below-floor")).toContainText("2.0.3");
  await expect(page.getByTestId("url-detail-below-floor")).toContainText("2.0.41");
});

test("an unreachable daemon renders the shared empty state", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.UNREACHABLE));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  const state = page.getByTestId("url-detail-unreachable");
  await expect(state.locator(".empty-title")).toHaveText("Optimizer not answering");
  await expect(state).toContainText("The optimizer is configured but not answering.");
  // The transport's words stay out of the page.
  await expect(state).not.toContainText(/HTTP \d{3}|daemon_|unreachable/);
});

test("an unconfigured daemon transport renders the shared empty state", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.NOT_CONFIGURED));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  const state = page.getByTestId("url-detail-unreachable");
  await expect(state.locator(".empty-title")).toHaveText("No optimizer configured");
  await expect(state).toContainText("No optimizer is configured for this server; the module optimizes on its own.");
});

test("a garbage alternates reply shows no NaN or undefined", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_MALFORMED));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-status")).toHaveText("Nothing cached");
  await expect(page.getByTestId("url-detail-count")).toHaveText("0");
  await expect(page.locator("main")).not.toContainText("NaN");
  await expect(page.locator("main")).not.toContainText("undefined");
});

// ── Inspect a URL: the module's half on every console ────────────────

// https://www.example.test/a.css: MESSAGES names it once ("Fetch of ... failed").
const CSS_HASH = "#/urls/detail?url=%2Fa.css&host=www.example.test&scheme=https";
const CSS_LOGS = "q=https%3A%2F%2Fwww.example.test%2Fa.css";

test("whole-server console, a URL the optimizer does not hold: both halves and the reasons", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalDetail(F.ALTERNATES_NOT_IN_INDEX),
    cache: F.cacheWithLookup(),
  });
  await openConsole(page, "pagespeed_global_admin", CSS_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-url")).toHaveText("https://www.example.test/a.css");
  await expect(page.getByTestId("url-detail-not-found")).toContainText("Not in the optimizer's index");
  // The module's half: its lookup in words, and its log entries as a count and a link.
  await expect(page.getByTestId("url-module-lookup")).toContainText("This is an original URL");
  await expect(page.getByTestId("url-module-log")).toContainText("1 entry in the module's log names this URL.");
  await expect(page.getByTestId("url-module-log").getByRole("link")).toHaveAttribute(
    "href",
    `#/logs?source=module&level=debug&${CSS_LOGS}`,
  );
  await expect(page.getByTestId("url-optimizer-log").getByRole("link")).toHaveAttribute(
    "href",
    `#/logs?source=optimizer&level=debug&${CSS_LOGS}`,
  );
  // The reasons: four, each with a documentation link, none with evidence.
  const reasons = page.getByTestId("url-reasons").getByRole("listitem");
  await expect(reasons).toHaveCount(4);
  for (const item of await reasons.all()) {
    await expect(item.getByRole("link")).toHaveAttribute("href", /^https:\/\/[a-z.]+\/docs\//);
  }
  await expect(page.getByTestId("url-reason-evidence")).toHaveCount(0);
  // Nothing whole-server to point at: this is that console.
  await expect(page.getByTestId("url-detail-per-vhost")).toHaveCount(0);
  await expect(page.getByRole("navigation").locator("a[aria-current='page']")).toHaveText(/URLs/);
});

test("a URL in cooldown that the optimizer does not hold: the cooldown leads the reasons", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalDetail(F.ALTERNATES_NOT_IN_INDEX),
    cache: F.cacheWithLookup(),
    "v1/daemon/cooldowns": F.ok({
      cooldowns: [
        { url: "/a.css", hostname: "other.example.test", scheme: "https", reason: "write_failure", remaining_seconds: 9 },
        { url: "/a.css", hostname: "www.example.test", scheme: "https", reason: "processing", remaining_seconds: 42 },
      ],
    }),
  });
  await openConsole(page, "pagespeed_global_admin", CSS_HASH);
  await ready(page);
  const first = page.getByTestId("url-reasons").getByRole("listitem").first();
  await expect(first).toHaveAttribute("data-testid", "url-reason-original-kept");
  await expect(first.getByTestId("url-reason-evidence")).toHaveText("In cooldown (Processing), 42 s remaining.");
});

test("an optimized URL shows the module's half too, without the reasons", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", { ...globalDetail(F.ALTERNATES_HERO), cache: F.cacheWithLookup(F.LOOKUP_CURRENT) });
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-status")).toHaveText("Complete");
  await expect(page.getByTestId("url-module-lookup")).toHaveText("The module holds a current rewrite result for this URL.");
  await expect(page.getByTestId("url-module-log")).toContainText("No entry in the module's log names this URL.");
  await expect(page.getByTestId("url-module-log").getByRole("link")).toHaveCount(0);
  await expect(page.getByTestId("url-reasons")).toHaveCount(0);
});

test("per-vhost console: the module's half is the answer, with one line on what the whole-server console adds", async ({ page }) => {
  const admin = await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.cacheWithLookup() });
  await openConsole(page, "pagespeed_admin", CSS_HASH);
  await ready(page);
  await expect(page.getByTestId("url-module-lookup")).toContainText("This is an original URL");
  await expect(page.getByTestId("url-module-log")).toContainText("1 entry in the module's log names this URL.");
  await expect(page.getByTestId("url-reasons").getByRole("listitem")).toHaveCount(4);
  await expect(page.getByTestId("url-detail-per-vhost")).toContainText("The whole-server console adds the optimizer's record of this URL");
  await expect(page.getByRole("link", { name: "whole-server console" })).toHaveAttribute(
    "href",
    "/pagespeed_global_admin/#/urls/detail?url=%2Fa.css&host=www.example.test&scheme=https",
  );
  // No optimizer data on a per-vhost console: nothing is asked, nothing is shown.
  await expect(page.getByTestId("url-optimizer-log")).toHaveCount(0);
  await expect(page.locator("main")).not.toContainText("Not in the optimizer's index");
  // The sidebar has no URLs entry here; the page lives under Overview, where its input is.
  await expect(page.getByRole("navigation").locator("a[aria-current='page']")).toHaveText(/Overview/);
  // The module's half keeps refreshing; once the scope is known the
  // optimizer's record is not requested (at most the one read that raced it).
  await expect(page.getByRole("button", { name: /Auto-refresh · \d+ s/ })).toHaveAttribute("aria-pressed", "true");
  const asked = admin.calls("v1/daemon/cache/alternates");
  const read = admin.calls("message_history");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect.poll(() => admin.calls("message_history")).toBeGreaterThan(read);
  expect(admin.calls("v1/daemon/cache/alternates")).toBe(asked);
  expect(asked).toBeLessThanOrEqual(1);
});

test("a per-vhost 403 that arrives before the scope is known is final; the module's half keeps refreshing", async ({ page }) => {
  // A renamed admin path looks whole-server until /config answers: the first read asks the optimizer.
  const admin = await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.cacheWithLookup() }, { delayMs: { config: 600 } });
  await openConsole(page, "pagespeed_admin", CSS_HASH);
  await ready(page);
  await expect(page.getByTestId("url-reasons")).toBeVisible();
  const asked = admin.calls("v1/daemon/cache/alternates");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByTestId("url-reasons")).toBeVisible();
  expect(admin.calls("v1/daemon/cache/alternates")).toBe(asked);
  expect(asked).toBeLessThanOrEqual(1);
});

test("a server without an optimizer: the module's answer is the whole answer, with nowhere else to send the reader", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    cache: F.cacheWithLookup(),
    "v1/daemon/health": F.NOT_CONFIGURED,
    "v1/daemon/stats": F.NOT_CONFIGURED,
    "v1/daemon/cooldowns": F.NOT_CONFIGURED,
  });
  await openConsole(page, "pagespeed_admin", CSS_HASH);
  await ready(page);
  await expect(page.getByTestId("url-module-lookup")).toContainText("This is an original URL");
  await expect(page.getByTestId("url-reasons").getByRole("listitem")).toHaveCount(4);
  await expect(page.getByTestId("url-detail-per-vhost")).toHaveText(
    "No optimizer is connected to this server: the module's answer is the whole answer.",
  );
  await expect(page.getByRole("link", { name: "whole-server console" })).toHaveCount(0);
  await expect(page.locator("main")).not.toContainText(/unreachable|HTTP [45]\d\d/);
});

test("whole-server console without an optimizer: no link to an optimizer log that does not exist", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalDetail(F.NOT_CONFIGURED),
    cache: F.cacheWithLookup(),
    "v1/daemon/health": F.NOT_CONFIGURED,
    "v1/daemon/cooldowns": F.NOT_CONFIGURED,
  });
  await openConsole(page, "pagespeed_global_admin", CSS_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-unreachable")).toContainText("not configured");
  await expect(page.getByTestId("url-optimizer-log")).toHaveCount(0);
  await expect(page.getByTestId("url-module-lookup")).toBeVisible();
  await expect(page.getByTestId("url-reasons").getByRole("listitem")).toHaveCount(4);
});

test("the module's refusals are said in words: another host's URL, a lookup that does not answer", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.cacheWithLookup(F.LOOKUP_OTHER_HOST) });
  await openConsole(page, "pagespeed_admin", "#/urls/detail?url=%2Fa.css&host=other.example.test&scheme=https");
  await ready(page);
  await expect(page.getByTestId("url-module-lookup")).toHaveText(
    "This URL belongs to another host; that host's console answers for it.",
  );
  await expect(page.getByTestId("url-reasons")).toBeVisible();

  // The structure where a lookup answer was expected, and a failing log: no result is invented.
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), message_history: F.MODULE_FAILURE });
  await openConsole(page, "pagespeed_admin", CSS_HASH);
  await page.reload();
  await ready(page);
  await expect(page.getByTestId("url-module-lookup")).toHaveText("The module gave no lookup result.");
  await expect(page.getByTestId("url-module-log")).toHaveText("The module's log could not be read.");
  await expect(page.locator("main")).not.toContainText(/HTTP [45]\d\d|NaN|undefined/);
});

test("a busy module lookup keeps the last answer", async ({ page }) => {
  let busy = false;
  const admin = await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    cache: (_n, request) => (request.url.searchParams.has("url") ? (busy ? F.BUSY : F.LOOKUP_CURRENT) : F.CACHE_STRUCTURE),
  });
  await openConsole(page, "pagespeed_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-module-lookup")).toContainText("holds a current rewrite result");
  busy = true;
  const before = admin.calls("cache");
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect.poll(() => admin.calls("cache")).toBeGreaterThan(before);
  await expect(page.getByTestId("url-module-lookup")).toContainText("holds a current rewrite result");
});

test("missing or invalid entry parameters never issue a request", async ({ page }) => {
  const admin = await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO));
  for (const hash of [
    "#/urls/detail?url=https%3A%2F%2Fwww.example.test%2Fhero.png&host=www.example.test&scheme=https",
    "#/urls/detail?url=%2Fhero.png&scheme=https",
    "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=ftp",
  ]) {
    await openConsole(page, "pagespeed_global_admin", hash);
    await ready(page);
    await expect(page.getByTestId("url-detail-misuse")).toBeVisible();
    // The way forward is on the page: the input that leads here.
    await expect(page.getByTestId("inspect-url")).toBeVisible();
    // Nothing to refresh without a URL: the header offers no auto-refresh,
    // no manual refresh and no update stamp.
    await expect(page.getByRole("button", { name: /Auto-refresh · \d+ s/ })).toHaveCount(0);
    await expect(page.getByRole("button", { name: "Refresh", exact: true })).toHaveCount(0);
    await expect(page.getByTestId("page-updated")).toHaveCount(0);
  }
  expect(admin.calls("v1/daemon/cache/alternates")).toBe(0);
});

test("duplicate variant ids render as separate rows and previews", async ({ page }) => {
  const hero = F.ALTERNATES_HERO.body as { alternates: Record<string, unknown>[] };
  const webp = hero.alternates.find((a) => a.alternate_id === 1) as Record<string, unknown>;
  await mockAdmin(
    page,
    "pagespeed_global_admin",
    globalDetail({
      status: 200,
      body: {
        url: "/hero.png",
        hostname: "www.example.test",
        scheme: "https",
        cache_key: "https://www.example.test/hero.png",
        count: 2,
        chain_length: 2,
        alternates: [webp, webp],
      },
    }),
  );
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  // The daemon sent alternate 1 twice: both rows and both previews render
  // (distinct each keys); no keyed-each breakage.
  await expect(page.getByTestId("url-detail-count")).toHaveText("2");
  await expect(page.getByTestId("variants-table").locator("tbody tr")).toHaveCount(2);
  await expect(page.getByTestId("variant-previews").locator("figure")).toHaveCount(2);
});

test("the variants table sorts from the keyboard", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.ALTERNATES_HERO));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  // Default: hits descending — webp (1042) first.
  const firstRow = page.getByTestId("variants-table").locator("tbody tr").first();
  await expect(firstRow).toContainText("WEBP");
  await page.getByRole("button", { name: "Size" }).focus();
  await page.keyboard.press("Enter");
  // A numeric column opens descending: the 45,210-byte original first.
  await expect(page.getByTestId("variants-table").locator("tbody tr").first()).toContainText("ORIGINAL");
  // A second activation reverses the order: ascending, the 0-byte sentinel first.
  await page.keyboard.press("Enter");
  await expect(page.getByTestId("variants-table").locator("tbody tr").first()).toContainText("Content hash record");
});

test("the Cache lookup result links to the detail view with path, host and scheme", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/caches");
  await page.getByRole("tab", { name: "Cache Lookup" }).click();
  await page.getByLabel("URL to look up:").fill("https://www.example.test/hero.png");
  await page.getByRole("button", { name: "Lookup" }).click();
  const link = page.getByRole("link", { name: "Inspect in the optimizer's URL index" });
  await expect(link).toHaveAttribute("href", DETAIL_HASH);
});
