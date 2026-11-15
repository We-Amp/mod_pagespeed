# Security policy

## Reporting a vulnerability

If you discover a security vulnerability in the mod_pagespeed modules for nginx
and Apache, please report it privately in one of these ways:

- **GitHub private vulnerability reporting:** open the
  [Security tab](https://github.com/We-Amp/mod_pagespeed/security) of this
  repository and choose **Report a vulnerability**, or go straight to the
  [report form](https://github.com/We-Amp/mod_pagespeed/security/advisories/new).
- **Email:** **security@modpagespeed.com**

Please do not open a public issue or pull request for a vulnerability. Both
are visible to everyone before a fix is available.

We will acknowledge your report within 72 hours and work with you to understand
and address the issue.

## Scope

The mod_pagespeed modules for nginx and Apache in this repository are in scope
for security reports.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Fixes and advisories

Security fixes ship in a release. Each release that fixes a vulnerability comes
with a published
[security advisory](https://github.com/We-Amp/mod_pagespeed/security/advisories)
that lists the impact and the affected and fixed versions, and credits the
reporter if they want to be credited.

## Supported versions

Only the latest release receives security updates. Users on older versions
should upgrade to receive fixes.

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

Replace `<version>` with the release version. The Sigstore bundle is in the
current bundle format: use cosign v2.4 or newer.

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

We follow a 90-day coordinated disclosure timeline. Once a vulnerability is
reported:

1. We confirm receipt within 72 hours.
2. We investigate and develop a fix.
3. We coordinate a release within 90 days of the initial report.
4. We publish a security advisory when the fix is released.
5. After the fix is released, the reporter may publish details.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the security advisory and the release notes.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/mod_pagespeed/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
