# PageSpeed Workbench

Management console for mod_pagespeed — a pnpm monorepo with 4 packages.

## Package Structure

| Package | Purpose | Entry |
|---------|---------|-------|
| `packages/api-client` | TypeScript API client, transport abstraction, types | `src/index.ts` |
| `packages/ui` | Shared CSS (theme, base) — no Svelte components yet | `src/index.ts` |
| `packages/web-shell` | SvelteKit web console (7 routes, 22 components) | `src/routes/` |
| `packages/vscode-shell` | VS Code extension (TreeViews, commands, placeholder webviews) | `src/extension.ts` |

## Build & Test

```bash
cd tools/workbench
pnpm install                          # Install all deps
pnpm test                             # Run all tests (478 TS across 18 files)
pnpm -C packages/api-client test      # api-client only (168 tests)
pnpm -C packages/web-shell test       # web-shell only (310 tests)
pnpm -C packages/web-shell dev        # Dev server (Vite hot reload)
pnpm build                            # Build all packages
```

### Playwright E2E Tests

Require the workbench-demo Docker stack running (origin :8081, nginx :8084,
worker :9880) plus the Vite dev server (:5173).

```bash
cd tools/workbench/packages/web-shell
npx playwright test                   # Run all E2E tests (14 files)
npx playwright test waterfall         # Waterfall viewer tests only
npx playwright test --headed          # Watch in browser
```

Test files in `tests/e2e/`: audit, auth, config, console, critical-css, demo-sites,
diff, error-states, exports, logs, metrics, navigation, reconnection, urls, waterfall.
Capture-heavy tests use `test.slow()` (3x timeout). URLs passed to capture endpoints
must be reachable from inside Docker (e.g., `http://nginx:8080/...`).

## Key Design Decisions

- **Transport abstraction**: `ApiTransport` interface with `DirectTransport` (HTTP/WS)
  and `VsCodeTransport` (postMessage bridge). All API calls go through this.
- **CSS custom properties**: `--ps-*` variables with `--vscode-*` fallbacks for theming.
  No external UI toolkit dependency.
- **Svelte 5 runes mode**: All components use `$state`, `$derived`, `$effect`.
- **SSR safety**: All browser APIs guarded with `typeof window/document !== 'undefined'`.
- **uPlot for charts**: Lightweight (35KB) time-series library on the dashboard.

## API Connection

The web console connects to the worker's HTTP API (default `http://localhost:9880`).
- Auth: Bearer token via `PAGESPEED_API_TOKEN` env var on worker
- WebSocket: `/v1/ws/stats` (live metrics), `/v1/ws/events` (real-time events), `/v1/ws/logs` (log streaming)
- All 23 API endpoints documented in the workbench design document, Section 4.2

## Known Gaps

- Shared Svelte components not extracted to `packages/ui/` (all in `web-shell`)
- VS Code webview panels render placeholder HTML (bridge is built, rendering not connected)
- No VS Code extension tests
