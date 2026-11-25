# SLSA provenance fixture

`generic-v2.1.0.intoto.jsonl` is a provenance file made by the SLSA
project's generic generator v2.1.0 (a Sigstore bundle), copied (with a final newline added) from
the test data of slsa-framework/slsa-verifier
(`cli/slsa-verifier/testdata/gha_generic/v2.1.0/binary-linux-amd64-workflow_dispatch.intoto.jsonl`,
Apache-2.0). `tools/ci/check_slsa_provenance.py --self-test` reads it to
prove the check parses the format the release lanes receive.
