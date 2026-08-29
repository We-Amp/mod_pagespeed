import { defineConfig } from "vitest/config";
import { fileURLToPath } from "node:url";

// Vitest config for the admin-console unit tests.
//
// Without this, `vitest` auto-discovers **/*.{test,spec}.* and tries to run
// the Playwright end-to-end specs under e2e/ (which import @playwright/test,
// not vitest) — failing with loader errors. Exclude e2e/ so vitest only owns
// real unit tests under src/; add src/**/*.test.ts and they are picked up
// automatically. passWithNoTests keeps `npm test` green if they all go away.
export default defineConfig({
  // Mirror the $lib alias from vite.config.ts so unit tests resolve the same
  // module specifiers the app uses.
  resolve: {
    alias: {
      $lib: fileURLToPath(new URL("./src/lib", import.meta.url)),
    },
  },
  test: {
    include: ["src/**/*.{test,spec}.{js,ts}"],
    exclude: ["e2e/**", "node_modules/**", "dist/**"],
    passWithNoTests: true,
  },
});
