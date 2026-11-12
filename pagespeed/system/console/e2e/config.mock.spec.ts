// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Configuration page: the server's text read into Filters and Options,
// a line filter with a count, Copy, and the lines that differ between the
// whole-server and the effective text.

import AxeBuilder from "@axe-core/playwright";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Reply } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const RIG = fileURLToPath(new URL("../test-fixtures/rig/", import.meta.url));
const rigConfig = JSON.parse(readFileSync(`${RIG}config.global.json`, "utf8")) as {
  config: string;
  effective_config: string;
  scope: string;
  host: string;
};

async function open(
  page: Page,
  config: Reply = F.ok(rigConfig),
  admin: "pagespeed_global_admin" | "pagespeed_admin" = "pagespeed_global_admin",
) {
  const errors: string[] = [];
  page.on("pageerror", (err) => errors.push(err.message));
  await mockAdmin(page, admin, { ...F.allReplies(), config });
  await openConsole(page, admin, "#/configuration");
  await expect(page.getByTestId("config-text")).toBeVisible();
  return { errors };
}

/** The clipboard a secure page has; what it was handed is read back with copied(). */
async function grantClipboard(page: Page) {
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: {
        writeText: async (text: string) => {
          (window as unknown as { __copied: string }).__copied = text;
        },
      },
    });
  });
}
const copied = (page: Page) => page.evaluate(() => (window as unknown as { __copied?: string }).__copied);

// The rig's effective text with one option changed and one filter more.
const EFFECTIVE_DIFFERS = rigConfig.effective_config
  .replace("EnableRewriting (e)                 1", "EnableRewriting (e)                 0")
  .replace("md\tStrip Image Meta Data\n", "md\tStrip Image Meta Data\nll\tLazyload Images\n");

