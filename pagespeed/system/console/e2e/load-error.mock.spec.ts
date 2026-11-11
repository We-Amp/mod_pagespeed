// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A failed first load explains itself: a title in words, a line on the
// likely cause, "Try again", a documentation link, and the transport
// message folded away -- never "HTTP 404: ..." as the page's text.
// Self-contained (support/mock-admin.ts); run after build.sh.

import AxeBuilder from "@axe-core/playwright";
import { expect, test, type Locator, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import * as R from "./support/rule-packs-fixtures";
import { mockAdmin, openConsole, type Admin, type Leaf, type Reply, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const DOCS = "https://modpagespeed.com/docs/admin-console/";
const DETAIL_HASH = "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https";

const unknownPage = (leaf: string): Reply => ({ status: 404, body: { error: `Unknown admin page: ${leaf}` } });
const SERVER_ERROR: Reply = { status: 500, body: { error: "boom" } };

const NOT_FOUND = "The server does not know this view.";
const SERVER = "The server reported an error while answering.";

interface Case {
  name: string;
  admin: Admin;
  hash: string;
  leaf: Leaf;
  failure: Reply;
  /** What the leaf answers once it works. */
  recovered: Reply;
  title: string;
  cause: string;
  raw: string;
  testid?: string;
  /** Visible once the read succeeds. */
  loaded: (page: Page) => Locator;
}

const CASES: Case[] = [
  {
    name: "Statistics",
    admin: "pagespeed_admin",
    hash: "#/statistics",
    leaf: "stats_json",
    failure: unknownPage("stats_json"),
    recovered: F.STATS_VHOST,
    title: "Could not load the statistics.",
    cause: NOT_FOUND,
    raw: "HTTP 404: Unknown admin page: stats_json",
    loaded: (page) => page.locator(".stat-group").first(),
  },
  {
    name: "Histograms",
    admin: "pagespeed_admin",
    hash: "#/histograms",
    leaf: "histograms",
    failure: SERVER_ERROR,
    recovered: F.HISTOGRAMS,
    title: "Could not load the histograms.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.locator("main table").first(),
  },
  {
    name: "Caches",
    admin: "pagespeed_admin",
    hash: "#/caches",
    leaf: "cache",
    failure: SERVER_ERROR,
    recovered: F.CACHE_STRUCTURE,
    title: "Could not load the cache structure.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.locator(".cache-list"),
  },
  {
    name: "Logs",
    admin: "pagespeed_admin",
    hash: "#/logs",
    leaf: "message_history",
    failure: SERVER_ERROR,
    recovered: F.MESSAGES,
    title: "Could not load the logs.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.getByTestId("log-group").first(),
  },
  {
    name: "Rule packs",
    admin: "pagespeed_admin",
    hash: "#/rule-packs",
    leaf: "rule_packs",
    failure: unknownPage("rule_packs"),
    recovered: R.ONE,
    title: "Could not load the rule packs.",
    cause: NOT_FOUND,
    raw: "HTTP 404: Unknown admin page: rule_packs",
    loaded: (page) => page.getByTestId("rp-config"),
  },
  {
    name: "Configuration",
    admin: "pagespeed_admin",
    hash: "#/configuration",
    leaf: "config",
    failure: SERVER_ERROR,
    recovered: F.CONFIG_VHOST_WITH_TEXT,
    title: "Could not load the configuration.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.locator(".config-block"),
  },
  {
    name: "URLs",
    admin: "pagespeed_global_admin",
    hash: "#/urls",
    leaf: "v1/daemon/cache/urls",
    failure: SERVER_ERROR,
    recovered: F.URLS_EMPTY,
    title: "Could not load the URL list.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.getByTestId("urls-empty"),
  },
  {
    name: "URL detail",
    admin: "pagespeed_global_admin",
    hash: DETAIL_HASH,
    leaf: "v1/daemon/cache/alternates",
    failure: SERVER_ERROR,
    recovered: F.ALTERNATES_HERO,
    title: "Could not load this URL's variants.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.getByTestId("url-detail-url"),
  },
  {
    name: "Optimizer status, health",
    admin: "pagespeed_global_admin",
    hash: "#/optimizer",
    leaf: "v1/daemon/health",
    failure: SERVER_ERROR,
    recovered: F.HEALTH_OK,
    title: "Could not load the optimizer's health.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.locator("#optimizer-health .info-grid").first(),
  },
  {
    name: "Optimizer status, statistics",
    admin: "pagespeed_global_admin",
    hash: "#/optimizer",
    leaf: "v1/daemon/stats",
    failure: SERVER_ERROR,
    recovered: F.DAEMON_STATS,
    title: "Could not load the optimizer's statistics.",
    cause: SERVER,
    raw: "HTTP 500: boom",
    loaded: (page) => page.locator("#optimizer-load .info-grid").first(),
  },
  {
    name: "Overview, module card",
    admin: "pagespeed_admin",
    hash: "#/overview",
    leaf: "stats_json",
    failure: unknownPage("stats_json"),
    recovered: F.STATS_VHOST,
    title: "Could not load the module statistics.",
    cause: NOT_FOUND,
    raw: "HTTP 404: Unknown admin page: stats_json",
    testid: "module-error",
    loaded: (page) => page.getByTestId("module-bytes-saved"),
  },
];

function repliesFor(c: Case, responder: Responder): Partial<Record<Leaf, Responder>> {
  const config = c.admin === "pagespeed_global_admin" ? F.CONFIG_GLOBAL : F.CONFIG_VHOST;
  return { ...F.allReplies(), config, [c.leaf]: responder };
}

/** The text of `main` as a reader sees it with every <details> closed. */
async function textOutsideDetails(page: Page): Promise<string> {
  return page.locator("main").evaluate((main) => {
    const copy = main.cloneNode(true) as HTMLElement;
    for (const details of copy.querySelectorAll("details")) {
      for (const child of [...details.childNodes]) {
        if (!(child instanceof HTMLElement && child.tagName === "SUMMARY")) child.remove();
      }
    }
    return copy.textContent ?? "";
  });
}

for (const c of CASES) {
  test(`${c.name}: a failed first load says what failed, and Try again asks again`, async ({ page }) => {
    // Timers stand still: only the button can cause the second read.
    await page.clock.install();
    let failing = true;
    const mock = await mockAdmin(page, c.admin, repliesFor(c, () => (failing ? c.failure : c.recovered)));
    await openConsole(page, c.admin, c.hash);

    const notice = page.getByTestId(c.testid ?? "load-error");
    await expect(notice).toBeVisible();
    await expect(notice).toHaveRole("alert");
    await expect(notice.locator(".notice-title")).toHaveText(c.title);
    await expect(notice.locator(".notice-body")).toContainText(c.cause);
    await expect(notice.getByRole("link", { name: "About the admin console" })).toHaveAttribute("href", DOCS);

    // The transport message is folded away, not the page's text.
    const details = notice.locator("details");
    await expect(details).not.toHaveAttribute("open", "");
    await expect(details.locator("code")).toBeHidden();
    expect(await textOutsideDetails(page)).not.toMatch(/HTTP \d{3}/);
    await details.locator("summary").click();
    await expect(details.locator("code")).toHaveText(c.raw);

    const before = mock.calls(c.leaf);
    failing = false;
    await notice.getByRole("button", { name: "Try again" }).click();
    await expect.poll(() => mock.calls(c.leaf)).toBeGreaterThan(before);
    await expect(c.loaded(page)).toBeVisible();
    await expect(page.getByTestId(c.testid ?? "load-error")).toHaveCount(0);
  });
}

test("Try again that fails again keeps the explanation", async ({ page }) => {
  await page.clock.install();
  const mock = await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: unknownPage("stats_json") });
  await openConsole(page, "pagespeed_admin", "#/statistics");
  const notice = page.getByTestId("load-error");
  await expect(notice).toBeVisible();
  const before = mock.calls("stats_json");
  const button = notice.getByRole("button", { name: "Try again" });
  await button.click();
  await expect.poll(() => mock.calls("stats_json")).toBeGreaterThan(before);
  await expect(notice.locator(".notice-title")).toHaveText("Could not load the statistics.");
  await expect(button).toBeVisible();
  await expect(button).toBeFocused();
});

