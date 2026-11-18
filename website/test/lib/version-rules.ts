// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Which version each artifact of a release may carry, from 2.5.0 on. One
// number names the release; the single exception is a module-only release,
// which publishes new module packages (apt, yum, the IIS installer) without a
// new product release. The module packages may then lead the release by
// patch versions on the same major.minor line. They never trail it, and they
// never move to another line. The container images, the NuGet packages, the
// Sidecar package and the Helm chart's appVersion stay on the release.
//
// The mirror case is an optimizer-only release (for example a 2.9.1 whose
// optimizer host packages move while the module, the images, the NuGet
// packages and the chart stay at the paired release 2.9.0). The row's release
// is then the paired release, and `optimizer` leads it by patch versions on the
// same line. The rule applies only when the module packages, the images, the
// NuGet packages, the Sidecar package and the chart's appVersion all equal that
// paired release; any one of them moving as well is an error, never a way to
// ship a different kind of release. A release can lead with the module or with
// the optimizer, not both.
//
// Pure functions, so test/version-map.test.ts can prove both the accepted
// module-only case and the rejected ones without editing the real data.

/** Numeric x.y.z comparison: negative, zero or positive. */
export function compareVersions(a: string, b: string): number {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

const line = (v: string) => v.split('.').slice(0, 2).join('.');

export interface ArtifactVersions {
  release: string;
  packages: string;
  images: string | null;
  nuget: string;
  /** The Helm chart's appVersion, when the row is the current release. */
  chartAppVersion?: string;
  /** The Sidecar NuGet package version, when the row is the current release. */
  sidecar?: string;
  /**
   * The optimizer host packages (apt, yum, the Windows zip), when they lead the
   * release: an optimizer-only release. Left out when they carry the release.
   */
  optimizer?: string;
}

/** Every rule the versions break; empty when they agree. */
export function artifactVersionProblems(v: ArtifactVersions): string[] {
  const problems: string[] = [];
  const onRelease = (what: string, got: string | null | undefined) => {
    if (got !== undefined && got !== v.release) {
      problems.push(`${v.release}: ${what} ${got} must equal the release`);
    }
  };
  onRelease('images', v.images);
  onRelease('nuget', v.nuget);
  onRelease('chart appVersion', v.chartAppVersion);
  onRelease('sidecar', v.sidecar);
  if (!/^\d+\.\d+\.\d+$/.test(v.packages)) {
    problems.push(`${v.release}: packages ${v.packages} is not x.y.z`);
  } else if (line(v.packages) !== line(v.release)) {
    problems.push(`${v.release}: packages ${v.packages} must stay on the ${line(v.release)} line`);
  } else if (compareVersions(v.packages, v.release) < 0) {
    problems.push(`${v.release}: packages ${v.packages} must not trail the release`);
  }
  if (v.optimizer !== undefined) {
    if (!/^\d+\.\d+\.\d+$/.test(v.optimizer)) {
      problems.push(`${v.release}: optimizer ${v.optimizer} is not x.y.z`);
    } else if (line(v.optimizer) !== line(v.release)) {
      problems.push(
        `${v.release}: optimizer ${v.optimizer} must stay on the ${line(v.release)} line`,
      );
    } else if (compareVersions(v.optimizer, v.release) < 0) {
      problems.push(`${v.release}: optimizer ${v.optimizer} must not trail the release`);
    } else if (compareVersions(v.optimizer, v.release) > 0 && v.packages !== v.release) {
      // Images, NuGet, the Sidecar and the chart are held to the release above.
      problems.push(
        `${v.release}: packages ${v.packages} must equal the release when the optimizer leads`,
      );
    }
  }
  return problems;
}
