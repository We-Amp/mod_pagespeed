// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tables and lists from the keyboard; counter names on a phone; what a
// screen reader is told about a table and a filter's result.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";
import { rigPerHostReplies, rigWholeServerReplies } from "./support/rig";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Tables", () => {
  test("Statistics: sortable headers are buttons that announce the sort", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const name = page.getByRole("columnheader", { name: "Name" }).first();
    const value = page.getByRole("columnheader", { name: "Value" }).first();
    await expect(name).toHaveAttribute("aria-sort", "ascending");
    await expect(value).toHaveAttribute("aria-sort", "none");
    await value.getByRole("button").focus();
    await page.keyboard.press("Enter");
    await expect(value).toHaveAttribute("aria-sort", "descending");
    await expect(name).toHaveAttribute("aria-sort", "none");
    await page.keyboard.press("Space");
    await expect(value).toHaveAttribute("aria-sort", "ascending");
  });

  test("Statistics: the delta column sorts from the keyboard", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await page.getByRole("checkbox", { name: "Δ since open" }).check();
    const delta = page.getByRole("columnheader", { name: "Δ since open" }).first();
    await expect(delta).toHaveAttribute("aria-sort", "none");
    await delta.getByRole("button").focus();
    await page.keyboard.press("Enter");
    await expect(delta).not.toHaveAttribute("aria-sort", "none");
  });

  test("Histograms: a histogram is chosen with the keyboard", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
    await openConsole(page, "pagespeed_admin", "#/histograms");
    const second = page.getByRole("button", { name: "Rewrite Latency ms" });
    await expect(second).toHaveAttribute("aria-pressed", "false");
    await second.focus();
    await page.keyboard.press("Enter");
    await expect(second).toHaveAttribute("aria-pressed", "true");
    await expect(page.getByRole("heading", { level: 2, name: "Rewrite Latency ms" })).toBeVisible();
  });

  test("Statistics shows descriptions as a second line until the toggle is turned off", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("columnheader", { name: "Description" })).toHaveCount(0);
    await expect(page.locator(".stat-desc").first()).toBeVisible();
  });

  test("on a phone, counter names wrap between words and the page does not scroll sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const cell = page.locator(".name-cell").first();
    await expect(cell).toBeVisible();
    expect(await cell.locator("wbr").count()).toBeGreaterThan(0);
    await expect(cell).toHaveCSS("word-break", "normal");
    expect(
      await page.evaluate(
        () => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1,
      ),
    ).toBe(true);
  });

  test("sortable columns show a sort affordance before they are sorted", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const nameHeader = page.getByRole("columnheader", { name: "Name" }).first();
    const valueHeader = page.getByRole("columnheader", { name: "Value" }).first();
    await expect(nameHeader).toHaveAttribute("aria-sort", "ascending");
    await expect(valueHeader).toHaveAttribute("aria-sort", "none");
    await expect(valueHeader.getByRole("button")).toContainText("↕");
    await valueHeader.getByRole("button").click();
    await expect(valueHeader).toHaveAttribute("aria-sort", "descending");
    await expect(valueHeader.getByRole("button")).toContainText("▼");
    await expect(nameHeader.getByRole("button")).toContainText("↕");
  });
});

/** Header cells in the main region that do not say what they head. */
function headersWithoutScope(page: Page): Promise<string[]> {
  return page.locator("main").evaluate((m) =>
    [...m.querySelectorAll("th")]
      .filter((th) => !["col", "row"].includes(th.getAttribute("scope") ?? ""))
      .map((th) => th.textContent!.trim()),
  );
}

