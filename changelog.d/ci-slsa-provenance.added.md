- **Releases carry SLSA build provenance.** Each module release, and each
  nginx.org package release, now includes `multiple.intoto.jsonl`, an SLSA
  build provenance statement for its files that `slsa-verifier` checks;
  SECURITY.md has the command.
