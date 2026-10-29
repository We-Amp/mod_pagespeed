# Contributing to mod_pagespeed

Thanks for helping. This repository holds the mod_pagespeed web-server
modules for Apache, nginx (the packaged `nginx-module-pagespeed` and the
`ngx_pagespeed` tarballs), IIS and Envoy (experimental, build from source).
The optimization worker (`pagespeed-optimizer`) that the Apache and nginx
modules pair with (nginx through `pagespeed DaemonSocketPath`), the nginx
module in the container images and Helm chart, the management console and
the ASP.NET Core middleware live in
[We-Amp/pagespeed-optimizer](https://github.com/We-Amp/pagespeed-optimizer).

## Reporting bugs and requesting features

Open an issue at <https://github.com/We-Amp/mod_pagespeed/issues> using the
bug report or feature request form. Search existing issues first. A useful bug
report names:

- the version, from the `X-PageSpeed: <version>` response header
  (`curl -s -o /dev/null -D - <url> | grep -i x-pagespeed`) or the package
  version,
- the platform: web server and its version, OS / distribution and
  architecture, and how you installed (signed apt/dnf repository, cPanel
  EasyApache RPM, IIS MSI, GitHub release asset, or built from source; Envoy
  is source-only),
- the smallest configuration excerpt that reproduces it,
- the `X-PageSpeed` header of an affected request and the relevant
  error-log lines.

**Security vulnerabilities:** do not open a public issue. Follow
[SECURITY.md](SECURITY.md).

## Building and testing

All Linux builds run inside the Docker development container (Clang,
GCC 13, Bazel, plus Redis and Memcached for the cache tests). The full
developer guide, including native builds, build configurations and common
problems, is [DEVELOPER.md](DEVELOPER.md).

```bash
docker compose up -d
docker compose exec dev bash

# inside the container
bazel build --config=clang-libstdcxx13 //:libmod_pagespeed.so                   # Apache
bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so # nginx
bazel build --config=envoy-filter //pagespeed/envoy:envoy_pagespeed              # Envoy (experimental)

bazel test --config=clang-libstdcxx13 \
  --test_env=REDIS_PORT=6379 --test_env=REDIS_HOST=redis \
  --test_env=MEMCACHED_PORT=11211 --test_env=MEMCACHED_HOST=memcached \
  //test/pagespeed/kernel/... //test/net/... \
  //test/pagespeed/opt/... //test/pagespeed/system/... \
  //test/pagespeed/apache/... //test/pagespeed/nginx/...

# on the host, when done
docker compose down
```

Use `--config=clang-libstdcxx13` (or `--config=gcc`): the Cyclone cache
sources do not build under plain `--config=clang`. The test targets above are
the ones CI runs.

The IIS module builds on Windows with clang-cl:

```powershell
bazel build --config=windows --config=clang-cl //pagespeed/iis:pagespeed_iis.dll
```

System tests: `./test/system/run_system_tests.sh` (Apache),
`./test/system/run_nginx_tests.sh` (nginx), `./test/system/run_envoy_tests.sh`
(Envoy); see [docs/test-catalog.md](docs/test-catalog.md).

## Code style

- C++20 (C++23 for the Cyclone cache files), Google C++ style, 80 columns.
- Formatting uses the repository's `.clang-format` with `clang-format-20`
  exactly, as CI does. Reformat with `tools/fix-format.sh`.
- Optional: `tools/install-hooks.sh` installs pre-commit and pre-push hooks
  that check formatting before CI does.

## Pull requests

- Branch from `master` and keep each pull request to one change.
- Add or update tests for behaviour changes.
- **Release notes are required.** A pull request that touches `pagespeed/`,
  `net/`, or a shipped `install/**/*.conf` / `*.template` file (outside tests,
  `BUILD` files and `*.md`) must also
  add a changelog fragment, `changelog.d/<branch-slug>.<category>.md`, holding
  the entry exactly as it should read (see
  [changelog.d/README.md](changelog.d/README.md)). Editing
  [CHANGELOG.md](CHANGELOG.md) under `## [Unreleased]` or the in-development
  section of [RELEASE_NOTES.md](RELEASE_NOTES.md) is still accepted for now. If the
  change is invisible to users, put `Release-Note: none - <why>` in a commit
  message or the pull request description (editing the description does not
  re-run the check; re-run the job). Check it locally with
  `bash tools/ci/check_release_note.sh --base origin/master --head HEAD`.
- Pull requests from forks are welcome. They are built on GitHub-hosted
  runners, in the pull request and again in the merge queue, so their checks
  take longer than a branch in this repository. A fork pull request that
  changes `.github/` or `tools/ci/` is re-landed by a maintainer from a branch
  in this repository: maintainers do not queue a fork pull request that
  changes `.github/` or `tools/ci/`.
- Describe the problem, the change and how you verified it.

## Licensing

The project is licensed under the [Apache License 2.0](LICENSE).

## Release history

There is no published roadmap. Changes are recorded in
[CHANGELOG.md](CHANGELOG.md), [RELEASE_NOTES.md](RELEASE_NOTES.md), the
[GitHub releases](https://github.com/We-Amp/mod_pagespeed/releases) and the
[release notes](https://modpagespeed.com/docs/release-notes/) on
modpagespeed.com.
