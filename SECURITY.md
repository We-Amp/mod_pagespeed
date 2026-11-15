# Security policy

## Reporting a vulnerability

If you discover a security vulnerability in the mod_pagespeed modules for
nginx, Apache and IIS, please report it privately in one of these ways:

- **GitHub private vulnerability reporting:** open the
  [Security tab](https://github.com/We-Amp/mod_pagespeed/security) of this
  repository and choose **Report a vulnerability**, or go straight to the
  [report form](https://github.com/We-Amp/mod_pagespeed/security/advisories/new).
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

The mod_pagespeed modules for nginx, Apache and IIS in this repository are in
scope for security reports. The optimizer worker has its own repository,
[We-Amp/pagespeed-optimizer](https://github.com/We-Amp/pagespeed-optimizer),
with the same policy and address.

The Envoy filter is experimental and built from source only. Reports for it
are welcome, but it is not a release target and is not covered by the release
security statements.

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

Security fixes are made in the latest release. Upgrading to it is the supported
path from any earlier release, including 1.x; the [upgrade guide](https://modpagespeed.com/docs/upgrade/)
lists the few settings to check.

## Verifying a release

Every GitHub release from 2.5.0 on carries `SHA256SUMS`, which lists every
file of the release, and `SHA256SUMS.asc`, a detached signature of it made
with the release signing key:

- Key: `RELEASE-SIGNING-KEY.asc` in the root of this repository
- Fingerprint: `411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313`
- User ID: We-Amp B.V. (Release Attestation) <packages@we-amp.com>

First check the key file's fingerprint against the one above:

```
gpg --show-keys RELEASE-SIGNING-KEY.asc
```

Then, in the directory holding the downloaded files, verify the signature
against that key alone (not against whatever else is in your keyring), and
check the files only if the signature is good:

```
gpg --dearmor < RELEASE-SIGNING-KEY.asc > release.gpg
gpgv --keyring ./release.gpg SHA256SUMS.asc SHA256SUMS \
  && sha256sum --check --ignore-missing SHA256SUMS
```

On macOS, use `shasum -a 256 --check --ignore-missing SHA256SUMS` for the
last step.

The Windows installer and binaries are also Authenticode signed by
We-Amp B.V. From v2.5.0 on, the source tags `v2.*` are signed with the same
key. The apt and yum repositories are signed by a separate repository key.

### Sigstore signature and build provenance

A release that carries `SHA256SUMS.sigstore.json` also has a keyless Sigstore
signature of `SHA256SUMS`, a build provenance attestation (SLSA v1) for every
module file, `SHA256SUMS` and `manifest.json`, and an SBOM attestation for
every module package. They are in addition to the PGP signature above, which
stays the primary check. The signer is the release workflow on `master`, and
the source ref is the release tag. With
[cosign](https://docs.sigstore.dev/cosign/system_config/installation/):

```
cosign verify-blob SHA256SUMS --bundle SHA256SUMS.sigstore.json \
  --certificate-identity 'https://github.com/We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@refs/heads/master' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-github-workflow-ref refs/tags/module-v<version>
```

With the [GitHub CLI](https://cli.github.com/), for any module file:

```
gh attestation verify <file> --repo We-Amp/mod_pagespeed \
  --cert-identity 'https://github.com/We-Amp/mod_pagespeed/.github/workflows/pipeline-release-module.yml@refs/heads/master' \
  --source-ref refs/tags/module-v<version>
```

Add `--predicate-type https://spdx.dev/Document/v2.3` to check the SBOM
attestation of a package. The `pagespeed-optimizer` packages in the release
are attested in the
[pagespeed-optimizer](https://github.com/We-Amp/pagespeed-optimizer)
repository; verify them with the commands of its `RELEASING.md`.

## Disclosure policy

We follow coordinated disclosure. Once a vulnerability is reported:

1. We confirm receipt.
2. We investigate and develop a fix.
3. We coordinate a release with the reporter. If a fix needs more time, we
   agree a date with the reporter.
4. We announce the fix in the release notes when it is released.
5. After the fix is released, the reporter may publish details. We ask that
   users get time to update first.

Findings from automated services follow the service's own deadline. For
example, [OSS-Fuzz's own policy](https://google.github.io/oss-fuzz/getting-started/bug-disclosure-guidelines/)
makes its findings public 90 days after they are reported, or when the fix is
released, whichever comes first.

If a vulnerability is being exploited, we do not wait: the fix is released as
soon as it is ready and announced with it.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the release notes.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/mod_pagespeed/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
