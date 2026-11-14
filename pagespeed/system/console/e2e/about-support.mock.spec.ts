// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// About and Support. Self-contained (support/mock-admin.ts); run after build.sh.

import { readFileSync } from "node:fs";
import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("About and Support", () => {
  test("About: the build stamp under its two labels, the optimizer version, the documentation", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/about");
    // The module and the console ship from one build: one stamp, shown once
    // per label (module build, console build), and nowhere else on the page.
    const build = (await page.locator(".topbar-version").getAttribute("title"))?.trim() ?? "";
    expect(build).not.toBe("");
    await expect(page.getByTestId("about-console-version")).toHaveText(build);
    await expect(page.getByTestId("about-module-version")).toContainText(build);
    const main = await page.locator("main").innerText();
    expect(main.split(build).length - 1).toBe(2);
    await expect(page.getByTestId("about-optimizer-version")).toHaveText("2.0.41 (52344ff)");
    await expect(page.getByRole("link", { name: "Admin console documentation" })).toHaveAttribute(
      "href",
      "https://modpagespeed.com/docs/admin-console/",
    );
  });

  test("About: where the pages of the old console went", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/about");
    const section = page.getByTestId("about-old-console");
    await expect(section.getByRole("heading", { level: 2 })).toHaveText("Coming from the old console?");
    await expect(section.locator("dt")).toHaveText(["Console (graphs)", "Message History"]);
    await expect(section.getByRole("link", { name: "Statistics, Graphs view" })).toHaveAttribute("href", "#/graphs");
    await section.getByRole("link", { name: "Logs, Module source" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=module$/);
    await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
  });

  test("About without an optimizer says so", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), "v1/daemon/health": F.NOT_CONFIGURED });
    await openConsole(page, "pagespeed_admin", "#/about");
    await expect(page.getByTestId("about-optimizer-version")).toHaveText("Not configured");
  });

  test("the versions block names all three builds", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      "v1/daemon/health": F.HEALTH_OK,
    });
    await openConsole(page, "pagespeed_admin", "#/about");
    await expect(page.getByTestId("about-module-version")).toContainText("v"); // the full module stamp, whatever this checkout's is
    await expect(page.getByTestId("about-optimizer-version")).toContainText("2.0.41"); // the version F.HEALTH_OK serves
    await expect(page.getByTestId("about-optimizer-version")).toContainText("52344ff");
    await expect(page.getByTestId("about-console-version")).toContainText("v");
  });

  test("the shortcut dialog carries the same versions", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      "v1/daemon/health": F.HEALTH_OK,
    });
    await openConsole(page, "pagespeed_admin", "#/about");
    await page.locator(".shortcuts-button").click();
    const versions = page.getByTestId("shortcut-versions");
    await expect(versions).toContainText("2.0.41");
    await expect(versions).toContainText("52344ff");
    const stamp = (await page.locator(".topbar-version").getAttribute("title")) ?? "";
    await expect(versions).toContainText(stamp);
  });

  test("the shortcut dialog says when the optimizer is not configured", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), "v1/daemon/health": F.NOT_CONFIGURED });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await page.locator(".shortcuts-button").click();
    await expect(page.getByTestId("shortcut-versions")).toContainText("Not configured");
  });

  test("the top bar shows the tag with the full stamp in its tooltip", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/about");
    const stamp = await page.locator(".topbar-version").getAttribute("title");
    expect(stamp).toBeTruthy();
    await expect(page.locator(".topbar-version")).toHaveText(/^v\d+\.\d+\.\d+$/);
  });

  test("the publisher is named on About, not in the top bar beside the connection state", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/about");
    await expect(page.getByTestId("about-vendor")).toContainText("is published by We-Amp B.V.");
    await expect(page.getByTestId("about-vendor").getByRole("link", { name: "we-amp.com" })).toHaveAttribute("href", "https://we-amp.com/");
    await expect(page.locator(".topbar a[href='https://we-amp.com/']")).toHaveCount(0);
  });

  test("Support: one full stop after the vendor, and still dismissible", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    const panel = page.getByTestId("support-panel");
    await expect(panel).toContainText("We-Amp B.V. A support subscription");
    await expect(panel).not.toContainText("..");
    await page.getByRole("button", { name: "Dismiss support panel" }).click();
    await expect(panel).toHaveCount(0);
    await page.getByRole("button", { name: "Show again" }).click();
    await expect(panel).toBeVisible();
  });

  test("Support: the documentation stays after the panel is dismissed", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    const heading = page.getByRole("heading", { name: "Documentation and help" });
    await expect(heading).toBeVisible();
    await expect(page.getByRole("link", { name: "Admin console documentation" })).toHaveAttribute(
      "href", "https://modpagespeed.com/docs/admin-console/",
    );
    await expect(page.getByRole("link", { name: "Support subscriptions", exact: true })).toHaveAttribute(
      "href", "https://modpagespeed.com/pricing/",
    );
    await expect(page.getByRole("link", { name: "Support terms" })).toHaveAttribute(
      "href", "https://we-amp.com/licensing/",
    );
    await page.getByRole("button", { name: "Dismiss support panel" }).click();
    await expect(page.getByTestId("support-panel")).toHaveCount(0);
    await expect(heading).toBeVisible();
    await expect(page.getByRole("button", { name: "Copy diagnostics" })).toBeVisible();
  });

  test("Support: the diagnostics bundle is shown when the page opens, read once from this console's pages", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    await expect(page.getByRole("heading", { name: "Diagnostics" })).toBeVisible();
    const area = page.getByLabel("Diagnostics text");
    await expect(area).toHaveAttribute("readonly", "");
    await expect(area).toHaveValue(/^mod_pagespeed diagnostics\n/);
    const text = await area.inputValue();
    expect(text).toMatch(/^Build: v\S+$/m);
    expect(text).toContain("Console: this host (www.example.test:80)\n");
    expect(text).toMatch(/^Browser: Mozilla\/5\.0 .+$/m);
    expect(text).toMatch(/^Web server: (not detected|Apache|nginx|IIS)$/m);
    expect(text).toMatch(/^Module started: (unknown|\d{4}-\d\d-\d\dT.+)$/m);
    expect(text).toContain("Optimizer: 2.0.41 (52344ff)\n");
    expect(text).toMatch(/^Optimizer started: /m);
    expect(text).toContain("Configuration SHA-256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n");
    expect(text).toMatch(/^(Findings \(\d+, from one reading\):|Findings: none in one reading)$/m);
    expect(text).toMatch(/^Optimizer checks/m);
    expect(text).toContain("Key counters (since the module started):\n  page_load_count: ");
    expect(text).toMatch(/^Caches/m);
    expect(text).toMatch(/^Purging: /m);
    expect(text).toContain("[error] [Sat, 26 Sep 2026 10:04:00 GMT] [Error] [531] Fetch of https://www.example.test/a.css failed\n");
    expect(text).not.toContain("CycloneCache enabled");
    expect(text).toMatch(/\nLeft out of this bundle:\n- the configuration text \(its SHA-256 is above\)\n- query strings in addresses\n/);

    // "Read again" reads each source once more; copying reads nothing.
    const leaves = ["config", "stats_json", "v1/daemon/health", "v1/daemon/stats", "message_history", "cache"] as const;
    const before = Object.fromEntries(leaves.map((l) => [l, mock.calls(l)]));
    await page.getByRole("button", { name: "Read again" }).click();
    const delta = () => Object.fromEntries(leaves.map((l) => [l, mock.calls(l) - before[l]]));
    // message_history: one read serves the findings and the warning list; cache: the structure and the purge set.
    await expect.poll(delta).toEqual({ config: 1, stats_json: 1, "v1/daemon/health": 1, "v1/daemon/stats": 1, message_history: 1, cache: 2 });
    await expect(page.getByRole("button", { name: "Read again" })).toBeEnabled();
    await expect(page.getByTestId("diagnostics-status")).toHaveText("");
  });

  test("Support: Copy diagnostics copies the text that is shown", async ({ page }) => {
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
    const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    const area = page.getByLabel("Diagnostics text");
    await expect(area).toHaveValue(/^mod_pagespeed diagnostics\n/);
    const config = mock.calls("config");
    await page.getByRole("button", { name: "Copy diagnostics" }).click();
    await expect(page.getByTestId("diagnostics-status")).toHaveText("Copied to the clipboard.");
    const copied = await page.evaluate(() => (window as unknown as { __copied: string }).__copied);
    expect(copied).toBe(await area.inputValue());
    expect(mock.calls("config")).toBe(config);
  });

  test("Support: when the browser refuses the clipboard, it points at the text box", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", {
        configurable: true,
        value: { writeText: async () => Promise.reject(new Error("denied")) },
      });
      document.execCommand = (() => false) as typeof document.execCommand;
    });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    await expect(page.getByLabel("Diagnostics text")).toHaveValue(/^mod_pagespeed diagnostics\n/);
    await page.getByRole("button", { name: "Copy diagnostics" }).click();
    await expect(page.getByTestId("diagnostics-status")).toContainText("select the text below");
  });

  test("Support: Download .txt saves the text that is shown", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    const area = page.getByLabel("Diagnostics text");
    await expect(area).toHaveValue(/^mod_pagespeed diagnostics\n/);
    const [download] = await Promise.all([
      page.waitForEvent("download"),
      page.getByRole("button", { name: "Download .txt" }).click(),
    ]);
    expect(download.suggestedFilename()).toMatch(/^mod_pagespeed-diagnostics-\d{4}-\d\d-\d\dT\d\d-\d\d-\d\dZ\.txt$/);
    const path = await download.path();
    expect(readFileSync(path, "utf8")).toBe(await area.inputValue());
    await expect(page.getByTestId("diagnostics-status")).toHaveText("Downloaded as a text file.");
  });

  test("Support: query strings are removed unless kept; the configuration only when included, secrets replaced", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      config: F.ok({
        config: "",
        effective_config:
          "Options\n  FileCachePath (afcp)                /var/cache/mod_pagespeed/\n" +
          "  DownstreamCachePurgeSecret (dcps)   hunter2-example",
        scope: "vhost",
        host: "www.example.test:80",
      }),
      message_history: F.ok({
        scope: "process",
        next: 1,
        messages: [{ severity: "error", message: "Fetch of https://www.example.test/a.css?session=abc123 failed" }],
      }),
    });
    await openConsole(page, "pagespeed_admin", "#/support");
    const area = page.getByLabel("Diagnostics text");
    await expect(area).toHaveValue(/a\.css\?\[query removed\] failed/);
    expect(await area.inputValue()).not.toContain("session=abc123");
    expect(await area.inputValue()).not.toContain("FileCachePath");

    const keep = page.getByLabel("Keep query strings");
    const config = page.getByLabel("Include the configuration");
    await expect(keep).not.toBeChecked();
    await expect(config).not.toBeChecked();

    await keep.check();
    await expect(area).toHaveValue(/a\.css\?session=abc123 failed/);
    await keep.uncheck();

    await config.check();
    await expect(area).toHaveValue(/FileCachePath \(afcp\) +\/var\/cache\/mod_pagespeed\//);
    const text = await area.inputValue();
    expect(text).toMatch(/DownstreamCachePurgeSecret \(dcps\) +\[redacted\]\n/);
    expect(text).not.toContain("hunter2-example");
    expect(text).toContain("- the values of options not known to be safe to share\n");
  });

  test("Support: where to send the bundle, next to the buttons", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    const where = page.getByTestId("diagnostics-where");
    await expect(where).toContainText("Where to send it");
    await expect(where.getByRole("link", { name: "support request" })).toHaveAttribute("href", "https://modpagespeed.com/support/");
    await expect(where.getByRole("link", { name: "troubleshooting guide" })).toHaveAttribute(
      "href",
      "https://modpagespeed.com/docs/troubleshooting/#how-to-get-more-diagnostic-information",
    );
  });
});
