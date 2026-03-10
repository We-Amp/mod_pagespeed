import { test } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

test("screenshot console large", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto(`${BASE}#/console`);
  await page.waitForTimeout(5000);
  await page.screenshot({
    path: "e2e/screenshots/console-large.png",
    fullPage: true,
  });
});