test("a refused read names the refusal", async ({ page }) => {
  // The module's own refusal carries a reason, so it is not the quiet "turned off" card.
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    histograms: { status: 403, body: { error: "not allowed here" } },
  });
  await openConsole(page, "pagespeed_admin", "#/histograms");
  const notice = page.getByTestId("load-error");
  await expect(notice.locator(".notice-title")).toHaveText("Could not load the histograms.");
  await expect(notice.locator(".notice-body")).toContainText("The server refused this request.");
  expect(await textOutsideDetails(page)).not.toMatch(/HTTP \d{3}/);
});

test("Configuration without any text says so, with a way on", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST });
  await openConsole(page, "pagespeed_admin", "#/configuration");
  const empty = page.getByTestId("config-empty");
  await expect(empty).toContainText("The server sent no configuration text.");
  await expect(empty.getByRole("link", { name: "About the admin console" })).toHaveAttribute("href", DOCS);
  // Nothing failed: no alert, and the header's Refresh stays the way to ask again.
  await expect(page.getByRole("alert")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Refresh" })).toBeVisible();
});

for (const scheme of ["light", "dark"] as const) {
  test.describe(`load error (${scheme})`, () => {
    test.use({ colorScheme: scheme });

    test("has no accessibility violations, closed or open", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), rule_packs: unknownPage("rule_packs") });
      await openConsole(page, "pagespeed_admin", "#/rule-packs");
      const notice = page.getByTestId("load-error");
      await expect(notice).toBeVisible();
      for (const open of [false, true]) {
        if (open) await notice.locator("summary").click();
        const wcag = await new AxeBuilder({ page }).withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"]).analyze();
        expect(wcag.violations.map((v) => `${v.id}: ${v.nodes.map((n) => n.target.join(" ")).join(" | ")}`)).toEqual([]);
      }
    });
  });
}
