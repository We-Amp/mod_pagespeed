- **Sigstore signature and build provenance for every release.** `SHA256SUMS`
  now also has a keyless Sigstore signature (`SHA256SUMS.sigstore.json`), every
  module file has a build provenance attestation, and every module package has
  an SBOM attestation, verifiable with `cosign verify-blob` and
  `gh attestation verify`. The PGP signature (`SHA256SUMS.asc`) and the
  existing `sha256sum` and `gpg` checks work as before. `SECURITY.md` lists the
  commands.
