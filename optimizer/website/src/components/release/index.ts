// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Barrel export for the release-aware component library that consumes the
// release manifest. MDX docs pages should prefer importing
// individual components directly so unused components don't pull in their
// `getRelease()` calls at build time.

export { default as Version } from './Version.astro';
export { default as CompatMatrix } from './CompatMatrix.astro';
export { default as NugetCmd } from './NugetCmd.astro';
export { default as XPageSpeedExample } from './XPageSpeedExample.astro';
export { default as ImageTag } from './ImageTag.astro';
export { default as VersionMap } from './VersionMap.astro';
