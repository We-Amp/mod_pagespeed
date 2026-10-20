# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in the mod_pagespeed modules for nginx and Apache, please report it
responsibly by emailing **security@modpagespeed.com**. Do NOT open a public GitHub
issue.

We will acknowledge your report within 72 hours and work with you to understand
and address the issue.

## Scope

The mod_pagespeed modules for nginx and Apache in this repository are in scope
for security reports.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Supported Versions

Only the latest release receives security updates. Users on older versions
should upgrade to receive fixes.

## Verifying a Release

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

## Disclosure Policy

We follow a 90-day coordinated disclosure timeline. Once a vulnerability is
reported:

1. We confirm receipt within 72 hours.
2. We investigate and develop a fix.
3. We coordinate a release within 90 days of the initial report.
4. After the fix is released, the reporter may publish details.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the release notes.

## Contact

- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