test.describe("Table semantics", () => {
  const CONSOLES = [
    { admin: "pagespeed_global_admin", replies: rigWholeServerReplies },
    { admin: "pagespeed_admin", replies: rigPerHostReplies },
  ] as const;
  const PAGES = [
    "#/overview", "#/savings", "#/statistics", "#/histograms", "#/caches", "#/optimizer", "#/urls",
    "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https", "#/rule-packs",
  ];

  for (const { admin, replies } of CONSOLES) {
    test(`${admin}: every header cell names its scope`, async ({ page }) => {
      await mockAdmin(page, admin, replies());
      for (const hash of PAGES) {
        await openConsole(page, admin, hash);
        await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
        await expect(page.locator("main .loading")).toHaveCount(0);
        expect(await headersWithoutScope(page), hash).toEqual([]);
      }
    });
  }

  test("Statistics: the counter's name heads its row", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const row = page.getByRole("row").filter({ has: page.getByRole("rowheader", { name: /^num_flushes/ }) });
    await expect(row).toHaveCount(1);
    await expect(row.getByRole("cell")).not.toHaveCount(0);
  });

  test("Savings: each table has a name and each row a type or host as its header", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", rigWholeServerReplies());
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await expect(page.getByRole("table", { name: "Module rewrites, by type" })).toBeVisible();
    const optimizer = page.getByRole("table", { name: "Optimizer cache serves, by type" });
    await expect(optimizer).toBeVisible();
    await expect(optimizer.getByRole("rowheader", { name: "CSS" })).toBeVisible();
    const byHost = page.getByTestId("savings-by-host");
    await expect(byHost).toHaveAccessibleName(/, by host$/);
    await expect(byHost.getByRole("rowheader").first()).toBeVisible();
  });

  test("a sortable header is a target over its whole cell", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const value = page.getByRole("columnheader", { name: "Value" }).first();
    await expect(value).toHaveAttribute("aria-sort", "none");
    // The cell's left edge, away from the right-aligned button.
    const cell = (await value.boundingBox())!;
    const button = (await value.getByRole("button").boundingBox())!;
    expect(button.x - cell.x).toBeGreaterThan(20);
    await page.mouse.click(cell.x + 4, cell.y + cell.height - 3);
    await expect(value).toHaveAttribute("aria-sort", "descending");
  });
});

test.describe("A filter's result count", () => {
  // The count is read out (a polite status) once the typing stops, and the
  // timer polls never fill it.
  const CASES = [
    {
      hash: "#/statistics",
      box: (page: Page) => page.getByRole("searchbox", { name: "Search statistics" }),
      status: "statistics-count-status",
      text: /^\d+ of \d+ variables$/,
      admin: "pagespeed_admin",
    },
    {
      hash: "#/histograms",
      box: (page: Page) => page.getByRole("textbox", { name: "Filter histograms by name" }),
      status: "histograms-count-status",
      text: /^\d+ of \d+ histograms$/,
      admin: "pagespeed_admin",
    },
    {
      hash: "#/logs",
      box: (page: Page) => page.getByTestId("logs-search"),
      status: "logs-count-status",
      text: /^Showing \d+ of \d+ entries$/,
      admin: "pagespeed_global_admin",
    },
    {
      hash: "#/urls",
      box: (page: Page) => page.getByTestId("urls-search"),
      status: "urls-count-status",
      text: /^\d+ of \d+ URLs on this page$/,
      admin: "pagespeed_global_admin",
    },
  ] as const;

  for (const c of CASES) {
    test(`${c.hash}: the count is a polite status, filled only after typing`, async ({ page }) => {
      const replies = c.admin === "pagespeed_global_admin" ? rigWholeServerReplies() : rigPerHostReplies();
      await mockAdmin(page, c.admin, replies);
      await openConsole(page, c.admin, c.hash);
      const status = page.getByTestId(c.status);
      await expect(status).toHaveRole("status");
      await expect(status).toHaveText("");
      // A refresh says nothing.
      await page.getByRole("button", { name: "Refresh", exact: true }).click();
      await expect(page.getByTestId("refresh-result")).not.toHaveText("");
      await expect(status).toHaveText("");
      await c.box(page).pressSequentially("e");
      await expect(status).toHaveText(c.text);
    });
  }
});
