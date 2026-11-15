# Releasing mod_pagespeed

Releases of the pagespeed-optimizer host packages are cut with the `Release`
workflow (`release.yml`). The lane builds the optimization daemon and the
`libpagespeed.so` client library natively on amd64 and arm64, packages them
as deb and rpm, validates the result fail-closed, and publishes a GitHub
release. The amd64 package and the Windows zip build on the
project's own runner classes (`fleet-heavy`, `fleet-windows`) when the
`FLEET_ENABLED` repository variable is `true`, and otherwise on a GitHub-hosted
twin job with identical steps; arm64 always builds on a GitHub-hosted arm64
runner, and every job that writes to GitHub runs hosted.

## Every user-facing change carries its note (enforced)

Release notes are not written at release time. They accumulate, one pull
request at a time, and CI enforces that.

`tools/ci/check_release_note.sh` runs on every pull request. If the diff
touches `src/` or `lib/` — excluding tests and build files — it requires
that the same pull request also adds a note: a fragment file
`changelog.d/<slug>.<category>.md` (see `changelog.d/README.md`; the release
cut folds fragments into `CHANGELOG.md`), or, during the transition, an edit
of `CHANGELOG.md`. Only a fragment the pull request adds or modifies counts;
deleting one does not. Fragment text is also scanned for security entries
(see "Security entries" in `changelog.d/README.md`). Write it for someone deciding whether to upgrade: what they
observed before, what they observe now.

If the change genuinely cannot be observed by a user, say so in one line:

```
Release-Note: none — <why this is invisible to users>
```

Put it in a commit message (pushing re-runs the check) or in the pull request
description (editing a description does not re-trigger CI, so re-run the job).
The reason is mandatory — the waiver records a judgement, it is not a mute
button.

Run it yourself before pushing:

```bash
bash tools/ci/check_release_note.sh --base origin/main --head HEAD
bash tools/ci/check_release_note.sh --self-test   # the gate's own tests
```

The gate exists because the failure it prevents is silent. A fix that ships
without a note leaves everyone still working around it, and nothing anywhere
goes red to say so.

## Version numbering: VERSION.txt vs the shipped daemon

`VERSION.txt` carries the product version, and from 2.5.0 the optimizer
release tag carries the same number: `optimizer-v<X.Y.Z>` must equal
`VERSION.txt` at the tagged commit (`tools/ci/check_optimizer_tag_version.sh`,
run by the lane). The shipped daemon reports `VERSION.txt` and the source
commit in `--version` and in `/v1/health`, so `optimizer-v2.5.2` reports
2.5.2. The host packages `optimizer-v1.16.0` to `optimizer-v1.19.0` were
numbered on the module's 1.x line (built from `VERSION.txt` 2.1.0 to 2.4.0);
those tags still verify, with a notice. The cut order is: bump `VERSION.txt` on
`main`, tag `optimizer-v<X>` on that commit, record the pin in the module
repository, then tag `module-v<X>` there; `docs/ci/release.md` has the details.

The notes join the cut commit: in the pull request that bumps `VERSION.txt`,
run `python3 -I tools/changelog/assemble.py` (it folds `changelog.d/` into
`## Unreleased` and removes the fragments), then rename `## Unreleased` to
`## <X.Y.Z> - <date>` and add a fresh empty `## Unreleased` above it. Security
paragraphs arrive in `CHANGELOG.md` with that fold, so the cut pull request
comes from a `landing/<X.Y.Z>` branch with `Release-Note: landing -- <why>`,
as the release-note gate already requires for any Security entry.

