export interface BasePathInfo {
  basePath: string;
  isGlobal: boolean;
}

/**
 * Detect the base path for API requests.
 *
 * The admin console can be served at either:
 * - /pagespeed_admin/  (per-vhost admin)
 * - /pagespeed_global_admin/  (global admin)
 *
 * In development mode (Vite dev server), the base path is empty
 * and the dev proxy forwards requests to the backend.
 */
export function detectBasePath(): BasePathInfo {
  const path = window.location.pathname;

  if (path.startsWith("/pagespeed_global_admin")) {
    return { basePath: "/pagespeed_global_admin", isGlobal: true };
  }

  if (path.startsWith("/pagespeed_admin")) {
    return { basePath: "/pagespeed_admin", isGlobal: false };
  }

  // Dev mode: proxy handles routing, no base path needed.
  return { basePath: "", isGlobal: false };
}
