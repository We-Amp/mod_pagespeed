// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Views the server has turned off: a leaf refused with 403 (as a proxy in
// front of the console does) is a quiet explanation on its page, not an
// error: no error banner, no reconnect banner, no retry, a muted "off" mark
// in the sidebar, and no axe violation. Self-contained
// (support/mock-admin.ts); run after build.sh.

import AxeBuilder from "@axe-core/playwright";
import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Leaf, type Reply, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const ADMIN = "pagespeed_global_admin";
const FORBIDDEN: Reply = { status: 403, body: "Forbidden" };
const TITLE = "Not available on this console.";
const DETAIL = "The server administrator has turned this view off.";
const DOCS = "https://modpagespeed.com/docs/admin-console/";

function watch(page: Page) {
  const problems: string[] = [];
  page.on("pageerror", (e) => problems.push(e.message));
  page.on("console", (m) => {
    // The browser itself logs every refused request; that line is not the console's.
    if (m.type() === "error" && !m.text().startsWith("Failed to load resource")) problems.push(m.text());
  });
  return problems;
}

function replies(off: Leaf[], extra: Partial<Record<Leaf, Responder>> = {}): Partial<Record<Leaf, Responder>> {
  const out: Partial<Record<Leaf, Responder>> = { ...F.allReplies(), config: F.CONFIG_GLOBAL, ...extra };
  for (const leaf of off) out[leaf] = FORBIDDEN;
  return out;
}

async function expectCard(page: Page, testid: string) {
  const card = page.getByTestId(testid);
  await expect(card).toBeVisible();
  await expect(card).toContainText(TITLE);
  await expect(card).toContainText(DETAIL);
  await expect(card.getByRole("link")).toHaveAttribute("href", DOCS);
  await expect(page.getByTestId("connection-banner")).toHaveCount(0);
  await expect(page.getByRole("alert")).toHaveCount(0);
}

async function expectNoViolations(page: Page) {
  const wcag = await new AxeBuilder({ page }).withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"]).analyze();
  const structure = await new AxeBuilder({ page })
    .withRules(["heading-order", "page-has-heading-one", "landmark-one-main", "landmark-unique", "region"])
    .analyze();
  const report = [...wcag.violations, ...structure.violations].map(
    (v) => `${v.id}: ${v.nodes.map((n) => n.target.join(" ")).join(" | ")}`,
  );
  expect(report).toEqual([]);
}

const settle = (page: Page) => page.waitForTimeout(150);

async function runFor(page: Page, ms: number) {
  for (let t = 0; t < ms; t += 5_000) {
    await page.clock.runFor(5_000);
    await settle(page);
  }
}

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Turned-off views (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    test("Configuration", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["config"]));
      await openConsole(page, ADMIN, "#/configuration");
      await expectCard(page, "config-unavailable");
      await expect(page.getByRole("button", { name: "Refresh" })).toHaveCount(0);
      await expect(page.getByRole("link", { name: "Configuration" })).toHaveCount(1);
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });

    test("Logs, with both sources off", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["message_history", "v1/daemon/logs"]));
      await openConsole(page, ADMIN, "#/logs");
      await expectCard(page, "logs-unavailable");
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });

    test("Logs, with only the optimizer's log off", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["v1/daemon/logs"], { message_history: F.MESSAGES }));
      await openConsole(page, ADMIN, "#/logs");
      await expect(page.getByTestId("logs-optimizer-off")).toBeVisible();
      await expect(page.getByTestId("logs-unavailable")).toHaveCount(0);
      await expect(page.getByTestId("log-group").first()).toBeVisible();
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });

    test("URLs", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["v1/daemon/cache/urls"]));
      await openConsole(page, ADMIN, "#/urls");
      await expectCard(page, "urls-unavailable");
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });

    test("Rule packs", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["rule_packs"]));
      await openConsole(page, ADMIN, "#/rule-packs");
      await expectCard(page, "rule-packs-unavailable");
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });

    test("Overview: the findings card that reads the message log", async ({ page }) => {
      const problems = watch(page);
      await mockAdmin(page, ADMIN, replies(["message_history"]));
      await openConsole(page, ADMIN, "#/overview");
      await expectCard(page, "overview-log-unavailable");
      await expect(page.getByTestId("health-summary")).toHaveText(
        "Findings from the message log are not available on this console",
      );
      await expectNoViolations(page);
      expect(problems).toEqual([]);
    });
  });
}

test("the sidebar keeps a turned-off page, marked off", async ({ page }) => {
  await mockAdmin(page, ADMIN, replies(["message_history", "v1/daemon/logs", "config"]));
  await openConsole(page, ADMIN, "#/logs");
  await expectCard(page, "logs-unavailable");
  const nav = page.getByRole("navigation", { name: "Console pages" });
  const logs = nav.getByRole("link", { name: "Logs" });
  await expect(logs).toBeVisible();
  await expect(logs).toHaveAccessibleDescription("off");
  // Configuration is marked once its leaf has been refused.
  await nav.getByRole("link", { name: "Configuration" }).click();
  await expectCard(page, "config-unavailable");
  await expect(nav.getByRole("link", { name: "Configuration" })).toHaveAccessibleDescription("off");
  // A page whose leaves answer carries no mark.
  await expect(nav.getByRole("link", { name: "Statistics" })).not.toHaveAccessibleDescription("off");
});

test("a turned-off leaf is asked once, then never again", async ({ page }) => {
  await page.clock.install();
  const admin = await mockAdmin(page, ADMIN, replies(["rule_packs", "message_history", "v1/daemon/logs"]));
  await openConsole(page, ADMIN, "#/rule-packs");
  await expectCard(page, "rule-packs-unavailable");
  await runFor(page, 60_000);
  expect(admin.calls("rule_packs")).toBe(1);
  await page.goto(`http://console.test/${ADMIN}/#/logs`);
  await expectCard(page, "logs-unavailable");
  await runFor(page, 60_000);
  expect(admin.calls("message_history")).toBe(1);
  expect(admin.calls("v1/daemon/logs")).toBe(1);
});

test("a leaf that answers 500 is still an error, not 'off'", async ({ page }) => {
  await mockAdmin(page, ADMIN, replies([], { config: { status: 500, body: { error: "config unavailable" } } }));
  await openConsole(page, ADMIN, "#/configuration");
  await expect(page.getByRole("alert").filter({ hasText: "config unavailable" })).toBeVisible();
  await expect(page.getByTestId("config-unavailable")).toHaveCount(0);
});
