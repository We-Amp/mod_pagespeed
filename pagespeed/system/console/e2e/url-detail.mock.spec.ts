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
  await expect(page.getByTestId("url-detail-cooldown")).toHaveText("In cooldown (Processing) — 42s remaining.");
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
  await expect(page.getByTestId("url-detail-unreachable")).toContainText("unreachable");
});

test("an unconfigured daemon transport renders the shared empty state", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalDetail(F.NOT_CONFIGURED));
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-unreachable")).toContainText("not configured");
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

test("per-vhost console explains the scope and links to the whole-server console", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", DETAIL_HASH);
  await ready(page);
  await expect(page.getByTestId("url-detail-per-vhost")).toBeVisible();
  await expect(page.getByRole("link", { name: "whole-server console" })).toHaveAttribute(
    "href",
    "/pagespeed_global_admin/#/urls",
  );
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
