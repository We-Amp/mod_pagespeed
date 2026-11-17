// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// A release cut in flight: the release prep pull request has bumped
// VERSION.txt and the Helm chart (chart appVersion = VERSION.txt), but the
// site's version map still starts with the previous release. The site's new
// row lands with the release's own site pull request, after the packages, the
// images and the chart are published. Until then the row-vs-chart check is a
// warning. Pure, from committed files only (no network).

import { compareVersions } from './version-rules';

export interface CutInFlightInput {
  /** VERSION.txt of the repository. */
  version: string;
  /** The site's current (first) version-map row. */
  rowRelease: string;
  /** The row's Helm chart version; null when the row ships no chart. */
  rowHelmChart: string | null;
  /** Chart.yaml version and appVersion. */
  chartVersion: string;
  chartAppVersion: string;
}

/**
 * True only when the chart already ships VERSION.txt, the site's row is
 * strictly behind it, and the chart version is strictly newer than the row's (a VERSION.txt bump always
 * ships a new chart version).
 * Strict (false) when the chart's appVersion disagrees with VERSION.txt, the
 * row is at or ahead of VERSION.txt, or the chart version is not ahead of the row.
 */
export function releaseCutInFlight(i: CutInFlightInput): boolean {
  // Without a chart on the row, chart vs row cannot be judged: stay strict.
  if (i.rowHelmChart === null) return false;
  return (
    i.chartAppVersion === i.version &&
    compareVersions(i.version, i.rowRelease) > 0 &&
    compareVersions(i.chartVersion, i.rowHelmChart) > 0
  );
}
