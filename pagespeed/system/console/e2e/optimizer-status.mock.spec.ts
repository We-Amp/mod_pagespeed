// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Optimizer status: health, load and cache on one page, and the retired
// optimizer pages landing on their sections. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Leaf, type MockRequest, type Reply } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "Optimizer status" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

test("health, load and cache on one page, every leaf read once per refresh", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  for (const name of ["Health", "Load", "Cache"]) {
    await expect(page.getByRole("heading", { level: 2, name, exact: true })).toBeVisible();
  }
  await expect(page.locator(".status-badge")).toHaveText("ok");
  await expect(page.getByText("Notifications received")).toBeVisible();
  await expect(page.getByText("Cache entries")).toBeVisible();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect.poll(() => mock.calls("v1/daemon/stats")).toBeGreaterThanOrEqual(2);
  await page.waitForTimeout(300);
  expect(mock.calls("v1/daemon/health")).toBe(mock.calls("v1/daemon/stats"));
  expect(mock.calls("v1/daemon/cooldowns")).toBe(mock.calls("v1/daemon/stats"));
  await expect(page).toHaveTitle("Optimizer status — mod_pagespeed Admin Console");
  await expect(page.getByRole("link", { name: "Status", exact: true })).toHaveAttribute("aria-current", "page");
});

for (const [old, section, text] of [
  ["#/daemon/back-pressure", "load", "Notifications received"],
  ["#/daemon/cache", "cache", "Cache entries"],
] as const) {
  test(`${old} lands on the ${section} section`, async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 600 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", old);
    await expect(page).toHaveURL(new RegExp(`#/optimizer\\?section=${section}$`));
    await ready(page);
    await expect(page.getByText(text, { exact: true })).toBeVisible();
    await expect(page.locator(`#optimizer-${section}`)).toBeInViewport();
  });
}

test("#/daemon/status lands on the page itself", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/daemon/status");
  await expect(page).toHaveURL(/#\/optimizer$/);
  await ready(page);
});

test("Back after a redirect returns to the page before it", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await page.evaluate(() => {
    location.hash = "#/daemon/status";
  });
  await expect(page).toHaveURL(/#\/optimizer$/);
  await ready(page);
  await page.goBack();
  await expect(page).toHaveURL(/#\/statistics$/);
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
});

test("a section link from another page moves focus to that section's heading", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await page.evaluate(() => {
    location.hash = "#/optimizer?section=cache";
  });
  await expect(page.getByRole("heading", { level: 2, name: "Cache", exact: true })).toBeFocused();
});

const NO_LEAF: Reply = { status: 404, body: { error: "Unknown admin page: v1/daemon" } };

for (const [name, reply, reason, title, says] of [
  ["not configured (503)", F.NOT_CONFIGURED, "not-configured", "No optimizer configured", "the module optimizes on its own"],
  ["not answering (502)", F.UNREACHABLE, "not-answering", "Optimizer not answering", "Start the optimizer service"],
  ["an optimizer that answers 503 itself", { status: 503, body: { error: "busy" } }, "did-not-answer", "Optimizer did not answer", "the next refresh tries again"],
  ["an optimizer without the endpoints (501)", F.UNSUPPORTED, "optimizer-outdated", "This build has no optimizer endpoints", "Update the optimizer package"],
  ["a module without the endpoints (404)", NO_LEAF, "module-unsupported", "This build has no optimizer endpoints", "does not serve the optimizer's status"],
] as [string, Reply, string, string, string][]) {
  test(`no optimizer, ${name}: one explanation in words, and no empty sections`, async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      "v1/daemon/health": reply,
      "v1/daemon/stats": reply,
      "v1/daemon/cooldowns": reply,
    });
    await openConsole(page, "pagespeed_admin", "#/optimizer");
    await ready(page);
    await expect(page.getByTestId("optimizer-absent")).toHaveAttribute("data-reason", reason);
    const state = page.getByTestId("daemon-unreachable");
    await expect(state).toHaveCount(1);
    await expect(state.locator(".empty-title")).toHaveText(title);
    await expect(state).toContainText(says);
    await expect(state.locator("p")).toHaveCount(2);
    if (reason === "did-not-answer") {
      // Nothing to fix and nothing to look up: the next refresh asks again.
      await expect(state.getByRole("link")).toHaveCount(0);
      await expect(state).not.toContainText("Start the optimizer service");
    } else {
      await expect(state.getByRole("link")).toHaveAttribute("href", /^https:\/\/[a-z.]+\/docs\/[a-z-]+\/#[a-z-]+$/);
    }
    // The transport's words stay out of the page.
    await expect(page.locator("main")).not.toContainText(/HTTP \d{3}|daemon_|endpoint_unsupported|Unknown admin page/);
    // No heading with nothing under it, and no report about a cache nothing fills.
    await expect(page.locator("main h2, main h3")).toHaveCount(0);
    await expect(page.locator("#optimizer-html-cache")).toHaveCount(0);
    await expect(page.getByTestId("health-unavailable")).toHaveCount(0);
    await expect(page.getByTestId("cache-unavailable")).toHaveCount(0);
  });
}

