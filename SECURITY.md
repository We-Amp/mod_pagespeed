# Security policy

## Reporting a vulnerability

If you discover a security vulnerability in mod_pagespeed, please report it
privately in one of these ways:

- **GitHub private vulnerability reporting:** open the
  [Security tab](https://github.com/We-Amp/pagespeed-optimizer/security) of this
  repository and choose **Report a vulnerability**, or go straight to the
  [report form](https://github.com/We-Amp/pagespeed-optimizer/security/advisories/new).
- **Email:** **security@modpagespeed.com**

Please do not open a public issue or pull request for a vulnerability. Both
are visible to everyone before a fix is available.

We will acknowledge your report and work with you to understand and address
the issue. We keep you informed as we work on it and tell you when
the fix is released, with the version that carries it.

We do not publish a PGP key for reports. GitHub private vulnerability
reporting is the encrypted channel; if you need another one, ask by email and
we will arrange it before you send details.

A good report has a description of the issue and its impact, the affected
version and server (nginx, Apache or IIS), and the steps or input that
reproduce it. We do not run a bug bounty.

## Scope

The following components are in scope for security reports:

- pagespeed-optimizer daemon and the nginx serving module (image
  optimization, HTML rewriting, cache subsystem, management HTTP API)
- ASP.NET Core middleware and NuGet packages
- Web console (the `/console/` UI)

Reports for the modules for nginx, Apache and IIS (the
[We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed) repository) are
covered by this same policy and address.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Fixes and announcements

Security fixes ship in a regular release, through every channel at the same
time. Nobody receives a fixed build before the public release.

- **Announcements.** Security fixes are announced in the security section of
  the release notes and in the CHANGELOG, with the impact class, the affected
  and fixed versions, and "update recommended". The same entries are in the
  [security updates table](https://modpagespeed.com/docs/release-notes/#security-updates).
  The credit goes in the same entry, if the reporter wants to be credited.
- **CVE ids.** When a CVE id has been assigned for a fixed issue, it is included
  in the entry. Many entries have none; the table says so.
- **Level of detail.** The announcements describe the impact and
  say who should update. They do not describe how to trigger an issue.
- **Machine-readable data.** Each release carries an SPDX SBOM and an OpenVEX
  document; see `sbom/` and the release assets.

## Supported versions

Security fixes are made in the latest release of mod_pagespeed. Upgrading to it
is the supported path from any earlier release. This covers the host packages,
the container images and the NuGet packages.

## Verifying a release

Every release carries `SHA256SUMS`, a PGP signature of it (`SHA256SUMS.asc`)
and, for the host packages, a Sigstore signature (`SHA256SUMS.sigstore.json`)
with build provenance and SBOM attestations. The host and NuGet releases also
carry SLSA build provenance (`multiple.intoto.jsonl`), which `slsa-verifier`
checks. The container images are signed
with keyless cosign. The commands are in [RELEASING.md](RELEASING.md) and in
the [deployment guide](https://modpagespeed.com/docs/deployment/#verify-the-images).

## Disclosure policy

We follow coordinated disclosure. Once a vulnerability is reported:

1. We confirm receipt.
2. We investigate and develop a fix.
3. We coordinate a release with the reporter. If a fix needs more time, we
   agree a date with the reporter.
4. We announce the fix in the release notes when it is released.
5. After the fix is released, the reporter may publish details. We ask that
   users get time to update first.

Findings from automated services follow the service's own disclosure
deadline.

If a vulnerability is being exploited, we do not wait: the fix is released as
soon as it is ready and announced with it.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the release notes.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/pagespeed-optimizer/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