test.describe("Configuration", () => {
  test("the server's text reads as a list of filters and a table of options", async ({ page }) => {
    const { errors } = await open(page);
    await expect(page.getByTestId("config-text")).toHaveAttribute("data-structured", "true");
    await expect(page.getByRole("heading", { name: "Filters (30)" })).toBeVisible();
    await expect(page.getByTestId("config-filters").getByRole("listitem")).toHaveCount(30);
    await expect(page.getByTestId("config-filters").getByRole("listitem").first()).toHaveText(/ah\s+Add Head/);
    const options = page.getByTestId("config-options");
    await expect(page.getByRole("heading", { name: "Options (8)" })).toBeVisible();
    await expect(options.getByRole("row")).toHaveCount(9);
    await expect(options.getByRole("row", { name: /FileCachePath/ }).getByRole("cell")).toHaveText([
      "afcp",
      "/var/cache/mod_pagespeed/",
    ]);
    // The rest of the text, as text.
    await expect(page.getByText("Version: 15: on")).toBeVisible();
    await expect(page.getByRole("heading", { name: "Domain Lawyer" })).toBeVisible();
    await expect(page.getByText("Invalidation Timestamp: (none)")).toBeVisible();
    await expect(page.getByTestId("config-count")).toHaveText("40 lines");
    // The two tabs and what they hold.
    await expect(page.getByRole("tab", { name: "Whole server" })).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tab", { name: "Effective" })).toBeVisible();
    await expect(page.getByTestId("config-same")).toHaveText(
      "The whole-server text and the text in effect for this request are the same.",
    );
    await expect(page.getByRole("region", { name: "Whole-server configuration" })).toBeVisible();
    expect(errors).toEqual([]);
  });

  test("the filter narrows every section, counts what is left and says it once the box is quiet", async ({ page }) => {
    await open(page);
    const search = page.getByRole("searchbox", { name: "Filter configuration lines" });
    await search.fill("jpeg");
    await expect(page.getByTestId("config-count")).toHaveText("5 of 40 lines");
    await expect(page.getByTestId("config-filters").getByRole("listitem")).toHaveCount(5);
    await expect(page.getByTestId("config-options")).toHaveCount(0);
    await expect(page.getByRole("heading", { name: "Domain Lawyer" })).toHaveCount(0);
    await expect(page.getByTestId("config-count-status")).toHaveText("5 of 40 lines");
    // An option by its id, its value, or both words.
    await search.fill("afcp /var");
    await expect(page.getByTestId("config-count")).toHaveText("1 of 40 lines");
    await expect(page.getByTestId("config-options").getByRole("row")).toHaveCount(2);
    await search.fill("no such option");
    await expect(page.getByTestId("config-no-match")).toHaveText('No line matches "no such option".');
    await expect(page.getByTestId("config-count")).toHaveText("0 of 40 lines");
    // The filter stays across the tabs.
    await search.fill("jpeg");
    await page.getByRole("tab", { name: "Effective" }).click();
    await expect(search).toHaveValue("jpeg");
    await expect(page.getByTestId("config-count")).toHaveText("5 of 40 lines");
    await search.fill("");
    await expect(page.getByTestId("config-count")).toHaveText("40 lines");
  });

  test("Copy hands the whole text of the tab to the clipboard, not only the lines shown", async ({ page }) => {
    await grantClipboard(page);
    await open(page, F.ok({ ...rigConfig, effective_config: EFFECTIVE_DIFFERS }));
    await page.getByRole("searchbox", { name: "Filter configuration lines" }).fill("jpeg");
    await page.getByRole("button", { name: "Copy text" }).click();
    await expect(page.getByTestId("config-copy-status")).toHaveText("Copied");
    expect(await copied(page)).toBe(rigConfig.config);
    await page.getByRole("tab", { name: "Effective" }).click();
    await expect(page.getByTestId("config-copy-status")).toHaveText("");
    await page.getByRole("button", { name: "Copy text" }).click();
    expect(await copied(page)).toBe(EFFECTIVE_DIFFERS);
  });

  test("a console over plain HTTP has no clipboard: Copy still copies, and the focus stays on the button", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", { configurable: true, value: undefined });
      document.execCommand = ((command: string) => {
        const area = document.activeElement as HTMLTextAreaElement | null;
        (window as unknown as { __copied: string }).__copied =
          `${command}:${area?.value.slice(area.selectionStart, area.selectionEnd) ?? ""}`;
        return true;
      }) as typeof document.execCommand;
    });
    await open(page);
    const button = page.getByRole("button", { name: "Copy text" });
    await button.click();
    await expect(page.getByTestId("config-copy-status")).toHaveText("Copied");
    expect(await copied(page)).toBe(`copy:${rigConfig.config}`);
    await expect(button).toBeFocused();
  });

  test("the lines that differ between the two texts are marked", async ({ page }) => {
    await open(page, F.ok({ ...rigConfig, effective_config: EFFECTIVE_DIFFERS }));
    // The whole-server text has one line the effective one lacks: EnableRewriting 1.
    await expect(page.getByTestId("config-differs")).toHaveText(
      '1 line not in the text in effect for this request, marked "differs".',
    );
    await expect(page.getByTestId("config-options").getByRole("row", { name: /EnableRewriting/ })).toContainText("differs");
    await page.getByRole("tab", { name: "Effective" }).click();
    await expect(page.getByRole("region", { name: "Effective configuration" })).toBeVisible();
    await expect(page.getByTestId("config-differs")).toHaveText('2 lines not in the whole-server text, marked "differs".');
    await expect(page.getByText("differs", { exact: true })).toHaveCount(2);
    await expect(page.getByTestId("config-filters").getByRole("listitem").filter({ hasText: "Lazyload" })).toHaveText(
      /ll\s+Lazyload Images\s+differs/,
    );
    await expect(page.getByTestId("config-count")).toHaveText("41 lines");
  });

  test("a per-host console names its first text this site's", async ({ page }) => {
    await open(page, F.ok({ ...rigConfig, scope: "vhost", host: "site-a.test:80" }), "pagespeed_admin");
    await expect(page.getByRole("tab")).toHaveText(["This site", "Effective"]);
    await expect(page.getByRole("region", { name: "This site's configuration" })).toBeVisible();
    await expect(page.getByTestId("config-same")).toHaveText(
      "This site's text and the text in effect for this request are the same.",
    );
  });

  test("text of another shape is shown line by line, and the filter and marks still work", async ({ page }) => {
    await open(page, F.CONFIG_VHOST_WITH_TEXT, "pagespeed_admin");
    await expect(page.getByTestId("config-text")).toHaveAttribute("data-structured", "false");
    await expect(page.getByTestId("config-filters")).toHaveCount(0);
    await expect(page.getByTestId("config-count")).toHaveText("4 lines");
    await page.getByRole("tab", { name: "Effective" }).click();
    await expect(page.getByTestId("config-count")).toHaveText("5 lines");
    await expect(page.getByTestId("config-differs")).toHaveText('1 line not in this site\'s text, marked "differs".');
    await expect(page.getByTestId("config-text")).toContainText("ModPagespeedStatistics on differs");
    await page.getByRole("searchbox", { name: "Filter configuration lines" }).fill("cache");
    await expect(page.getByTestId("config-count")).toHaveText("1 of 5 lines");
    await expect(page.getByTestId("config-text")).toHaveText("ModPagespeedFileCachePath /var/cache/mod_pagespeed/");
  });

  test("the server's shape with a line that does not parse: every line can still be found", async ({ page }) => {
    const malformed = rigConfig.config.replace("cc\tCombine Css", "cc Combine Css no tab");
    await open(page, F.ok({ ...rigConfig, config: malformed, effective_config: malformed }));
    await expect(page.getByTestId("config-text")).toHaveAttribute("data-structured", "false");
    await expect(page.getByTestId("config-count")).toHaveText("43 lines");
    await page.getByRole("searchbox", { name: "Filter configuration lines" }).fill("no tab");
    await expect(page.getByTestId("config-count")).toHaveText("1 of 43 lines");
    await expect(page.getByTestId("config-text")).toHaveText("cc Combine Css no tab");
  });

  test("no effective text: no tabs and no comparison", async ({ page }) => {
    await open(page, F.ok({ config: rigConfig.config, scope: "global", host: "" }));
    await expect(page.getByRole("tablist", { name: "Configuration view" })).toHaveCount(0);
    await expect(page.getByTestId("config-same")).toHaveCount(0);
    await expect(page.getByTestId("config-differs")).toHaveCount(0);
    await expect(page.getByRole("region", { name: "Whole-server configuration" })).toBeVisible();
  });

  test("the options table is label/value cards on a phone, and the page does not scroll sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await open(page);
    const row = page.getByTestId("config-options").getByRole("row", { name: /DaemonVolumePath/ });
    const cells = row.locator("th, td");
    const boxes = await cells.evaluateAll((els) => els.map((el) => el.getBoundingClientRect().top));
    // Stacked: each cell below the one before.
    expect(boxes[1]).toBeGreaterThan(boxes[0]);
    expect(boxes[2]).toBeGreaterThan(boxes[1]);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  });

  test("at 320 px neither the page nor the configuration box scrolls sideways", async ({ page }) => {
    await page.setViewportSize({ width: 320, height: 800 });
    await open(page, F.ok({ ...rigConfig, effective_config: EFFECTIVE_DIFFERS }));
    for (const tab of ["Whole server", "Effective"]) {
      await page.getByRole("tab", { name: tab }).click();
      const region = page.locator(".config-wrapper");
      expect(await region.evaluate((el) => el.scrollWidth <= el.clientWidth), tab).toBe(true);
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), tab).toBe(true);
    }
  });
});

for (const scheme of ["light", "dark"] as const) {
  test(`Configuration with marked lines and a filter has no accessibility violations (${scheme})`, async ({ page }) => {
    await page.emulateMedia({ colorScheme: scheme });
    await open(page, F.ok({ ...rigConfig, effective_config: EFFECTIVE_DIFFERS }));
    await page.getByRole("tab", { name: "Effective" }).click();
    await page.getByRole("searchbox", { name: "Filter configuration lines" }).fill("e");
    await expect(page.getByText("differs", { exact: true })).toHaveCount(2);
    const result = await new AxeBuilder({ page })
      .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa", "wcag22a", "wcag22aa"])
      .analyze();
    expect(result.violations.map((v) => `${v.id}: ${v.nodes.map((n) => n.target.join(" ")).join(" | ")}`)).toEqual([]);
  });
}
