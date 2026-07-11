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

  // Derive the base from the ACTUAL serving path, not the two built-in
  // defaults. The admin endpoint is operator-configurable (Apache
  // ModPagespeedAdminPath, nginx/IIS equivalents), and the documented way to
  // hide it is to rename it. The SPA is served at "<adminPath>/" (optionally
  // "<adminPath>/console"); the JSON API lives directly under "<adminPath>".
  // Strip a trailing slash and an optional trailing "console" segment so a
  // custom path like /secret-admin resolves its API calls correctly instead of
  // firing them at the site root (which 404s or returns rewritten site HTML).
  const basePath = path.replace(/\/(console\/?)?$/, "");

  // isGlobal keys on the conventional name for the built-in paths. For a custom
  // GlobalAdminPath this heuristic can't tell; the backend's `is_global` field
  // in the license status response is authoritative where the UI needs it.
  const isGlobal = /(^|\/)pagespeed_global_admin(\/|$)/.test(path);

  // Dev mode (Vite proxy) lands here with basePath "".
  return { basePath, isGlobal };
}