The optimizer tag lane (`pipeline-release-optimizer.yml`, step "Verify the
release commit holds no unfolded changelog fragments") fails a tag whose
commit still has anything in `changelog.d/` besides `README.md`; for a
prerelease tag, and for a dry run of a tag that does not exist yet, it only
warns. A tag from before `changelog.d/` existed has no directory and passes.
Nothing else in the tag lanes reads the changelog. The module's own lanes
live in its repository and are not covered here.

### Converting a pull request that already edits CHANGELOG.md

`to_fragment.py` restores `CHANGELOG.md` to the merge-base, so run it on the
branch BEFORE rebasing onto the main that carries the fragments, never after
resolving a `CHANGELOG.md` conflict:

1. Commit your work. The tool finds the repository from its own location,
   so a branch without it gets it in place, untracked:
   `mkdir -p tools/changelog && for f in assemble to_fragment; do git show
   origin/main:tools/changelog/$f.py > tools/changelog/$f.py; done`.
2. `python3 -I tools/changelog/to_fragment.py` (base defaults to
   `origin/main`). It moves the
   lines you added under `## Unreleased` into fragments named after your
   branch and puts `CHANGELOG.md` back. It exits 2 when the edit removes
   lines or touches text outside `## Unreleased`; convert such an edit by
   hand.
3. `rm -r tools/changelog` if you added it in step 1 (a leftover untracked
   copy blocks the rebase), `git add changelog.d && git commit`, then
   `git rebase origin/main`. No
   `CHANGELOG.md` conflict is left to resolve.
4. `python3 -I tools/changelog/assemble.py --lint`, then push once.

If you already rebased and hit the conflict, abort the rebase, run step 2 on
the pre-rebase branch, then rebase. A pull request that only carries a
`Release-Note: none` waiver needs nothing.

## Cutting a release

A release is a tag push. The tag names the version and the lane builds from
exactly that commit:

```
optimizer-v<X.Y.Z>          e.g. optimizer-v2.5.2
optimizer-v<X.Y.Z>-<pre>    e.g. optimizer-v2.5.3-rc.1 (a prerelease)
```

The lane never creates the tag. Pushing `optimizer-v<version>` to the
repository is the releaser's act, and a real run refuses to start unless the
tag already exists on the remote; if the tag moved between resolution and
checkout, the run refuses rather than package the wrong tree under the tag's
name.

Two ways to start a run:

- **Push an `optimizer-v*` tag** — a real run: build, validate, publish.
- **Dispatch the workflow by hand** with inputs `tag` and `dry_run`:
  - `dry_run: true` (the default) builds the four packages and the Windows
    zip, validates the asset set and SHA256SUMS fail-closed, and uploads the
    result as a workflow artifact. It creates NO release and NO tag, and it
    may name a tag that does not exist yet (it then builds the dispatched
    ref). This is the rehearsal — run it before every real release.
  - `dry_run: false` is a real run and requires the tag on the remote.

Before assembling anything, the lane requires a green `ci.yml` run of the
release commit (`docs/ci/release.md`, "The CI gate"). A push run of `ci.yml`
on the release commit does not satisfy it, because a push to `main` skips
`full / Lint (whole tree)`: dispatch `ci.yml` on `main` with `full=true` while
the release commit is the head of `main`. The gate accepts only runs on `main`.

Runs are serialized per tag and never cancelled in progress: two real
releases for the same tag cannot race, and a release build always runs out.

### The release asset contract

The lane builds and validates eight files (the manifest job checks them as a
set), and a real run publishes ten: the eight, plus the two signatures that
the `sign-checksums` job makes over `SHA256SUMS`. Exactly these, no more and
no fewer:

| Asset | Contents |
|---|---|
| `pagespeed-optimizer-<version>-1.x86_64.rpm` | daemon + client library, rpm hosts |
| `pagespeed-optimizer-<version>-1.aarch64.rpm` | the same, arm64 |
| `pagespeed-optimizer_<version>_amd64.deb` | the same, deb hosts |
| `pagespeed-optimizer_<version>_arm64.deb` | the same, arm64 |
| `pagespeed-optimizer-<version>-win-x64.zip` | worker + client library for Windows x64 (`factory_worker.exe`, `pagespeed.dll`, `LICENSE`, `NOTICE`) |
| `pagespeed-optimizer-<version>.spdx.json` | software bill of materials (SPDX 2.3) |
| `pagespeed-optimizer-<version>.vex.json` | known vulnerabilities in bundled components that do not affect these packages, with the reason (OpenVEX) |
| `SHA256SUMS` | checksums for the other seven files |
| `SHA256SUMS.asc` | PGP signature over `SHA256SUMS` (release signing key, `RELEASE-SIGNING-KEY.asc`) |
| `SHA256SUMS.sigstore.json` | Sigstore signature over `SHA256SUMS` (keyless cosign bundle) |

The serving-module packages download these assets by name and verify them
against SHA256SUMS, so the set and the names are a contract. Asset names
carry the upstream SemVer spelling (`2.5.3-rc.1`); the package-internal
version keeps the dpkg/rpm tilde pre-release form (`2.5.3~rc.1`). Both are
derived from the tag, never hand-typed. A `-<pre>` tag produces a release
marked as a prerelease.

Validation is fail-closed at two points. The manifest job checks that the
set is exactly the eight files, all present and non-empty, that
`sha256sum -c` passes and `SHA256SUMS` lists seven files, that the SBOM and
VEX file are well-formed, and that the package-internal versions are the
derived tilde form. The release job re-verifies SHA256SUMS across the job
boundary before publishing anything. The release job also refuses to publish
without a non-empty `SHA256SUMS.asc` and `SHA256SUMS.sigstore.json`.

Every real run also records a build provenance attestation for the five
packages, the SBOM and the VEX file, and an SBOM attestation for each
package, in GitHub's attestation store (docs/ci/release.md, "Sigstore and
attestations"). A dry run signs and attests nothing.

#### Verifying a release

Download the assets of the release, then, from the download directory:

```bash
# Checksums (works on any Linux host; Windows has Get-FileHash)
sha256sum --check --ignore-missing SHA256SUMS

# PGP signature on SHA256SUMS, release key fingerprint
# 411D EFDB 5713 51D2 B25A EFCC 9C02 B5BD CCE7 5313
gpg --import RELEASE-SIGNING-KEY.asc
gpg --verify SHA256SUMS.asc SHA256SUMS

# Sigstore signature on SHA256SUMS (needs cosign)
cosign verify-blob SHA256SUMS --bundle SHA256SUMS.sigstore.json \
  --certificate-identity 'https://github.com/We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-optimizer.yml@refs/heads/main' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-github-workflow-ref refs/tags/optimizer-v<version>

# Build provenance and SBOM attestations of one package (needs the GitHub CLI)
gh attestation verify <package file> --repo We-Amp/pagespeed-optimizer \
  --cert-identity 'https://github.com/We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-optimizer.yml@refs/heads/main' \
  --source-ref refs/tags/optimizer-v<version>
gh attestation verify <package file> --repo We-Amp/pagespeed-optimizer \
  --cert-identity 'https://github.com/We-Amp/pagespeed-optimizer/.github/workflows/pipeline-release-optimizer.yml@refs/heads/main' \
  --source-ref refs/tags/optimizer-v<version> \
  --predicate-type https://spdx.dev/Document/v2.3
```

Replace `<version>` with the release version. The Sigstore bundle is in the
current bundle format: use cosign v2.4 or newer. The PGP check and the
checksums need nothing beyond `gpg` and `sha256sum`.

### The glibc floor gate

The daemon binary is built inside the jammy-based development image
(`docker/Dockerfile`), never on the runner host. Jammy's static libc++
archives keep the binary at a GLIBC_2.34 floor — the RHEL 9 / Debian 12
install contract. A build on a newer base advertises a higher floor
(GLIBC_2.38) and the resulting packages refuse to install on those hosts,
so the manifest job asserts the floor mechanically: every deb must declare
exactly `libc6 (>= 2.34)`, and every rpm must require `glibc >= 2.34` with
nothing above it.

Both legs are native builds (amd64 and arm64 each on their own runner arch)
with clang-20 + libc++-20; the release build adds optimization, LTO and
binary hardening, and `tools/assert-binary-hardening.sh` re-checks the
hardening properties on both artifacts before they are packaged.

## The website tree

`website/` (the modpagespeed.com site) is maintained in this repository and is
the site's source of truth: a website change is an ordinary pull request here,
and `website/CLAUDE.md` is the guide to the tree. The `Website` workflow gates
every push or pull request touching `website/**`: the content guards (no
version-pinned URLs in non-frozen content; the release manifest does not lag the
latest stable release tag), `npm ci`, `npm run format:check`, the vitest unit suite, a production build
(with `PRICING_ALLOW_STALE=1` — a hosted build holds no pricing API credentials,
so it builds on the committed pricing file), the build-output vitest suite, the
built-output content lint (`npm run check:content`: titles, descriptions,
headings and product naming on every built page), a check that the generated
reference data matches the pinned module source, the Playwright browser suite,
and Lighthouse budgets.
Deploying the site is the operator's step, from this tree; nothing in CI deploys.

## Publishing the Helm chart

The chart source is `deploy/helm/pagespeed/`; the chart repository customers add
(`helm repo add weamp https://modpagespeed.com/charts`) is the static directory
`website/public/charts/`. To publish a chart version: bump `version` (and
`appVersion` when the product version moved) in `deploy/helm/pagespeed/Chart.yaml`,
run `deploy/helm/package-chart.sh` (needs the `helm` CLI; `HELM=/path/to/helm`
overrides), and commit the packaged `pagespeed-<version>.tgz` together with the
regenerated `website/public/charts/index.yaml` in one pull request. When
`appVersion` moves, the Compose defaults move in the same pull request: both
`${PAGESPEED_VERSION:-X}` image tags in `deploy/docker-compose.yml` and the
`# PAGESPEED_VERSION=X` line in `deploy/.env.example` (the worker and nginx
images publish no `latest` tag, so a stale default fails to pull);
`tools/ci/check_helm.sh` fails CI when they disagree with `appVersion`. The
website side of the same bump is driven by
`website/src/content/releases-2.1/release.yaml`: the `Website` workflow's
version-drift guard names any docs literal left behind (`deployment.mdx`,
`installation-docker.md`, `rsl-cap.md`, `web-bot-auth.md`), and the vitest
content-accuracy gate covers `website/public/docker-compose.yml`. The script
merges into the existing index, so earlier entries keep their timestamps. Merging
publishes nothing by itself; the chart is live once the site is deployed.

## Vulnerability Scanning

```bash
# Scan source dependencies (VEX suppressions applied)
grype sbom:sbom/mod_pagespeed-2.1.spdx.json --vex sbom/mod_pagespeed-2.1.vex.json
```

### Vulnerability SLA

| Severity | Timeline |
|----------|----------|
| Critical/High | Patch or mitigate within 30 days |
| Medium | Within 90 days |
| Low | Next scheduled release |

## SBOM Maintenance

When adding or updating dependencies:
1. Update `MODULE.bazel` (C++), `Cargo.toml` (Rust), or `package.json` (JS)
2. Update `THIRD-PARTY-NOTICES` with new dependency info: name, built version, license identifier, copyright holder(s) as they appear in the component's license file, and a link to that file at the pinned tag or commit
3. Run `tools/generate-sbom.py` to regenerate SBOMs (the Rust and JavaScript rows are curated tables in the script; keep them at the lockfile versions)
4. Verify: `tools/generate-sbom.py --verify`
5. Verify: `tools/sbom/check-third-party-notices.py --links`

## Annual Review

- Update copyright year range in license headers
- Review for new compliance requirements
- Audit dependency licenses for any changes
