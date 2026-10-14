// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Rule packs page over the module's JSON status, every state mocked end
// to end: zero, one and two packs, a pack not read yet (polled again), a
// refused pack, a duplicate id, overlap warnings, the decision tail's
// filters and its pack selector, and the host lens. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import * as R from "./support/rule-packs-fixtures";
import { mockAdmin, openConsole, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const replies = (rule_packs: Responder, config = F.CONFIG_VHOST) => ({ ...F.allReplies(), config, rule_packs });

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "Rule packs" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

test.describe("the packs", () => {
  test("no pack file configured says so plainly", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(R.ZERO));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-none-configured")).toContainText("No rule pack is configured");
    await expect(page.getByTestId("rp-filter-enabled")).toHaveText("off");
    await expect(page.getByTestId("rp-pack")).toHaveCount(0);
    await expect(page.getByTestId("rp-not-loaded-yet")).toHaveCount(0);
    await expect(page.getByTestId("rp-counters")).toContainText("reports no rule pack counters");
  });

  test("one pack: configuration first, then the pack, the counters and the decisions", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(R.ONE));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    const headings = await page.locator("main h2").allTextContents();
    expect(headings).toEqual(["Configuration", "Packs", "Counters", "Decisions"]);
    await expect(page.getByTestId("rp-filter-enabled")).toHaveText("on");
    await expect(page.getByTestId("rp-mode")).toHaveText("report");
    await expect(page.getByTestId("rp-files")).toContainText("/etc/packs/edge.json");
    const pack = page.getByTestId("rp-pack");
    await expect(pack).toHaveCount(1);
    await expect(pack).toContainText("edge-seo");
    await expect(pack).toContainText("0.1.0");
    await expect(pack).toContainText("Example Publisher");
    await expect(pack).toContainText("not verified");
    await expect(pack.locator(".state-badge")).toHaveText("loaded");
    await expect(page.getByTestId("rp-counter")).toHaveCount(3);
    await expect(page.getByTestId("rp-counter").first()).toContainText("actions_applied");
    await expect(page.getByTestId("rp-decision")).toHaveCount(3);
    // Newest first, as the module sends them; a hostile path stays text.
    await expect(page.getByTestId("rp-decision").first()).toContainText("/<script>alert(1)</script>");
    await expect(page.locator("main script")).toHaveCount(0);
    await expect(page.getByTestId("rp-decision").last()).toContainText("missing by edge-seo");
  });

  test("asks for the server's largest tail", async ({ page }) => {
    const seen: string[] = [];
    await mockAdmin(page, "pagespeed_admin", replies((_c, req) => (seen.push(req.url.search), R.ONE)));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    expect(seen[0]).toContain("limit=1000");
    expect(seen[0]).toContain("json=1");
  });

  test("two packs in load order, with a refused file, a duplicate id, a licence and overlap warnings", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(R.TWO_BY_PACK));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    const packs = page.getByTestId("rp-pack");
    await expect(packs).toHaveCount(4);
    await expect(packs.nth(0)).toHaveAttribute("data-pack-id", "edge-seo");
    await expect(packs.nth(1)).toHaveAttribute("data-pack-id", "house-rules");
    await expect(packs.nth(1)).toContainText("verified with key example-key");
    await expect(page.getByTestId("rp-license")).toContainText("Example licence, expires 2027-01-01; see the publisher");
    await expect(packs.nth(1).getByTestId("rp-pack-warnings")).toContainText("deprecated kind");
    await expect(packs.nth(2).locator(".state-badge")).toHaveText("duplicate id");
    await expect(packs.nth(3).locator(".state-badge")).toHaveText("refused");
    await expect(packs.nth(3).getByTestId("rp-pack-error")).toContainText("signature does not verify");
    await expect(page.getByTestId("rp-overlap-warnings")).toContainText("title owned by edge-seo");
  });

  test("a file the server has not read yet is shown as starting and asked for again", async ({ page }) => {
    await page.clock.install();
    await mockAdmin(page, "pagespeed_admin", replies((call) => (call === 0 ? R.NOT_READ : R.ONE)));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-not-loaded-yet")).toBeVisible();
    await expect(page.getByTestId("rp-none-configured")).toHaveCount(0);
    await expect(page.getByTestId("rp-pack").locator(".state-badge")).toHaveText("not read yet");
    await page.clock.runFor(5_000);
    await expect(page.getByTestId("rp-not-loaded-yet")).toHaveCount(0);
    await expect(page.getByTestId("rp-pack").locator(".state-badge")).toHaveText("loaded");
  });
});

