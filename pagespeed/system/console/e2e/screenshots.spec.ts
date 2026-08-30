import { test } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

const pages = [
  { hash: "#/statistics", name: "statistics" },
  { hash: "#/configuration", name: "configuration" },
  { hash: "#/histograms", name: "histograms" },
  { hash: "#/caches", name: "caches" },
  { hash: "#/console", name: "console" },
  { hash: "#/messages", name: "messages" },
  { hash: "#/graphs", name: "graphs" },
  { hash: "#/daemon/status", name: "daemon-status" },
  { hash: "#/daemon/cache", name: "daemon-cache" },
  { hash: "#/daemon/back-pressure", name: "daemon-back-pressure" },
  { hash: "#/support", name: "support" },
  { hash: "#/about", name: "about" },
];

for (const p of pages) {
  test(`screenshot ${p.name}`, async ({ page }) => {
    await page.goto(`${BASE}${p.hash}`);
    await page.waitForTimeout(3000);
    await page.screenshot({
      path: `e2e/screenshots/${p.name}.png`,
      fullPage: true,
    });
  });
}
