import { defineConfig } from "vitest/config";

// Vitest config for the admin-console unit tests.
//
// Without this, `vitest` auto-discovers **/*.{test,spec}.* and tries to run
// the Playwright end-to-end specs under e2e/ (which import @playwright/test,
// not vitest) — failing with loader errors. Exclude e2e/ so vitest only owns
// real unit tests under src/. There are none yet, so passWithNoTests keeps
// `npm test` green; add src/**/*.test.ts and they are picked up automatically.
export default defineConfig({
  test: {
    include: ["src/**/*.{test,spec}.{js,ts}"],
    exclude: ["e2e/**", "node_modules/**", "dist/**"],
    passWithNoTests: true,
  },
});
