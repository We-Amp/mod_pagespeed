// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// About and Support. Self-contained (support/mock-admin.ts); run after build.sh.

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

  test("Support: Copy diagnostics copies a plain-text bundle, reading only this console's pages", async ({ page }) => {
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
    await expect(page.getByRole("heading", { name: "Diagnostics" })).toBeVisible();
    const before = {
      config: mock.calls("config"),
      health: mock.calls("v1/daemon/health"),
      messages: mock.calls("message_history"),
    };
    await page.getByRole("button", { name: "Copy diagnostics" }).click();
    await expect(page.getByTestId("diagnostics-status")).toHaveText("Copied to the clipboard.");
    const text = await page.evaluate(() => (window as unknown as { __copied: string }).__copied);
    expect(text.startsWith("mod_pagespeed diagnostics\n")).toBe(true);
    expect(text).toMatch(/^Module build: (\S+)\nConsole build: \1$/m);
    expect(text).toContain("Console: this host (www.example.test:80)\n");
    expect(text).toContain("Optimizer: 2.0.41 (52344ff)\n");
    expect(text).toContain("Configuration SHA-256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n");
    expect(text).toContain("[error] [Sat, 26 Sep 2026 10:04:00 GMT] [Error] [531] Fetch of https://www.example.test/a.css failed\n");
    expect(text).not.toContain("CycloneCache enabled");
    expect(mock.calls("config") - before.config).toBe(1);
    expect(mock.calls("v1/daemon/health") - before.health).toBe(1);
    expect(mock.calls("message_history") - before.messages).toBe(1);
  });

  test("Support: when the browser refuses the clipboard, the bundle is shown to copy by hand", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", {
        configurable: true,
        value: { writeText: async () => Promise.reject(new Error("denied")) },
      });
      document.execCommand = (() => false) as typeof document.execCommand;
    });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/support");
    await page.getByRole("button", { name: "Copy diagnostics" }).click();
    await expect(page.getByTestId("diagnostics-status")).toContainText("select the text below");
    const area = page.getByLabel("Diagnostics text");
    await expect(area).toHaveValue(/^mod_pagespeed diagnostics\n/);
    await expect(area).toHaveAttribute("readonly", "");
  });
});