test("leaves that disagree: the one that names a configured optimizer decides", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": NO_LEAF });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.getByTestId("optimizer-absent")).toHaveAttribute("data-reason", "not-answering");
});

test("the sections come back when the optimizer answers again", async ({ page }) => {
  let up = false;
  const all = F.allReplies();
  const live = (leaf: Leaf) => (call: number, request: MockRequest): Reply => {
    const answer = all[leaf]!;
    return up ? (typeof answer === "function" ? answer(call, request) : answer) : F.UNREACHABLE;
  };
  await mockAdmin(page, "pagespeed_admin", {
    ...all,
    "v1/daemon/health": live("v1/daemon/health"),
    "v1/daemon/stats": live("v1/daemon/stats"),
    "v1/daemon/cooldowns": live("v1/daemon/cooldowns"),
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.getByTestId("optimizer-absent")).toBeVisible();
  up = true;
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByRole("heading", { level: 2, name: "Health", exact: true })).toBeVisible();
  await expect(page.locator(".status-badge")).toHaveText("ok");
  await expect(page.getByTestId("optimizer-absent")).toHaveCount(0);
});

test("the HTML cache report reads as one more section of the page", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  const size = (el: Element) => getComputedStyle(el).fontSize;
  const panel = page.locator("#optimizer-html-cache");
  const heading = panel.getByRole("heading", { level: 2 });
  await expect(heading).toBeVisible();
  const sibling = await page.getByRole("heading", { level: 2, name: "Cache", exact: true }).evaluate(size);
  expect(await heading.evaluate(size)).toBe(sibling);
  // Its sentences are notes, the size of the page's other notes, not body text.
  const note = await page.getByTestId("serve-savings-source").evaluate(size);
  const sentences = panel.locator("p");
  expect(await sentences.count()).toBeGreaterThan(0);
  for (const p of await sentences.all()) expect(await p.evaluate(size)).toBe(note);
});

test("health answers and statistics do not: each section says what it has", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": F.UNSUPPORTED,
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.locator(".status-badge")).toHaveText("ok");
  await expect(page.getByTestId("statistics-unavailable")).toHaveCount(1);
  await expect(page.getByTestId("statistics-unavailable")).toContainText("this optimizer version does not provide this panel");
  await expect(page.getByTestId("daemon-unreachable")).toHaveCount(0);
  await expect(page.locator("main")).not.toContainText(/HTTP \d{3}|endpoint_unsupported/);
  await expect(page.getByTestId("cache-unavailable")).toBeVisible();
});

test("health answers and statistics get the optimizer's own 503: a refresh without an answer, not 'unreachable'", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": { status: 503, body: { error: "busy" } },
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.locator(".status-badge")).toHaveText("ok");
  const state = page.getByTestId("statistics-unavailable");
  await expect(state).toContainText("Statistics are not available: the optimizer did not answer this refresh.");
  await expect(state).not.toContainText("unreachable");
});

test("statistics answer and health does not: the health section says so", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/stats": F.DAEMON_STATS });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.getByTestId("health-unavailable")).toBeVisible();
  await expect(page.getByText("Cache entries")).toBeVisible();
  await expect(page.getByTestId("daemon-unreachable")).toHaveCount(0);
});

test("phone: the status page does not scroll sideways", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    "v1/daemon/cooldowns": F.ok({
      cooldowns: [{ url: `https://www.example.test/${"x".repeat(300)}.png`, reason: "processing", remaining_seconds: 30, duration_seconds: 60 }],
    }),
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  expect(
    await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
  ).toBe(true);
});