test.describe("the decisions", () => {
  test("filters by pack, action, reason and rule, and clears them", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(R.TWO_BY_PACK));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    const rows = page.getByTestId("rp-decision");
    await expect(rows).toHaveCount(3);
    await page.getByTestId("rp-filter-action").selectOption("none");
    await expect(rows).toHaveCount(1);
    await expect(rows.first()).toContainText("title-keep");
    await expect(page.getByTestId("rp-count")).toHaveText("Showing 1 of 3");
    await page.getByTestId("rp-clear").click();
    await expect(rows).toHaveCount(3);
    await page.getByTestId("rp-filter-reason").selectOption("present");
    await expect(rows).toHaveCount(1);
    await page.getByTestId("rp-clear").click();
    await page.getByTestId("rp-filter-rule").fill("CANON");
    await expect(rows).toHaveCount(2);
    await page.getByTestId("rp-filter-pack").selectOption("edge-seo");
    await expect(rows).toHaveCount(2);
    await page.getByTestId("rp-filter-rule").fill("nothing-like-this");
    await expect(page.getByTestId("rp-tail-none-match")).toBeVisible();
    await expect(page.getByTestId("rp-decisions")).toHaveCount(0);
  });

  test("the selector loads another pack's log and the address carries it", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", replies(R.TWO_BY_PACK));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-decision")).toHaveCount(3);
    await page.getByTestId("rp-tail-pack").getByRole("combobox").selectOption("house-rules");
    await expect(page).toHaveURL(/#\/rule-packs\?pack=house-rules$/);
    await expect(page.getByTestId("rp-decision")).toHaveCount(1);
    await expect(page.getByTestId("rp-decision").first()).toContainText("robots-meta");
    expect(mock.calls("rule_packs")).toBeGreaterThanOrEqual(2);
    await expect(page.getByTestId("rp-raw-log")).toHaveAttribute("href", "/pagespeed_admin/rule_packs?raw=1&pack=house-rules");
    await expect(page.getByTestId("rp-server-page")).toHaveAttribute("href", "/pagespeed_admin/rule_packs?pack=house-rules");
  });

  test("raw log and server page links point at the console that is open", async ({ page }) => {
    await mockAdmin(page, "renamed_admin", replies(R.ONE));
    await openConsole(page, "renamed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-raw-log")).toHaveAttribute("href", "/renamed_admin/rule_packs?raw=1&pack=edge-seo");
    await expect(page.getByTestId("rp-server-page")).toHaveAttribute("href", "/renamed_admin/rule_packs?pack=edge-seo");
  });

  const NOTES: Array<[string, string]> = [
    ["missing", "No decision log exists"],
    ["log_dir_not_set", "LogDir is not set"],
    ["error", "could not be read: permission denied"],
  ];
  for (const [status, text] of NOTES) {
    test(`a ${status} decision log is explained`, async ({ page }) => {
      await mockAdmin(
        page,
        "pagespeed_admin",
        replies(F.ok(R.status({ packs: [R.PACK_EDGE], pack: "edge-seo", status, error: status === "error" ? "permission denied" : null }))),
      );
      await openConsole(page, "pagespeed_admin", "#/rule-packs");
      await ready(page);
      await expect(page.getByTestId("rp-tail-note")).toContainText(text);
    });
  }

  test("an empty log reads as 'no decisions yet'", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(F.ok(R.status({ packs: [R.PACK_EDGE], pack: "edge-seo", rows: [] }))));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-tail-empty")).toBeVisible();
  });
});

test.describe("the console around it", () => {
  test("a status the module cannot give is a load error, not an empty page", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies({ status: 404, body: { error: "Unknown admin page: rule_packs" } }));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await expect(page.getByRole("heading", { level: 1, name: "Rule packs" })).toBeVisible();
    await expect(page.getByTestId("rp-config")).toHaveCount(0);
    await expect(page.getByRole("alert")).toBeVisible();
  });

  test("the host lens narrows the whole-server console's decisions to one host", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", replies(R.GLOBAL_TWO, F.CONFIG_GLOBAL));
    await openConsole(page, "pagespeed_global_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("rp-decision")).toHaveCount(3);
    await page.getByTestId("host-lens").getByRole("combobox", { name: "Host" }).selectOption("cdn.example.test");
    await expect(page.getByTestId("rp-decision")).toHaveCount(1);
    await expect(page.getByTestId("rp-lens")).toContainText("cdn.example.test");
    await expect(page.getByTestId("lens-note")).toHaveCount(0);
  });

  test("a per-host console has no host lens control and shows every row it is given", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", replies(R.ONE));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    await expect(page.getByTestId("host-lens")).toHaveCount(0);
    await expect(page.getByTestId("rp-decision")).toHaveCount(3);
  });

  test("fits a phone without sideways page scroll", async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 800 });
    await mockAdmin(page, "pagespeed_admin", replies(R.TWO_BY_PACK));
    await openConsole(page, "pagespeed_admin", "#/rule-packs");
    await ready(page);
    const fits = await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1);
    expect(fits).toBe(true);
  });
});
