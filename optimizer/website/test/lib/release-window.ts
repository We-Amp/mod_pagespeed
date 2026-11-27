// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Release-window tolerance for the tests that compare the repository with
// LIVE release state (the package index, the Helm chart that ships with a
// release pull request). website.yml sets RELEASE_WINDOW=1 for the
// `auto/sync-release-manifests` and `release/*` pull requests
// (scripts/release-window.sh); there a mismatch is a warning, because that
// pull request is the one half of a two-step release and the other half
// (the site's version map and package version) lands next. Everywhere else,
// main pushes and ordinary pull requests included, a mismatch fails.

export const inReleaseWindow = (env: NodeJS.ProcessEnv = process.env): boolean =>
  env.RELEASE_WINDOW === '1';

/**
 * Run `assertions`. A failure throws, except in a release window, where it is
 * reported as a warning (an annotation on CI) and the test passes.
 */
export function expectOrWarn(
  what: string,
  assertions: () => void,
  windowOn: boolean = inReleaseWindow(),
): void {
  try {
    assertions();
  } catch (err) {
    if (!windowOn) throw err;
    const msg = err instanceof Error ? err.message : String(err);
    console.warn(
      `::warning::${what}: ${msg} -- warning only: a release is in progress (a sync or release/* pull request, or a release cut in flight); outside a release the same mismatch fails.`,
    );
  }
}
